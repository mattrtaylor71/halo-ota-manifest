/*
 * sense_http.h
 *
 * HTTP/TLS transport helpers for Sense_Minimal.
 * DNS resolution, deadline utilities, TLS configuration,
 * WiFi diagnostics, URL parsing, and HTTP failure logging.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 4.
 *
 * Prerequisites (must be declared before #include "sense_http.h"):
 *   - WiFi.h, WiFiClientSecure.h, HTTPClient.h
 *   - Arduino.h (millis, delay, Serial)
 *   - rootCA certificate string (or HAS_CRT_BUNDLE)
 *   - TREPO_API_HOST, awsEndpoint constants
 *   - uart_send_sense_diag() from sense_uart.h
 */

#ifndef SENSE_HTTP_H
#define SENSE_HTTP_H

// ── URL parsing ──────────────────────────────────────────────────────

static bool extract_host_from_url(const String& url, String& host) {
  int scheme = url.indexOf("://");
  int start = scheme >= 0 ? scheme + 3 : 0;
  int slash = url.indexOf('/', start);
  if (slash < 0) {
    slash = url.length();
  }
  host = url.substring(start, slash);
  return host.length() > 0;
}

static bool parse_url_parts(const String& url, bool& https, String& host, uint16_t& port, String& path) {
  https = false;
  host = "";
  path = "/";
  port = 0;
  if (url.startsWith("https://")) {
    https = true;
  } else if (url.startsWith("http://")) {
    https = false;
  } else {
    return false;
  }
  const int host_start = https ? 8 : 7;
  const int path_start = url.indexOf('/', host_start);
  const String host_port = (path_start >= 0)
                             ? url.substring(host_start, path_start)
                             : url.substring(host_start);
  if (host_port.length() == 0) {
    return false;
  }
  const int colon = host_port.indexOf(':');
  if (colon >= 0) {
    host = host_port.substring(0, colon);
    port = (uint16_t)host_port.substring(colon + 1).toInt();
  } else {
    host = host_port;
    port = https ? 443 : 80;
  }
  if (path_start >= 0) {
    path = url.substring(path_start);
    if (path.length() == 0) {
      path = "/";
    }
  }
  return host.length() > 0 && port > 0;
}

// ── DNS helpers ──────────────────────────────────────────────────────

static void net_diag_dns_for_host(const char* host, const char* reason) {
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  IPAddress resolved_ip;
  bool resolve_ok = false;
  if (host && host[0]) {
    resolve_ok = WiFi.hostByName(host, resolved_ip);
  }
  Serial.printf("[NET_DIAG] reason=%s host=%s dns0=%s dns1=%s resolve_ok=%d\n",
                reason ? reason : "unknown",
                host ? host : "(null)",
                dns0.toString().c_str(),
                dns1.toString().c_str(),
                resolve_ok ? 1 : 0);
}

static bool dns_ip_is_empty(const IPAddress& ip) {
  bool all_zero = (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0);
  bool all_ff = (ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255);
  return all_zero || all_ff;
}

static bool dns_resolve_test(const char* host) {
  if (!host || !host[0]) {
    return true;
  }
  IPAddress resolved_ip;
  bool resolve_ok = WiFi.hostByName(host, resolved_ip);
  Serial.printf("[DNS] resolve_test host=%s ok=%d\n",
                host,
                resolve_ok ? 1 : 0);
  return resolve_ok;
}

static bool apply_dns_strategy(const char* reason, const char* host_a, const char* host_b) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  IPAddress dns0 = WiFi.dnsIP(0);
  IPAddress dns1 = WiFi.dnsIP(1);
  bool dns_missing = dns_ip_is_empty(dns0) && dns_ip_is_empty(dns1);
  bool resolve_ok = true;
  resolve_ok = dns_resolve_test(host_a) && dns_resolve_test(host_b);
  const char* decision = (dns_missing || !resolve_ok) ? "use_fallback" : "use_dhcp";
  const char* why = dns_missing ? "dhcp_missing" : (!resolve_ok ? "resolve_fail" : "dhcp_ok");
  Serial.printf("[DNS] dhcp d0=%s d1=%s decision=%s reason=%s\n",
                dns0.toString().c_str(),
                dns1.toString().c_str(),
                decision,
                why);
  if (dns_missing || !resolve_ok) {
    IPAddress new_dns1(1, 1, 1, 1);
    IPAddress new_dns2(8, 8, 8, 8);
    bool applied = WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, new_dns1, new_dns2);
    if (applied) {
      delay(100);
    }
    bool retry_ok = dns_resolve_test(host_a) && dns_resolve_test(host_b);
    (void)reason;
    return retry_ok;
  }
  (void)reason;
  return resolve_ok;
}

static void apply_public_dns_for_api(const char* reason) {
  (void)apply_dns_strategy(reason ? reason : "wifi", TREPO_API_HOST, awsEndpoint);
}

static bool ensure_dns_ready(const char* host) {
  if (!host || !host[0]) {
    return false;
  }
  return apply_dns_strategy("ensure_dns", host, NULL);
}

static bool wait_for_network_ready(const char* host, uint32_t timeout_ms) {
  const uint32_t STABLE_MS = 1000;
  unsigned long start = millis();
  unsigned long stable_start = 0;
  bool dns_ok = (host == NULL || !host[0]);
  bool dns_checked = (host == NULL || !host[0]);
  Serial.printf("[NET] waiting_ready timeout=%lu host=%s\n",
                (unsigned long)timeout_ms,
                host ? host : "(null)");
  while ((millis() - start) < timeout_ms) {
    bool wifi_ok = (WiFi.status() == WL_CONNECTED);
    IPAddress ip = WiFi.localIP();
    bool ip_ok = (static_cast<uint32_t>(ip) != 0);
    if (!dns_checked && wifi_ok && ip_ok && host && host[0]) {
      dns_ok = ensure_dns_ready(host);
      dns_checked = true;
    }
    if (wifi_ok && ip_ok) {
      if (stable_start == 0) {
        stable_start = millis();
      }
      if ((millis() - stable_start) >= STABLE_MS) {
        if (dns_checked && !dns_ok) {
          Serial.printf("[NET] dns_error host=%s\n", host ? host : "(null)");
        }
        Serial.printf("[NET] ready wifi=1 ip=%s dns_ok=%d stable_ms=%lu\n",
                      ip.toString().c_str(),
                      dns_ok ? 1 : 0,
                      (unsigned long)(millis() - stable_start));
        return true;
      }
    } else {
      stable_start = 0;
    }
    delay(100);
  }
  IPAddress ip = WiFi.localIP();
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  bool ip_ok = (static_cast<uint32_t>(ip) != 0);
  if (!dns_checked && wifi_ok && ip_ok && host && host[0]) {
    dns_ok = ensure_dns_ready(host);
    dns_checked = true;
  }
  if (wifi_ok && ip_ok) {
    if (dns_checked && !dns_ok) {
      Serial.printf("[NET] dns_error host=%s\n", host ? host : "(null)");
    }
    Serial.printf("[NET] ready wifi=1 ip=%s dns_ok=%d stable_ms=%lu\n",
                  ip.toString().c_str(),
                  dns_ok ? 1 : 0,
                  (unsigned long)timeout_ms);
    return true;
  }
  Serial.printf("[NET] not_ready wifi=%d ip=%s dns_ok=%d\n",
                wifi_ok ? 1 : 0,
                ip.toString().c_str(),
                dns_ok ? 1 : 0);
  return false;
}

// ── TLS configuration ────────────────────────────────────────────────

static void tls_configure(WiFiClientSecure& client, const char* reason) {
#if HAS_CRT_BUNDLE
  extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
  extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
  client.setCACertBundle(x509_crt_bundle_start,
                         x509_crt_bundle_end - x509_crt_bundle_start);
  Serial.printf("[TLS] using crt bundle reason=%s\n", reason ? reason : "unknown");
#else
  client.setCACert(rootCA);
  Serial.printf("[TLS] using root CA reason=%s\n", reason ? reason : "unknown");
#endif
  client.setTimeout(15000);
}

// ── WiFi diagnostics ─────────────────────────────────────────────────

static void log_wifi_snapshot(const char* label) {
  IPAddress ip = WiFi.localIP();
  IPAddress gw = WiFi.gatewayIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress dns1 = WiFi.dnsIP(0);
  IPAddress dns2 = WiFi.dnsIP(1);
  int32_t rssi = WiFi.RSSI();
  Serial.printf("[WIFI] %s status=%d rssi=%ld ip=%s gw=%s mask=%s dns1=%s dns2=%s\n",
                label ? label : "snapshot",
                (int)WiFi.status(),
                (long)rssi,
                ip.toString().c_str(),
                gw.toString().c_str(),
                mask.toString().c_str(),
                dns1.toString().c_str(),
                dns2.toString().c_str());
}

static void log_http_failure_details(const char* label, const char* url, int http_code, WiFiClientSecure* client) {
  const bool is_https = url && (strncmp(url, "https://", 8) == 0);
  char tls_err_msg[128] = {0};
  int tls_err = 0;
  if (client) {
    tls_err = client->lastError(tls_err_msg, sizeof(tls_err_msg));
  }
  Serial.printf("[HTTP_FAIL] label=%s code=%d err=%s errno=%d tls_err=%d tls_msg=%s url=%s https=%d heap=%u\n",
                label ? label : "http",
                http_code,
                HTTPClient::errorToString(http_code).c_str(),
                errno,
                tls_err,
                tls_err_msg,
                url ? url : "",
                is_https ? 1 : 0,
                (unsigned)ESP.getFreeHeap());
  char detail[96];
  snprintf(detail, sizeof(detail), "errno=%d tls_err=%d https=%d", errno, tls_err, is_https ? 1 : 0);
  uart_send_sense_diag("http", "fail", label, (int32_t)http_code, detail);
  log_wifi_snapshot(label);
}

// ── Deadline / timeout utilities ─────────────────────────────────────

static bool deadline_expired(uint32_t deadline_ms) {
  if (deadline_ms == 0) {
    return false;
  }
  return (int32_t)(millis() - deadline_ms) >= 0;
}

static uint32_t deadline_remaining_ms(uint32_t deadline_ms) {
  if (deadline_ms == 0) {
    return UINT32_MAX;
  }
  int32_t diff = (int32_t)(deadline_ms - millis());
  return diff > 0 ? (uint32_t)diff : 0;
}

static uint32_t clamp_timeout_ms(uint32_t desired_ms, uint32_t deadline_ms) {
  uint32_t remaining = deadline_remaining_ms(deadline_ms);
  if (remaining == UINT32_MAX) {
    return desired_ms;
  }
  return (remaining < desired_ms) ? remaining : desired_ms;
}

#endif // SENSE_HTTP_H
