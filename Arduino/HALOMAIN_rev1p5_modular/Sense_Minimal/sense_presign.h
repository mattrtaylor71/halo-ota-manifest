/*
 * sense_presign.h
 *
 * Presign URL request functions for dish, discard, and checkin uploads.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 12.
 *
 * Prerequisites (must be declared before #include "sense_presign.h"):
 *   - ArduinoJson.h
 *   - PresignReply struct, UploadJob::CameraUploadMeta struct
 *   - diag_note_stage(), diag_record_error() from sense_diag.h
 *   - presign_set_error_text(), http_post_json_with_retries() from sense_upload.h
 *   - append_camera_meta_json(), log_camera_meta_for_presign() from sense_camera.h
 *   - load_owner_id_or_default(), load_runtime_device_id()
 *   - API_KEY, BEARER_TOKEN, current_job (OpJob)
 *   - CHECKIN_API_BASE_URL, CHECKIN_PRESIGN_ENDPOINT,
 *     DISH_PRESIGN_URL, DISCARD_PRESIGN_ENDPOINT
 */

#ifndef SENSE_PRESIGN_H
#define SENSE_PRESIGN_H

// Forward declaration for .ino function called by get_presign
static bool scan_mode_is_discard(const char* mode);

// ── Simple presign request ─────────────────────────────────────────

static bool do_presign_request_simple(const char* url,
                                      const char* type,
                                      PresignReply& out,
                                      int& http_code,
                                      String& resp_body,
                                      uint32_t deadline_ms) {
  diag_note_stage("presign", 0);
  StaticJsonDocument<256> doc;
  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));
  if (owner_id[0] == '\0') {
    presign_set_error_text("Owner not set");
    Serial.println("[PRESIGN] owner_id empty -> abort");
    diag_record_error("presign", -1, "owner_missing");
    return false;
  }
  doc["device_id"] = device_id;
  doc["user_id"] = owner_id;
  doc["type"] = type ? type : "";
  String body;
  serializeJson(doc, body);

  // Piggyback undelivered error telemetry
  char err_buf[512];
  uint32_t err_seq = sense_errlog_collect_json(err_buf, sizeof(err_buf), 3);
  if (err_seq > 0 && body.length() > 1) {
    body.remove(body.length() - 1);
    body += ",\"errors\":";
    body += err_buf;
    body += "}";
    Serial.printf("[PRESIGN] piggyback %s (seq<=%lu)\n", err_buf, (unsigned long)err_seq);
  }

  Serial.printf("[PRESIGN] Using job_type: %s\n", type ? type : "");
  Serial.println("[PRESIGN] POST " + String(url ? url : ""));
  if (!http_post_json_with_retries(url,
                                   body,
                                   http_code,
                                   resp_body,
                                   "PRESIGN",
                                   API_KEY,
                                   BEARER_TOKEN,
                                   current_job.job_id,
                                   deadline_ms)) {
    Serial.printf("[PRESIGN] Request failed with code %d\n", http_code);
    diag_record_error("presign_http", http_code, "request_failed");
    return false;
  }
  Serial.printf("[PRESIGN] HTTP %d\n", http_code);
  if (resp_body.length()) Serial.println("[PRESIGN] Body: " + resp_body);

  StaticJsonDocument<768> r;
  auto err = deserializeJson(r, resp_body);
  if (err) {
    Serial.print("[PRESIGN] JSON parse error: ");
    Serial.println(err.c_str());
    diag_record_error("presign_parse", -1, "json_parse");
    return false;
  }

  out.job_id       = r["job_id"].as<String>();
  out.put_url      = r["put_url"].as<String>();
  out.s3_key       = r["s3_key"].as<String>();
  out.content_type = r["content_type"].as<String>();
  out.result_url   = r["result_url"].as<String>();
  if (out.content_type.isEmpty() || out.content_type == "null") {
    out.content_type = "image/jpeg";
  }
  if (out.result_url == "null") {
    out.result_url = "";
  }
  out.ttl_s        = r["ttl_s"] | 0;

  if (out.job_id.isEmpty() || out.put_url.isEmpty()) {
    diag_record_error("presign_parse", -1, "missing_fields");
    return false;
  }
  return true;
}

// ── Full presign request with action/owner/expiry ──────────────────

static bool do_presign_request(const char* base_url,
                               const char* endpoint,
                               const char* type,
                               const char* action,
                               const char* owner,
                               const char* expiry_date,
                               bool add_to_shopping_list,
                               const UploadJob::CameraUploadMeta* camera_meta,
                               PresignReply& out,
                               int& http_code,
                               String& resp_body,
                               uint32_t deadline_ms) {
  diag_note_stage("presign", 0);
  String presign_url = String(base_url) + String(endpoint ? endpoint : "");
  StaticJsonDocument<512> doc;
  char owner_id[64] = {0};
  char device_id[32] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  load_runtime_device_id(device_id, sizeof(device_id));
  if (owner_id[0] == '\0') {
    presign_set_error_text("Owner not set");
    Serial.println("[PRESIGN] owner_id empty -> abort");
    diag_record_error("presign", -1, "owner_missing");
    return false;
  }
  doc["user_id"] = owner_id;
  doc["device_id"] = device_id;
  doc["owner"] = owner ? owner : "";
  doc["action"] = action ? action : "";
  doc["type"] = type ? type : "";
  doc["content_type"] = "image/jpeg";
  if (expiry_date && expiry_date[0]) {
    doc["product_expiration"] = expiry_date;
  }
  if (type && strcmp(type, "discard") == 0) {
    doc["add_to_shopping_list"] = add_to_shopping_list;
  }
  if (camera_meta) {
    append_camera_meta_json(doc, *camera_meta);
  }
  String body;
  serializeJson(doc, body);

  // Piggyback undelivered error telemetry on presign request.
  // Append errors JSON array to the serialized body string to avoid
  // bumping StaticJsonDocument size.
  char err_buf[512];
  uint32_t err_seq = sense_errlog_collect_json(err_buf, sizeof(err_buf), 3);
  if (err_seq > 0 && body.length() > 1) {
    // Replace trailing "}" with ","errors":[...]}"
    body.remove(body.length() - 1);  // remove '}'
    body += ",\"errors\":";
    body += err_buf;
    body += "}";
    Serial.printf("[PRESIGN] piggyback %s (seq<=%lu)\n", err_buf, (unsigned long)err_seq);
  }

  Serial.printf("[PRESIGN] Using type=%s action=%s\n",
                type ? type : "",
                action ? action : "");
  log_camera_meta_for_presign("PRESIGN", camera_meta);
  Serial.println("[PRESIGN] POST " + presign_url);
  if (!http_post_json_with_retries(presign_url.c_str(),
                                   body,
                                   http_code,
                                   resp_body,
                                   "PRESIGN",
                                   API_KEY,
                                   BEARER_TOKEN,
                                   current_job.job_id,
                                   deadline_ms)) {
    Serial.printf("[PRESIGN] Request failed with code %d\n", http_code);
    diag_record_error("presign_http", http_code, "request_failed");
    return false;
  }
  Serial.printf("[PRESIGN] HTTP %d\n", http_code);
  if (resp_body.length()) Serial.println("[PRESIGN] Body: " + resp_body);

  StaticJsonDocument<768> r;
  auto err = deserializeJson(r, resp_body);
  if (err) {
    Serial.print("[PRESIGN] JSON parse error: ");
    Serial.println(err.c_str());
    diag_record_error("presign_parse", -1, "json_parse");
    return false;
  }

  out.job_id       = r["job_id"].as<String>();
  out.put_url      = r["put_url"].as<String>();
  out.s3_key       = r["s3_key"].as<String>();
  out.content_type = r["content_type"].as<String>();
  out.result_url   = r["result_url"].as<String>();
  if (out.content_type.isEmpty() || out.content_type == "null") {
    out.content_type = "image/jpeg";
  }
  if (out.result_url == "null") {
    out.result_url = "";
  }
  out.ttl_s        = r["ttl_s"] | 0;

  if (out.job_id.isEmpty() || out.put_url.isEmpty()) {
    diag_record_error("presign_parse", -1, "missing_fields");
    return false;
  }
  return true;
}

// ── Mode-aware presign dispatch ────────────────────────────────────

static bool get_presign(PresignReply& out,
                        const char* mode,
                        const char* expiry_date,
                        bool add_to_shopping_list,
                        const UploadJob::CameraUploadMeta* camera_meta,
                        uint32_t deadline_ms) {
  const bool is_discard = scan_mode_is_discard(mode);
  int code = 0;
  String body;
  char owner_id[64] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  if (is_discard) {
    if (do_presign_request(CHECKIN_API_BASE_URL,
                           CHECKIN_PRESIGN_ENDPOINT,
                           "discard",
                           "IN",
                           owner_id,
                           expiry_date,
                           add_to_shopping_list,
                           camera_meta,
                           out,
                           code,
                           body,
                           deadline_ms)) {
      Serial.println("[PRESIGN] Presign OK");
      sense_errlog_mark_delivered(g_errlog_last_collect_seq);
      return true;
    }
    Serial.printf("[PRESIGN] Presign failed (primary): %d\n", code);
    if (do_presign_request(CHECKIN_API_BASE_URL,
                           DISCARD_PRESIGN_ENDPOINT,
                           "discard",
                           "IN",
                           owner_id,
                           expiry_date,
                           add_to_shopping_list,
                           camera_meta,
                           out,
                           code,
                           body,
                           deadline_ms)) {
      Serial.println("[PRESIGN] Presign OK (fallback)");
      sense_errlog_mark_delivered(g_errlog_last_collect_seq);
      return true;
    }
    Serial.printf("[PRESIGN] Presign failed (fallback): %d\n", code);
    diag_record_error("presign_http", code, "discard_failed");
    return false;
  }
  if (do_presign_request(DISH_PRESIGN_URL,
                         "",
                         "dish",
                         "IN",
                         owner_id,
                         NULL,
                         false,
                         camera_meta,
                         out,
                         code,
                         body,
                         deadline_ms)) {
    Serial.println("[PRESIGN] Presign OK");
    sense_errlog_mark_delivered(g_errlog_last_collect_seq);
    return true;
  }
  Serial.printf("[PRESIGN] Presign failed: %d\n", code);
  diag_record_error("presign_http", code, "dish_failed");
  return false;
}

#endif // SENSE_PRESIGN_H
