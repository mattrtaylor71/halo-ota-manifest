/*
 * sense_list.h
 *
 * Shopping list API operations: parse/update list from JSON,
 * delete item, and fetch list from backend.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 11.
 *
 * Prerequisites (must be declared before #include "sense_list.h"):
 *   - WiFi.h, WiFiClientSecure.h, HTTPClient.h, ArduinoJson.h
 *   - freertos/semphr.h, freertos/task.h
 *   - TREPO_API_BASE_URL, TREPO_LIST_ENDPOINT, TREPO_DEVICE_ID
 *   - load_owner_id_or_default()
 *   - extract_host_from_url(), ensure_dns_ready() from sense_http.h
 *   - g_list_mutex, g_shopping_list, g_list_count, g_selected_index
 *   - MAX_LIST_ITEMS, MAX_ITEM_LENGTH
 *   - g_last_refresh_reason
 *   - uart_send_ui_status() (forward declared)
 */

#ifndef SENSE_LIST_H
#define SENSE_LIST_H

// Forward declarations for .ino functions called by list API code
static void uart_send_ui_list();
static void list_refresh_mark_complete(const char* reason);

// ── List refresh failure ───────────────────────────────────────────

static void list_refresh_fail(const char* reason) {
  Serial.printf("[LIST_REFRESH] fail reason=%s\n", reason ? reason : "unknown");
  uart_send_ui_status("IDLE");
}

// ── Parse and update shopping list ─────────────────────────────────

static bool parse_and_update_shopping_list(const String& json_response) {
  Serial.println("\n=== Parsing Shopping List from JSON ===");

  DynamicJsonDocument doc(32768);
  DeserializationError error = deserializeJson(doc, json_response);

  if (error) {
    Serial.print("✗ JSON parse error: ");
    Serial.println(error.c_str());
    return false;
  }

  if (!doc.containsKey("items") || !doc["items"].is<JsonArray>()) {
    Serial.println("✗ No 'items' array in response");
    return false;
  }

  JsonArray items = doc["items"].as<JsonArray>();
  int item_count = items.size();
  Serial.print("Found ");
  Serial.print(item_count);
  Serial.println(" items in response");

  if (g_list_mutex == NULL) {
    Serial.println("✗ List mutex not initialized!");
    return false;
  }

  if (xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println("✗ Failed to acquire list mutex");
    return false;
  }

  // Clear existing list
  g_list_count = 0;
  g_selected_index = -1;

  // Parse items
  int actual_count = 0;
  for (JsonObject item : items) {
    if (actual_count >= MAX_LIST_ITEMS) break;

    // Skip checked items
    const char* action = item["action"] | "";
    if (strcmp(action, "CHECKED") == 0) continue;

    // Extract product_name
    const char* product_name = item["product_name"] | "";
    if (product_name == NULL || strlen(product_name) == 0) continue;

    // Copy product name
    size_t name_len = strlen(product_name);
    if (name_len >= MAX_ITEM_LENGTH) name_len = MAX_ITEM_LENGTH - 1;
    strncpy(g_shopping_list[actual_count].text, product_name, name_len);
    g_shopping_list[actual_count].text[name_len] = '\0';

    // Extract item ID
    const char* item_id_str = NULL;
    if (item.containsKey("id")) {
      if (item["id"].is<const char*>()) {
        item_id_str = item["id"].as<const char*>();
      } else if (item["id"].is<int>()) {
        int item_id_int = item["id"].as<int>();
        char id_buf[32];
        snprintf(id_buf, sizeof(id_buf), "%d", item_id_int);
        item_id_str = id_buf;
      }
    }

    if (item_id_str != NULL && strlen(item_id_str) > 0) {
      size_t id_len = strlen(item_id_str);
      if (id_len >= MAX_ITEM_LENGTH) id_len = MAX_ITEM_LENGTH - 1;
      strncpy(g_shopping_list[actual_count].id, item_id_str, id_len);
      g_shopping_list[actual_count].id[id_len] = '\0';
    } else {
      g_shopping_list[actual_count].id[0] = '\0';
    }

    actual_count++;
  }

  g_list_count = actual_count;
  g_selected_index = (actual_count > 0) ? 0 : -1;

  xSemaphoreGive(g_list_mutex);

  Serial.print("✓ Parsed ");
  Serial.print(actual_count);
  Serial.println(" items");

  // Send updated list to LCD
  uart_send_ui_list();

  // Send IDLE status
  delay(50);
  uart_send_ui_status("IDLE");
  return true;
}

// ── Delete item from API ───────────────────────────────────────────

static void delete_item_from_api(const char* item_id) {
  if (item_id == NULL || strlen(item_id) == 0) {
    Serial.println("✗ Cannot delete item: invalid ID!");
    return;
  }

  Serial.printf("\n=== Deleting Item ID: %s ===\n", item_id);

  // Build JSON request body for remove operation
  char owner_id[64] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  String request_body = "{";
  request_body += "\"operation\":\"remove\",";
  request_body += "\"ownerId\":\"" + String(owner_id) + "\",";
  request_body += "\"id\":\"" + String(item_id) + "\"";
  request_body += "}";

  // Create HTTPS client
  WiFiClientSecure client;
  HTTPClient http;
  client.setInsecure();

  String list_url = String(TREPO_API_BASE_URL) + String(TREPO_LIST_ENDPOINT);
  http.begin(client, list_url);
  http.setTimeout(30000);
  http.setConnectTimeout(10000);
  http.addHeader("Content-Type", "application/json");

  Serial.print("URL: ");
  Serial.println(list_url);
  Serial.print("Request body: ");
  Serial.println(request_body);

  int httpResponseCode = http.POST(request_body);
  Serial.print("HTTP Response code: ");
  Serial.println(httpResponseCode);

  String response_json = "";
  if (httpResponseCode > 0) {
    response_json = http.getString();
    Serial.print("Response length: ");
    Serial.println(response_json.length());
  }

  http.end();
  client.stop();

  if (httpResponseCode == 200) {
    Serial.println("✓ Item deleted successfully from backend");
    uart_send_ui_status("Item deleted");
  } else {
    Serial.printf("✗ Delete failed with HTTP code: %d\n", httpResponseCode);
    Serial.println("[DELETE] fail -> UI idle");
    uart_send_ui_status("IDLE");
  }
}

// ── Fetch shopping list from API ───────────────────────────────────

static void fetch_shopping_list_from_api() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("✗ Cannot fetch list: WiFi not connected!");
    list_refresh_fail("wifi_not_connected");
    return;
  }

  Serial.println("\n=== Fetching Shopping List from Trepo API ===");

  // Build JSON request body
  char owner_id[64] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  String request_body = "{";
  request_body += "\"operation\":\"view\",";
  request_body += "\"ownerId\":\"" + String(owner_id) + "\",";
  request_body += "\"device\":\"" + String(TREPO_DEVICE_ID) + "\"";
  request_body += "}";

  // Build full URL
  String list_url = String(TREPO_API_BASE_URL) + String(TREPO_LIST_ENDPOINT);
  String list_host;
  if (!extract_host_from_url(list_url, list_host)) {
    Serial.println("✗ List fetch: Failed to parse host from URL");
    list_refresh_fail("host_parse_fail");
    return;
  }
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  IPAddress ip = WiFi.localIP();
  bool ip_ok = (static_cast<uint32_t>(ip) != 0);
  if (!wifi_ok || !ip_ok) {
    Serial.println("[NET] not ready - skipping list fetch");
    list_refresh_fail("net_not_ready");
    return;
  }
  bool dns_ok = ensure_dns_ready(list_host.c_str());
  if (!dns_ok) {
    Serial.println("[NET] dns_error; proceeding with fetch");
  }
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  Serial.printf("[NET_DIAG] fetch_attempt reason=%s wifi=%d ip=%s dns0=%s dns1=%s dns_ok=%d\n",
                g_last_refresh_reason,
                wifi_ok ? 1 : 0,
                ip.toString().c_str(),
                dns0.toString().c_str(),
                dns1.toString().c_str(),
                dns_ok ? 1 : 0);

  Serial.print("URL: ");
  Serial.println(list_url);
  Serial.print("Request body: ");
  Serial.println(request_body);

  auto do_list_request = [&](String& response_json,
                             String& err_str,
                             unsigned long& duration_ms,
                             bool& begin_ok,
                             int& tls_err,
                             String& tls_err_str) -> int {
    unsigned long start_ms = millis();
    WiFiClientSecure req_client;
    HTTPClient req_http;
    req_client.setInsecure();
    begin_ok = false;
    tls_err = 0;
    tls_err_str = "";
    if (!req_http.begin(req_client, list_url)) {
      err_str = "begin_failed";
      duration_ms = millis() - start_ms;
      return -1;
    }
    begin_ok = true;
    req_http.setTimeout(30000);
    req_http.setConnectTimeout(10000);
    req_http.addHeader("Content-Type", "application/json");
    int httpResponseCode = req_http.POST(request_body);
    err_str = req_http.errorToString(httpResponseCode);
    if (httpResponseCode <= 0) {
      char tls_err_buf[128] = {0};
      tls_err = req_client.lastError(tls_err_buf, sizeof(tls_err_buf));
      tls_err_str = String(tls_err_buf);
    }
    if (httpResponseCode > 0) {
      response_json = req_http.getString();
    }
    req_http.end();
    req_client.stop();
    duration_ms = millis() - start_ms;
    return httpResponseCode;
  };

  const int kMaxAttempts = 2;
  for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
    String response_json = "";
    String err_str = "";
    unsigned long request_duration_ms = 0;
    bool begin_ok = false;
    int tls_err = 0;
    String tls_err_str = "";
    int httpResponseCode =
        do_list_request(response_json, err_str, request_duration_ms, begin_ok, tls_err, tls_err_str);
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    if (httpResponseCode == 200) {
      Serial.print("Response length: ");
      Serial.println(response_json.length());

      if (response_json.length() > 0 && response_json.charAt(0) == '{') {
        Serial.println("✓ Response appears to be JSON");
        if (!parse_and_update_shopping_list(response_json)) {
          list_refresh_fail("json_parse");
        }
      } else {
        Serial.println("⚠ Response doesn't look like JSON");
        Serial.print("First 100 chars: ");
        Serial.println(response_json.substring(0, 100));
      }
      return;
    }
    if (httpResponseCode > 0) {
      Serial.printf("[NET] fetch_fail kind=HTTP code=%d errno=%d duration_ms=%lu\n",
                    httpResponseCode,
                    errno,
                    request_duration_ms);
      list_refresh_fail("http_error");
      return;
    }

    String err_lower = err_str;
    err_lower.toLowerCase();
    IPAddress dns_ip;
    bool dns_resolve_ok = WiFi.hostByName(list_host.c_str(), dns_ip);
    const char* fail_kind = "TLS";
    if (!dns_resolve_ok) {
      fail_kind = "DNS";
    } else if (httpResponseCode == HTTPC_ERROR_READ_TIMEOUT ||
               err_lower.indexOf("timeout") >= 0 ||
               err_lower.indexOf("timed out") >= 0 ||
               err_lower.indexOf("connect") >= 0) {
      fail_kind = "TIMEOUT";
    }
    Serial.printf("[NET] fetch_fail kind=%s code=%d errno=%d duration_ms=%lu\n",
                  fail_kind,
                  httpResponseCode,
                  errno,
                  request_duration_ms);
    Serial.printf("[NET] fetch_fail begin_ok=%d dns_ok=%d tls_err=%d tls_msg=%s\n",
                  begin_ok ? 1 : 0,
                  dns_resolve_ok ? 1 : 0,
                  tls_err,
                  tls_err_str.c_str());

    if (httpResponseCode == -1 && attempt == 0) {
      unsigned long backoff_ms = 500 + (unsigned long)random(0, 1001);
      Serial.printf("[HTTP] code=-1 retrying backoff_ms=%lu\n", backoff_ms);
      vTaskDelay(pdMS_TO_TICKS(backoff_ms));
      continue;
    }

    Serial.print("✗ HTTP request failed! Error: ");
    Serial.println(err_str);
    if (strcmp(fail_kind, "DNS") == 0) {
      list_refresh_fail("dns_error");
    } else if (strcmp(fail_kind, "TIMEOUT") == 0) {
      list_refresh_fail("timeout");
    } else {
      list_refresh_fail("network_fail");
    }
    return;
  }
}

#endif // SENSE_LIST_H
