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
 *   - g_last_refresh_reason, list_last_fetch_ok_ms
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
  // Do not publish the RAM cache as a successful refresh. In particular, a
  // fresh boot's zero count is not evidence that the backend list is empty.
  // The existing LCD hard timeout preserves its cache and shows Couldn't refresh.
  uart_send_ui_status("IDLE");
}

// ── Parse and update shopping list ─────────────────────────────────

// Sort comparator: group items by store name (empty store sorts last) so the
// LCD can render store-group headers from a contiguous run of like-stored items.
static int shopping_list_cmp_by_store(const void* a, const void* b) {
  const shopping_list_item_t* ia = (const shopping_list_item_t*)a;
  const shopping_list_item_t* ib = (const shopping_list_item_t*)b;
  bool ae = (ia->store[0] == '\0'), be = (ib->store[0] == '\0');
  if (ae && be) return 0;
  if (ae) return 1;          // empty store sorts last
  if (be) return -1;
  return strcmp(ia->store, ib->store);
}

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

    // Extract household_item_uuid — the key the backend deletes by (same as
    // the iOS app's swipe-delete). Fallback empty if the field is missing.
    const char* huuid_str = item["household_item_uuid"] | "";
    if (huuid_str != NULL && strlen(huuid_str) > 0) {
      size_t huuid_len = strlen(huuid_str);
      if (huuid_len >= sizeof(g_shopping_list[actual_count].huuid)) {
        huuid_len = sizeof(g_shopping_list[actual_count].huuid) - 1;
      }
      strncpy(g_shopping_list[actual_count].huuid, huuid_str, huuid_len);
      g_shopping_list[actual_count].huuid[huuid_len] = '\0';
    } else {
      g_shopping_list[actual_count].huuid[0] = '\0';
    }

    // Store name (e.g. "Whole Foods", "Costco") — for LCD store-group headers.
    const char* store_str = item["store"] | "";
    if (store_str != NULL && strlen(store_str) > 0) {
      size_t store_len = strlen(store_str);
      if (store_len >= sizeof(g_shopping_list[actual_count].store)) {
        store_len = sizeof(g_shopping_list[actual_count].store) - 1;
      }
      strncpy(g_shopping_list[actual_count].store, store_str, store_len);
      g_shopping_list[actual_count].store[store_len] = '\0';
    } else {
      g_shopping_list[actual_count].store[0] = '\0';
    }

    actual_count++;
  }

  g_list_count = actual_count;

  // Group items by store so the LCD can render store-group headers. Safe to
  // reorder: delete is by id (not index), and g_selected_index is reset to 0
  // below (not preserved by position; the LCD re-syncs selection from UI_LIST).
  qsort(g_shopping_list, g_list_count, sizeof(shopping_list_item_t), shopping_list_cmp_by_store);

  g_selected_index = (actual_count > 0) ? 0 : -1;

  xSemaphoreGive(g_list_mutex);

  Serial.print("✓ Parsed ");
  Serial.print(actual_count);
  Serial.println(" items");

  // Record successful fetch time — request_list_refresh() serves this cached
  // list when a user refresh lands inside the cooldown window.
  list_last_fetch_ok_ms = millis();

  // Send updated list to LCD
  uart_send_ui_list();

  // Send IDLE status
  delay(50);
  uart_send_ui_status("IDLE");
  return true;
}

// ── Delete item from API ───────────────────────────────────────────

// Remove the item at list_index from g_shopping_list (caller must hold
// g_list_mutex). Compacts the array and fixes g_list_count/g_selected_index
// so a subsequent cached serve (request_list_refresh cooldown path) can't
// resurrect the deleted item.
static void remove_item_from_ram_list_locked(int list_index) {
  if (list_index < 0 || list_index >= g_list_count) return;
  for (int i = list_index; i < g_list_count - 1; i++) {
    g_shopping_list[i] = g_shopping_list[i + 1];
  }
  g_list_count--;
  if (g_list_count <= 0) {
    g_list_count = 0;
    g_selected_index = -1;
  } else {
    if (g_selected_index > list_index) g_selected_index--;
    if (g_selected_index >= g_list_count) g_selected_index = g_list_count - 1;
  }
}

static void delete_item_from_api(const char* item_id) {
  if (item_id == NULL || strlen(item_id) == 0) {
    Serial.println("✗ Cannot delete item: invalid ID!");
    return;
  }

  Serial.printf("\n=== Deleting Item ID: %s ===\n", item_id);

  // Look up the item's household_item_uuid in the RAM list. The backend
  // (and the iOS app's swipe-delete) delete by itemUUID — the household-wide
  // key — NOT the per-table row id.
  char item_huuid[64] = {0};
  if (g_list_mutex != NULL && xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    for (int i = 0; i < g_list_count; i++) {
      if (strcmp(g_shopping_list[i].id, item_id) == 0) {
        strncpy(item_huuid, g_shopping_list[i].huuid, sizeof(item_huuid) - 1);
        item_huuid[sizeof(item_huuid) - 1] = '\0';
        break;
      }
    }
    xSemaphoreGive(g_list_mutex);
  }

  // Build JSON request body for remove operation. Mirrors the iOS app:
  // {"operation":"remove","ownerId":...,"device":...,"itemUUID":<huuid>}.
  // Legacy {"id":...} body only if we have no huuid for this item.
  char owner_id[64] = {0};
  load_owner_id_or_default(owner_id, sizeof(owner_id));
  bool have_huuid = (item_huuid[0] != '\0');
  String request_body = "{";
  request_body += "\"operation\":\"remove\",";
  request_body += "\"ownerId\":\"" + String(owner_id) + "\",";
  request_body += "\"device\":\"" + String(TREPO_DEVICE_ID) + "\",";
  if (have_huuid) {
    request_body += "\"itemUUID\":\"" + String(item_huuid) + "\"";
  } else {
    Serial.printf("[DELETE] warn no household_item_uuid for id=%s — using legacy id body\n",
                  item_id);
    request_body += "\"id\":\"" + String(item_id) + "\"";
  }
  request_body += "}";

  // Free the camera's 16 KB internal-DMA reserve for the duration of this TLS
  // handshake, and take it back on every exit path.
  //
  // WHY: an mbedtls AES-DMA handshake needs a large slice of INTERNAL SRAM, and
  // that is the same scarce pool the camera reserve sits in. During the
  // post-provisioning burst -- SoftAP still tearing down, WiFi reconnecting,
  // LIST_REFRESH and the OTA manifest fetch both wanting TLS -- internal heap
  // fell to ~18 KB and the handshake could not even allocate a mutex for a
  // printf: lock_init_generic -> abort(), device rebooted, and the user's first
  // capture was destroyed (2026-08-31).
  //
  // The OTA path already does this ("[OTA] Camera DMA reservation released for
  // TLS headroom"); the list path did not, and it is the one that runs first
  // after provisioning. Measured margin: the synthetic repro bottomed out at
  // ~20.5 KB versus the ~18.4 KB crash, so returning 16 KB here is ample.
  struct ListTlsDmaGuard {
    bool held;
    ListTlsDmaGuard() : held(g_camera_dma_reserve != nullptr) {
      if (held) camera_dma_reserve_release("list_tls");
    }
    ~ListTlsDmaGuard() { if (held) camera_dma_reserve_acquire("list_tls"); }
  } list_tls_dma_guard;

  // Create HTTPS client
  HaloNtpDnsGuard ntp_dns_guard;
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
    Serial.printf("[DELETE] ok itemUUID=%s id=%s\n",
                  have_huuid ? item_huuid : "(none)", item_id);
    // Remove from the RAM cache too so a cached serve can't resurrect the
    // deleted item. No UI_LIST push — the LCD already removed it locally.
    if (g_list_mutex != NULL && xSemaphoreTake(g_list_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      int found_index = -1;
      for (int i = 0; i < g_list_count; i++) {
        if (strcmp(g_shopping_list[i].id, item_id) == 0) {
          found_index = i;
          break;
        }
      }
      if (found_index >= 0) {
        remove_item_from_ram_list_locked(found_index);
        Serial.printf("[DELETE] ram list updated count=%d selected=%d\n",
                      g_list_count, g_selected_index);
      }
      xSemaphoreGive(g_list_mutex);
    }
    uart_send_ui_status("Item deleted");
  } else {
    Serial.printf("[DELETE] fail code=%d id=%s\n", httpResponseCode, item_id);
    // RAM list left unchanged. Re-send the list so the LCD's optimistic
    // removal reconverges with reality. Note: the LCD's deleted_item_ids RAM
    // filter may still hide the item this boot — acceptable; logged here.
    Serial.println("[DELETE] resending UI_LIST to reconverge LCD (may be filtered by LCD deleted_item_ids this boot)");
    uart_send_ui_list();
    uart_send_ui_status("IDLE");
  }
}

// ── Fetch shopping list from API ───────────────────────────────────

// Short, bounded WiFi-connect budget for the LIST fetch only. On flaky WiFi
// the global ensure_wifi_ready() path can churn ~40s (25s connect timeout +
// ~15s hard-reset chain), which pins op_inflight and blocks deep sleep on the
// darkened list screen. For the list fetch we use a short budget and DO NOT
// escalate to the hard-reset chain — if WiFi isn't up in time we fail the
// fetch fast so the op (current_job) completes and op_inflight releases.
// Other paths (OTA/uploads) keep using WIFI_CONNECT_TIMEOUT_MS unchanged.
static const uint32_t LIST_FETCH_WIFI_BUDGET_MS = 6000;

// Minimum INTERNAL heap required before starting a TLS handshake.
//
// Below roughly this, an mbedtls AES-DMA handshake cannot complete and the
// failure is vicious: it aborts inside a printf (lock_init_generic cannot
// allocate its mutex), so the device dies with no useful message and reboots,
// destroying whatever was in PSRAM. Measured: crash at ~18.4 KB internal,
// healthy runs sit at ~20.5-21 KB. Refusing at 24 KB keeps a real margin and,
// crucially, turns an abort into a retryable skip with a log line.
static const size_t LIST_TLS_MIN_INTERNAL_HEAP = 24 * 1024;

static bool list_tls_heap_ok(const char* why) {
  const size_t internal_free =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (internal_free >= LIST_TLS_MIN_INTERNAL_HEAP) {
    return true;
  }
  Serial.printf("[LIST_TLS] SKIP %s: internal heap %u < %u — refusing handshake "
                "(would abort in printf)\n",
                why ? why : "", (unsigned)internal_free,
                (unsigned)LIST_TLS_MIN_INTERNAL_HEAP);
  return false;
}

static bool fetch_shopping_list_from_api() {
  if (WiFi.status() != WL_CONNECTED) {
    // Bounded, no-hard-reset connect attempt. ensure_wifi_connected() does NOT
    // escalate to wifi_hard_reset_and_reconnect() (only ensure_wifi_ready()
    // does), so this caps the wait at LIST_FETCH_WIFI_BUDGET_MS.
    Serial.printf("[LIST_FETCH] wifi not connected; bounded connect budget_ms=%lu\n",
                  (unsigned long)LIST_FETCH_WIFI_BUDGET_MS);
    unsigned long wifi_start_ms = millis();
    if (!ensure_wifi_connected("list_fetch", LIST_FETCH_WIFI_BUDGET_MS) ||
        WiFi.status() != WL_CONNECTED) {
      Serial.println("✗ Cannot fetch list: WiFi not connected (bounded budget exhausted)!");
      list_refresh_fail("wifi_not_connected");
      return false;
    }
    Serial.printf("[NET_DIAG] list_wifi_connect_ms=%lu\n",
                  (unsigned long)(millis() - wifi_start_ms));
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
    return false;
  }
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  IPAddress ip = WiFi.localIP();
  bool ip_ok = (static_cast<uint32_t>(ip) != 0);
  if (!wifi_ok || !ip_ok) {
    Serial.println("[NET] not ready - skipping list fetch");
    list_refresh_fail("net_not_ready");
    return false;
  }
  HaloNtpDnsGuard ntp_dns_guard;
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
                             bool& heap_admitted,
                             int& tls_err,
                             String& tls_err_str) -> int {
    unsigned long start_ms = millis();
    begin_ok = false;
    heap_admitted = false;
    tls_err = 0;
    tls_err_str = "";
    // Test the memory available to TLS, after releasing the banked camera
    // reserve. Testing before the guard rejected otherwise viable requests.
    // Keep the threshold and request-scoped ownership unchanged.
    if (!list_tls_heap_ok("fetch_shopping_list")) {
      err_str = "low_internal_heap";
      duration_ms = millis() - start_ms;
      return -1;
    }
    heap_admitted = true;
    WiFiClientSecure req_client;
    HTTPClient req_http;
    req_client.setInsecure();
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
    bool retry = false;
    {
      // The response buffer is owned by this attempt, not by HTTPClient. Keep
      // the reserve released until parsing and all response/error temporaries
      // are destroyed. TLS clients still close inside do_list_request().
      struct ListTlsDmaGuard {
        bool held;
        ListTlsDmaGuard() : held(g_camera_dma_reserve != nullptr) {
          if (held) camera_dma_reserve_release("list_tls");
        }
        ~ListTlsDmaGuard() {
          if (!held) return;
          const bool restored = camera_dma_reserve_acquire("list_tls");
          Serial.printf("[LIST_TLS] reserve_restore ok=%d held=%d\n",
                        restored ? 1 : 0, g_camera_dma_reserve != nullptr ? 1 : 0);
        }
      } list_tls_dma_guard;
      String response_json = "";
      String err_str = "";
      unsigned long request_duration_ms = 0;
      bool begin_ok = false;
      bool heap_admitted = false;
      int tls_err = 0;
      String tls_err_str = "";
      int httpResponseCode =
          do_list_request(response_json, err_str, request_duration_ms, begin_ok,
                          heap_admitted, tls_err, tls_err_str);
      if (!heap_admitted) {
        list_refresh_fail("low_internal_heap");
        return false;  // No handshake occurred; do not enter the network retry path.
      }
      Serial.print("HTTP Response code: ");
      Serial.println(httpResponseCode);
      if (httpResponseCode == 200) {
        Serial.printf("[NET_DIAG] fetch_ok code=200 http_ms=%lu resp_len=%u attempt=%d\n",
                      request_duration_ms,
                      (unsigned)response_json.length(),
                      attempt);
        Serial.print("Response length: ");
        Serial.println(response_json.length());

        if (response_json.length() > 0 && response_json.charAt(0) == '{') {
          Serial.println("✓ Response appears to be JSON");
          unsigned long parse_start_ms = millis();
          bool parse_ok = parse_and_update_shopping_list(response_json);
          Serial.printf("[LIST_REFRESH] parse_ms=%lu ok=%d\n",
                        (unsigned long)(millis() - parse_start_ms),
                        parse_ok ? 1 : 0);
          if (!parse_ok) {
            list_refresh_fail("json_parse");
          }
          return parse_ok;
        } else {
          Serial.println("⚠ Response doesn't look like JSON");
          Serial.print("First 100 chars: ");
          Serial.println(response_json.substring(0, 100));
          list_refresh_fail("json_parse");
        }
        return false;
      }
      if (httpResponseCode > 0) {
        Serial.printf("[NET] fetch_fail kind=HTTP code=%d errno=%d duration_ms=%lu\n",
                      httpResponseCode,
                      errno,
                      request_duration_ms);
        list_refresh_fail("http_error");
        return false;
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
        retry = true;
      } else {
        Serial.print("✗ HTTP request failed! Error: ");
        Serial.println(err_str);
        if (strcmp(fail_kind, "DNS") == 0) {
          list_refresh_fail("dns_error");
        } else if (strcmp(fail_kind, "TIMEOUT") == 0) {
          list_refresh_fail("timeout");
        } else {
          list_refresh_fail("network_fail");
        }
        return false;
      }
    }  // Destroy response/error/JSON temporaries, then restore the camera reserve.
    if (retry) {
      unsigned long backoff_ms = 500 + (unsigned long)random(0, 1001);
      Serial.printf("[HTTP] code=-1 retrying backoff_ms=%lu\n", backoff_ms);
      vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }
  }
  return false;
}

#endif // SENSE_LIST_H
