/*
 * lcd_uart_task.h
 *
 * FreeRTOS UART task: drains TX queue, reads serial RX byte-by-byte,
 * assembles JSON lines, and dispatches to uart_process_received_message.
 * Runs on Core 0, priority 2.
 *
 * Extracted from LCD_Minimal.ino as modularization Step 4.
 *
 * Prerequisites (must be declared before #include "lcd_uart_task.h"):
 *   - lcd_uart.h (tx_msg_t, uart_tx_queue, deferred_awake_tx_*)
 *   - lcd_uart_rx.h (uart_process_received_message)
 *   - senseSerial, uart_rx_line_buffer, uart_rx_line_pos, etc.
 *   - sense_ready_for_control_tx(), deferred_awake_tx_service()
 */

#ifndef LCD_UART_TASK_H
#define LCD_UART_TASK_H

#include "lcd_id1_diagnostic.h"

static void uart_task(void *arg) {
  Serial.println("[UART] UART task started");

  // This task owns the inter-board link and runs on Core 0. When it stops, the
  // LCD goes silent on BOTH UART and USB and the board is unrecoverable without
  // a physical power cycle - observed three times in one bench session. Watch it.
  lcd_freeze_wdt_subscribe("uart_task");

  static bool diag_mode = false;
  static unsigned long diag_mode_end_ms = 0;
  static unsigned long diag_last_ping_ms = 0;

  for (;;) {
    lcd_freeze_wdt_feed();
    const int UART_TX_MAX_PER_LOOP = 16;
    const int UART_RX_MAX_BYTES_PER_LOOP = 512;
    bool yielded_early = false;

    if (lcd_refresh_inflight && !lcd_refresh_ack_seen && lcd_refresh_sent_ms == 0) {
      if (refresh_state != REFRESH_INFLIGHT) {
        Serial.printf("[REFRESH_SM] skip INPUT_WAKE reason=state state=%s\n",
                      refresh_state_name(refresh_state));
      } else if (!refresh_wake_sent) {
        wake_sense_for_request("refresh_send");
        uart_send_input_message("INPUT_WAKE");
        refresh_request_needs_send = false;
        lcd_refresh_sent_ms = millis();
        refresh_wake_sent = true;
        refresh_last_wake_send_ms = lcd_refresh_sent_ms;
        Serial.println("[REFRESH] INPUT_WAKE sent (uart_task)");
      }
    }

    // 1) TX drain - send queued messages
    tx_msg_t tx_msg;
    int tx_count = 0;
    if (deferred_awake_tx_pending()) {
      // deferred_awake_tx_service() now owns both halves: it flushes the ring in
      // order when the Sense is reachable, and otherwise ages entries out and
      // keeps the wake handshake going.
      const uint8_t before = deferred_ring_count;
      deferred_awake_tx_service();
      if (deferred_ring_count < before) {
        tx_count += (int)(before - deferred_ring_count);
      }
      if (deferred_awake_tx_pending()) {
        yielded_early = true;
      }
    }
    // Never interleave JSON with a COBS transfer. This TX drain shares the UART
    // with the binary framing, and it runs BEFORE the binary-mode branch below,
    // so a status line emitted mid-transfer lands inside the peer's frame
    // parser. The Sense saw exactly that: "Frame too short: decoded_len=58,
    // expected=8825 (data_len=8818)" — 0x2272 is '"r', i.e. JSON text being
    // decoded as a frame header. Messages stay queued and drain once the
    // transfer ends; holding them is correct, because the alternative is
    // corrupting a transfer that is carrying the user's photo.
    // Must include the spool SEND state, not just the receive states. When the
    // LCD streams a spooled image BACK to the Sense it is the transmitter, and
    // the drain below would happily interleave JSON into its own COBS stream —
    // the Sense saw exactly that as data_len=8818 (0x2272 = '"r').
    const bool binary_xfer_active = g_img_rx_binary_mode || g_lcd_ota_binary_mode ||
                                    g_spool_tx_pending || g_spool_tx_active;

    // Retransmit any user-intent message the Sense has not acked. Skipped
    // during a binary transfer for the same reason the TX drain is: injecting
    // JSON into a COBS stream corrupts it.
    if (!binary_xfer_active) {
      link_ack_service();
    }

    // No longer gated on the deferred slot being empty. A message waiting for the
    // Sense to wake used to stop this drain entirely, so pings and diagnostics —
    // which need no awake-proof and would have gone out fine — sat behind it.
    if (!binary_xfer_active) {
      while (uart_tx_queue != NULL && xQueueReceive(uart_tx_queue, &tx_msg, 0) == pdTRUE) {
        if (tx_msg_requires_awake_proof(&tx_msg) && !sense_ready_for_control_tx()) {
          if (deferred_ring_count == DEFERRED_AWAKE_RING_SLOTS &&
              !strcmp(deferred_ring_oldest()->msg.type, "INPUT_DELETE")) {
            post_list_delete_result(deferred_ring_oldest()->msg.id, false);
          }
          const bool evicted = deferred_ring_push(&tx_msg);
          deferred_awake_tx_last_ping_ms = 0;
          Serial.printf("[UART] deferred_until_awake type=%s awake=%d synced=%d recent=%d "
                        "ring=%u/%u%s\n",
                        tx_msg.type,
                        sense_awake_confirmed ? 1 : 0,
                        link_synced ? 1 : 0,
                        sense_recently_heard(SENSE_CONTROL_READY_WINDOW_MS) ? 1 : 0,
                        (unsigned)deferred_ring_count, (unsigned)DEFERRED_AWAKE_RING_SLOTS,
                        evicted ? " EVICTED_OLDEST" : "");
          deferred_awake_tx_service();
          // Keep draining: the next message may not need awake-proof at all.
          continue;
        } else {
          tx_msg_send_now(&tx_msg);
        }
        tx_count++;
        if (tx_count >= UART_TX_MAX_PER_LOOP) {
          yielded_early = true;
          break;
        }
      }
    }
    
    // Drain: hand a spooled image back to the Sense. Done here rather than in
    // the RX handler so the ~16s stream does not block line parsing, and before
    // the inbound branch because the link can only carry one transfer at a time.
    if (g_spool_tx_pending) {
      g_spool_tx_pending = false;
      lcd_spool_send_file(g_spool_tx_slot);
      g_spool_tx_slot = 0;
      continue;
    }

    // Image binary mode — delegate all RX to lcd_img_receive_loop.
    // Placed BEFORE the OTA branch: both use the same COBS transport, and if an
    // image transfer is in flight it must own the link until it finishes.
    if (g_img_rx_binary_mode) {
      lcd_img_receive_loop();
      vTaskDelay(pdMS_TO_TICKS(1));
      continue;
    }

    // OTA binary mode — delegate all RX to lcd_ota_receive_loop
    if (g_lcd_ota_binary_mode) {
      static bool s_binary_announced = false;
      if (!s_binary_announced) {
        Serial.printf("[LCD_UART_TASK] entering binary mode avail=%d\n",
                      senseSerial.available());
        s_binary_announced = true;
      }
      lcd_ota_receive_loop();
      vTaskDelay(pdMS_TO_TICKS(1));
      if (!g_lcd_ota_binary_mode) s_binary_announced = false;
      continue;
    }

    // 2) RX read and parse lines
    int rx_bytes = 0;
    while (senseSerial.available() > 0) {
      char c = senseSerial.read();
      uart_rx_raw_bytes_seen++;
      if (UART_RX_DEBUG) {
        Serial.printf("[UART_RAW] rx_byte=0x%02X\n", (uint8_t)c);
      }
      if (uart_rx_diag_raw_logged < UART_RX_DIAG_CAPTURE_LIMIT) {
        char printable = (c >= 32 && c <= 126) ? c : '.';
        Serial.printf("[UART_RAW_DIAG] idx=%lu byte=0x%02X char=%c\n",
                      uart_rx_raw_bytes_seen,
                      (uint8_t)c,
                      printable);
        uart_rx_diag_raw_logged++;
      }
      
      // A control retry may leave COBS mode with a zero delimiter followed by
      // JSON. Treat that delimiter as resynchronization, including after the
      // receiver has already cleaned up and only its ABORT_ACK was lost.
      if (c == 0) {
        uart_rx_line_pos = 0;
        uart_rx_partial_started_ms = 0;
      } else if (c == '\n' || c == '\r') {
        if (uart_rx_line_pos > 0) {
          uart_rx_line_buffer[uart_rx_line_pos] = '\0';
          uart_rx_completed_lines_seen++;
          uart_process_received_message(uart_rx_line_buffer);
          uart_rx_line_pos = 0;
          uart_rx_partial_started_ms = 0;
          // That message may have switched us into a binary transfer
          // (IMG_XFER_BEGIN or LCD_OTA_BEGIN). If so we must stop
          // line-parsing IMMEDIATELY — otherwise this loop keeps consuming the
          // incoming COBS frames as text and the transfer dies at seq=0 with a
          // frame timeout on both boards.
          if (g_img_rx_binary_mode || g_lcd_ota_binary_mode) break;
        }
      } else if (uart_rx_line_pos < MAX_LINE_LENGTH - 1) {
        if (uart_rx_line_pos == 0) {
          uart_rx_partial_started_ms = millis();
        }
        uart_rx_line_buffer[uart_rx_line_pos++] = c;
      } else {
        // Line too long, flush buffer
        Serial.printf("[PROTO] Line overflow (max=%d) - flushing buffer\n", MAX_LINE_LENGTH);
        uart_rx_line_pos = 0;
        uart_rx_partial_started_ms = 0;
      }
      rx_bytes++;
      if (rx_bytes >= UART_RX_MAX_BYTES_PER_LOOP) {
        yielded_early = true;
        break;
      }
    }
    unsigned long now_ms = millis();
    if (uart_rx_line_pos > 0 &&
        uart_rx_partial_started_ms > 0 &&
        (now_ms - uart_rx_partial_started_ms) >= 250 &&
        (now_ms - uart_rx_last_partial_log_ms) >= 1000) {
      char preview[33];
      int preview_len = uart_rx_line_pos < (int)(sizeof(preview) - 1)
                          ? uart_rx_line_pos
                          : (int)(sizeof(preview) - 1);
      memcpy(preview, uart_rx_line_buffer, preview_len);
      preview[preview_len] = '\0';
      HALO_CHATTY_PRINTF("[UART_DIAG] partial_line age_ms=%lu len=%d preview=%s\n",
                    now_ms - uart_rx_partial_started_ms,
                    uart_rx_line_pos,
                    preview);
      uart_rx_last_partial_log_ms = now_ms;
    }
    if ((deferred_awake_tx_pending() || uart_rx_valid_msgs_seen == 0) &&
        (now_ms - uart_rx_last_summary_ms) >= 1000) {
      int available_now = senseSerial.available();
      unsigned long proof_age_ms = last_proof_of_life_ms > 0 ? (now_ms - last_proof_of_life_ms) : 0xFFFFFFFFUL;
      unsigned long rx_age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0xFFFFFFFFUL;
      HALO_CHATTY_PRINTF("[UART_DIAG] summary avail=%d rx_gpio=%d raw_bytes=%lu lines=%lu valid=%lu line_pos=%d awake=%d synced=%d rx_age_ms=%lu proof_age_ms=%lu deferred=%d\n",
                    available_now,
                    (int)gpio_get_level((gpio_num_t)UART_RX_PIN),
                    uart_rx_raw_bytes_seen,
                    uart_rx_completed_lines_seen,
                    uart_rx_valid_msgs_seen,
                    uart_rx_line_pos,
                    sense_awake_confirmed ? 1 : 0,
                    link_synced ? 1 : 0,
                    rx_age_ms,
                    proof_age_ms,
                    deferred_awake_tx_pending() ? 1 : 0);
      uart_rx_last_summary_ms = now_ms;
    }

    // 3) USB Serial input handler — read JSON commands or text shortcuts,
    //    forward recognized types to Sense via UART.
    {
      static char usb_buf[256];
      static int  usb_pos = 0;
      int usb_read = 0;
      while (Serial.available() > 0 && usb_read < 64) {
        char c = Serial.read();
        usb_read++;
        if (c == '\n' || c == '\r') {
          if (usb_pos > 0) {
            usb_buf[usb_pos] = '\0';

            // --- Plain-text shortcut commands ---
            if (strncmp(usb_buf, "id1", 3) == 0 && (usb_buf[3] == ' ' || usb_buf[3] == '\0')) {
              lcd_id1_request(usb_buf);
            } else if (strcasecmp(usb_buf, "fw") == 0 || strcasecmp(usb_buf, "ver") == 0) {
              // Canonical machine-readable line — reuses the SAME partition/state
              // logic as LCD_OTA_QUERY_RESP (lcd_build_fw_status_json in
              // lcd_ota_uart.h) so the two reporting paths never diverge.
              StaticJsonDocument<256> fwdoc;
              lcd_build_fw_status_json(fwdoc);
              String fwout;
              serializeJson(fwdoc, fwout);
              Serial.printf("[FW] %s\n", fwout.c_str());

              // Legacy human-readable lines (back-compat for existing tooling).
              Serial.printf("[FW] lcd_fw=%s sense_fw=%s\n",
                            kFirmwareVersion ? kFirmwareVersion : "?",
                            g_sense_fw_version);
              Serial.printf("[FW] device_id=%s\n",
                            g_lcd_device_id[0] ? g_lcd_device_id : "?");
            } else if (strcmp(usb_buf, "clearwindow") == 0) {
              if (lcd_clear_persisted_maintenance_state("usb_clear")) {
                g_lcd_maintenance_active = false;
                g_lcd_maintenance_deadline_ms = 0;
                Serial.println("[USB_CMD] maintenance window cleared");
              } else {
                Serial.println("[USB_CMD] maintenance window clear failed");
              }
            } else if (strcmp(usb_buf, "ota") == 0) {
              // Run the exact Settings action on the LVGL owner task.
              g_manual_ota_ui_requested = true;
              Serial.println("[USB_CMD] ota -> Settings software-update action");
            } else if (strncmp(usb_buf, "sfwd ", 5) == 0) {
              // Generic Sense forwarder: 'sfwd <raw json>' relays the rest of the
              // line verbatim to the Sense over the inter-board UART. Lets the
              // bench drive ANY Sense INPUT_* from the reliable LCD USB, so the
              // flaky Sense USB is never needed for command injection. The Sense
              // relays its own diagnostics back as SENSE_DIAG, which print here.
              const char* payload = usb_buf + 5;
              Serial.printf("[USB_CMD] sfwd -> sense: %s\n", payload);
              uart_send_json(payload);
            } else if (strcmp(usb_buf, "voicestart") == 0) {
              // Emulate the mic long-press start on the Sense (begins an OP_VOICE
              // recording). Pair with 'voicestop'. The Sense bridges capture
              // stats back as SENSE_DIAG [voice] record_done (peak/avg/nz).
              Serial.println("[USB_CMD] shortcut 'voicestart' -> INPUT_LONG_PRESS_START");
              uart_send_input_message("INPUT_LONG_PRESS_START");
            } else if (strcmp(usb_buf, "voicestop") == 0) {
              Serial.println("[USB_CMD] shortcut 'voicestop' -> INPUT_LONG_PRESS_END");
              uart_send_input_message("INPUT_LONG_PRESS_END");
            } else if (strcmp(usb_buf, "fwinfo") == 0) {
              // Print the cached Sense fw, then fire a FAST version request.
              // INPUT_SENSE_FW is the same fast type the Settings screen now
              // uses; Sense replies with a FW_INFO message, so the round-trip
              // is observable via the "[UART] FW_INFO received sense_fw=...
              // age_ms=..." log in lcd_uart_rx.h. We send via
              // uart_send_input_message (mirroring the 'ota' command) rather
              // than ship_menu_request_fw_info() to avoid touching the
              // Settings-screen retry/UI state from the UART task context.
              Serial.printf("[USB_CMD] fwinfo cached_sense_fw=%s\n", g_sense_fw_version);
              Serial.println("[USB_CMD] shortcut 'fwinfo' -> INPUT_SENSE_FW");
              uart_send_input_message("INPUT_SENSE_FW");
            } else if (strcmp(usb_buf, "list") == 0) {
              // Emulate tapping List on the second menu. LVGL/screen work MUST run
              // on the UI task (Core 1), so post an event — never call LVGL here.
              Serial.println("[USB] list");
              uart_send_input_message("INPUT_WAKE");
              if (app_event_queue != NULL) {
                app_event_t evt = {};
                evt.type = EVT_USB_ENTER_LIST;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
            } else if (strcmp(usb_buf, "refresh") == 0) {
              // Emulate the pull-to-refresh gesture on the list (UI task handles it).
              Serial.println("[USB] refresh");
              if (app_event_queue != NULL) {
                app_event_t evt = {};
                evt.type = EVT_USB_REFRESH;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
            } else if (strcmp(usb_buf, "pull") == 0) {
              // Emulate the touch pull-to-refresh path explicitly ("usb_pull" reason).
              Serial.println("[USB] pull");
              if (app_event_queue != NULL) {
                app_event_t evt = {};
                evt.type = EVT_USB_PULL;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
            } else if (strncmp(usb_buf, "scroll", 6) == 0 &&
                       (usb_buf[6] == ' ' || usb_buf[6] == '\0')) {
              // 'scroll <±n>' — post EVT_SCROLL_DELTA (selection moves on the
              // list; CCW deltas at the top emulate the overscroll refresh).
              if (usb_buf[6] != ' ') {
                Serial.println("[USB] scroll: usage 'scroll <±n>'");
              } else {
                int delta = atoi(usb_buf + 7);
                Serial.printf("[USB] scroll %d\n", delta);
                if (delta == 0) {
                  Serial.println("[USB] scroll: usage 'scroll <±n>' (n != 0)");
                } else if (app_event_queue != NULL) {
                  if (delta > 127) delta = 127;
                  if (delta < -128) delta = -128;
                  app_event_t evt = {};
                  evt.type = EVT_SCROLL_DELTA;
                  evt.data.scroll_delta = (int8_t)delta;
                  xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
                }
              }
#if defined(HALO_UI_REVIEW) && HALO_UI_REVIEW
            } else if (strcmp(usb_buf, "uilayout") == 0) {
              if (app_event_queue) {
                app_event_t evt = {};
                evt.type = EVT_USB_UI_REVIEW;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
#endif
            } else if (strcmp(usb_buf, "liststate") == 0) {
              // One-line list/refresh state dump, printed from the UI task so
              // the harness sees a coherent snapshot.
              if (app_event_queue != NULL) {
                app_event_t evt = {};
                evt.type = EVT_USB_LISTSTATE;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
            } else if (strcmp(usb_buf, "deltouch") == 0) {
              // 'deltouch' — full touch-path delete for the e2e harness:
              // opens the Delete/Back overlay for the current selection, then
              // (~300ms later, on the UI task) synthesizes a tap at the center
              // of the REAL Delete button via shopping_list_handle_touch, so
              // the derived hitboxes are exercised exactly like a finger.
              Serial.println("[USB] deltouch");
              if (app_event_queue != NULL) {
                app_event_t evt = {};
                evt.type = EVT_USB_DELTOUCH;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
            } else if (strncmp(usb_buf, "del", 3) == 0 &&
                       (usb_buf[3] == ' ' || usb_buf[3] == '\0')) {
              // 'del N' — emulate the DELETE touch on the N-th visible item.
              int del_idx = (usb_buf[3] == ' ') ? atoi(usb_buf + 4) : -1;
              Serial.printf("[USB] del %d\n", del_idx);
              if (del_idx < 0) {
                Serial.println("[USB] del: usage 'del N' (0-based index)");
              } else if (app_event_queue != NULL) {
                app_event_t evt = {};
                evt.type = EVT_USB_DELETE;
                evt.data.usb_index = del_idx;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
            } else if (strcmp(usb_buf, "home") == 0) {
              // Emulate returning to the main menu (UI task handles the screen swap).
              Serial.println("[USB] home");
              if (app_event_queue != NULL) {
                app_event_t evt = {};
                evt.type = EVT_USB_HOME;
                xQueueSend(app_event_queue, &evt, pdMS_TO_TICKS(20));
              }
            } else if (strcmp(usb_buf, "wake") == 0) {
              Serial.println("[USB_CMD] shortcut 'wake' -> INPUT_WAKE");
              uart_send_input_message("INPUT_WAKE");
            } else if (strcmp(usb_buf, "sleep") == 0) {
              Serial.println("[USB_CMD] shortcut 'sleep' -> INPUT_SLEEP");
              uart_send_input_message("INPUT_SLEEP");
            } else if (strcmp(usb_buf, "ping") == 0) {
              Serial.println("[USB_CMD] shortcut 'ping' -> INPUT_PING");
              uart_send_input_message("INPUT_PING");
            } else if (strcmp(usb_buf, "wifi") == 0) {
              Serial.println("[USB_CMD] shortcut 'wifi' -> dumping wifi summaries");
              diag_dump_wifi_summaries(Serial);
            } else if (strcmp(usb_buf, "scan") == 0) {
              Serial.println("[USB_CMD] shortcut 'scan' -> INPUT_WIFI_SCAN");
              uart_send_input_message("INPUT_WIFI_SCAN");
            } else if (strcmp(usb_buf, "wifitest") == 0) {
              Serial.println("[USB_CMD] shortcut 'wifitest' -> INPUT_WIFI_TEST");
              uart_send_input_message("INPUT_WIFI_TEST");
            } else if (strcmp(usb_buf, "diag") == 0) {
              diag_mode = true;
              diag_mode_end_ms = millis() + 300000;  // 5 minutes
              diag_last_ping_ms = 0;
              Serial.println("[USB_CMD] DIAG MODE ON (5 min) - sleep disabled, continuous monitoring");
              uart_send_input_message("INPUT_WAKE");
            } else if (strcmp(usb_buf, "diagoff") == 0) {
              diag_mode = false;
              diag_mode_end_ms = 0;
              Serial.println("[USB_CMD] DIAG MODE OFF");
            } else if (strcmp(usb_buf, "errors") == 0) {
              Serial.println("[USB_CMD] shortcut 'errors' -> dumping error log");
              errlog_dump(Serial);
#if defined(HALO_OTA_BENCH_CASE)
            } else if (strcmp(usb_buf, "otabench") == 0) {
              halo_bench_dump(Serial, g_lcd_ota_uart_receiving.load());
#endif
            } else if (strcmp(usb_buf, "clearerrors") == 0) {
              errlog_clear();
              Serial.println("[USB_CMD] error log cleared");
            } else if (strcmp(usb_buf, "testerrors") == 0) {
              Serial.println("[USB_CMD] injecting test errors into error log...");
              // LCD-side test errors (stored directly)
              lcd_errlog_store_with_context("lcd", "boot", "ABNORMAL_RESET", 3, "test: task watchdog reset");
              lcd_errlog_store_with_context("lcd", "sense_wake", "MISSED_PONGS", 3, "test: sense unresponsive");
              lcd_errlog_store_with_context("lcd", "ota", "OTA_ABORT", -1, "test: sha mismatch");
              Serial.println("[USB_CMD] 3 LCD test errors stored");
              // Tell Sense to inject its test errors via UART
              uart_send_input_message("INPUT_TEST_ERRORS");
              Serial.println("[USB_CMD] sent INPUT_TEST_ERRORS to Sense (expect 4 errors back via UART)");
              Serial.println("[USB_CMD] total: 7 test errors. Type 'errors' to see them, 'clearerrors' to wipe.");
// The three commands below MUTATE the spool, so they must not exist in a shipped
// build. `spoolfill` forges slots that the next drain uploads as real check-ins
// into the owner's kitchen, `spoolclear` deletes captures that have not been
// uploaded yet, and `spoolcaps` evicts. A shipped device has no USB host, but
// "unreachable in practice" is not the same as absent — a command that can
// fabricate or destroy a user's food log has no business being in the binary.
#if HALO_SPOOL_TEST
            } else if (strncmp(usb_buf, "enq", 3) == 0) {
              // "enq [count]" — put user-intent messages on the REAL TX queue.
              //
              // Injecting {"type":"INPUT_MENU_SELECT"} over USB does NOT exercise
              // the deferral path: that handler calls uart_send_json() directly
              // and bypasses uart_tx_queue entirely, so the drain, the
              // awake-proof check and the deferred ring never see it. Every real
              // enqueue site is a touch/encoder UI path, which this bench rig
              // cannot drive (one fixed stylus position).
              //
              // So this goes through uart_tx_enqueue() — the same call the UI
              // makes. Only the trigger is synthetic; the path under test is the
              // production one.
              {
                const char* a = usb_buf + 3;
                while (*a == ' ') a++;
                int n = atoi(a); if (n <= 0) n = 3;
                if (n > 12) n = 12;
                int ok = 0;
                for (int k = 0; k < n; k++) {
                  tx_msg_t m; memset(&m, 0, sizeof(m));
                  snprintf(m.type, sizeof(m.type), "INPUT_MENU_SELECT");
                  m.has_delta = false; m.has_id = false;
                  if (uart_tx_enqueue(&m, "enq_bench")) ok++;
                }
                Serial.printf("[ENQ] queued=%d/%d ring=%u/%u ready=%d\n",
                              ok, n,
                              (unsigned)deferred_ring_count,
                              (unsigned)DEFERRED_AWAKE_RING_SLOTS,
                              sense_ready_for_control_tx() ? 1 : 0);
              }
            } else if (strcmp(usb_buf, "ringstat") == 0) {
              Serial.printf("[RINGSTAT] count=%u/%u oldest=%s dropped=%lu ready=%d\n",
                            (unsigned)deferred_ring_count,
                            (unsigned)DEFERRED_AWAKE_RING_SLOTS,
                            deferred_awake_tx_pending() ? deferred_ring_oldest()->msg.type : "-",
                            (unsigned long)deferred_ring_dropped,
                            sense_ready_for_control_tx() ? 1 : 0);
            } else if (strncmp(usb_buf, "spoolfill", 9) == 0) {
              SdCardLease sd_lease;
              if (!sd_lease || !LCD_SD_SPOOL_ENABLED || !lcd_sd_init()) {
                Serial.println("[SD] command unavailable");
              } else {
                // Bench: forge N small spool slots (+ sidecars) so the caps can be
                // exercised without performing N real captures. The cap logic keys
                // on file COUNT and the sidecar epoch, not on image size, so tiny
                // files test it faithfully.
                //
                // Usage: spoolfill <N> [age_s]
                //   age_s backdates the sidecar epoch so age eviction can be aimed
                //   at a specific subset. `spoolfill 0` really does create nothing —
                //   it used to fall into the `want <= 0` default and forge 45 slots,
                //   which during a bench run silently doubled the card instead of
                //   leaving it alone.
                lcd_sd_init();
                const char* a = usb_buf + 9;
                while (*a == ' ') a++;
                const bool have_arg = (*a >= '0' && *a <= '9');
                int want = have_arg ? atoi(a) : 45;
                uint32_t age_s = 0;
                if (have_arg) {
                  const char* a2 = a;
                  while (*a2 >= '0' && *a2 <= '9') a2++;
                  while (*a2 == ' ') a2++;
                  age_s = (uint32_t)strtoul(a2, NULL, 10);
                }
                if (want < 0) want = 0;
                uint32_t made = 0;
                for (int k = 0; k < want; k++) {
                  const uint32_t slot = lcd_spool_alloc_seq();
                  char ip[64], mp[64];
                  snprintf(ip, sizeof(ip), LCD_SD_SPOOL_DIR "/%lu.jpg",  (unsigned long)slot);
                  snprintf(mp, sizeof(mp), LCD_SD_SPOOL_DIR "/%lu.json", (unsigned long)slot);
                  FILE* f = fopen(ip, "wb");
                  if (!f) break;
                  static uint8_t junk[256]; fwrite(junk, 1, sizeof(junk), f); fclose(f);
                  FILE* m = fopen(mp, "wb");
                  if (m) {
                    char meta[192];
                    const int n = snprintf(meta, sizeof(meta),
                        "{\"job_id\":%lu,\"len\":256,\"mode\":\"filltest\",\"is_voice\":0,"
                        "\"expiry\":\"\",\"qty\":1,\"add_list\":0,\"retries\":0,\"epoch\":%lu}",
                        (unsigned long)slot,
                        (unsigned long)((g_img_rx_meta_epoch ? g_img_rx_meta_epoch : 1787000000UL) - age_s));
                    fwrite(meta, 1, (size_t)n, m); fclose(m);
                  }
                  made++;
                }
                uint32_t tot = 0;
                Serial.printf("[SPOOLFILL] created=%lu now_count=%lu total=%lu\n",
                              (unsigned long)made, (unsigned long)lcd_sd_spool_count(&tot),
                              (unsigned long)tot);
              }
#endif  // HALO_SPOOL_TEST (spoolfill)
#if HALO_FREEZE_TEST
            } else if (strcmp(usb_buf, "freeze") == 0) {
              // Hang uart_task — the case from the header comment, where the LCD
              // stays enumerated but answers nothing and the Sense is talking to
              // a corpse. We are already inside uart_task's loop body, so simply
              // never returning means the feed at the top is never reached again.
              Serial.printf("[FREEZE_TEST] uart_task hanging now — TWDT should reset in ~%ums\n",
                            (unsigned)LCD_FREEZE_WDT_TIMEOUT_MS);
              Serial.flush();
              for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
            } else if (strcmp(usb_buf, "freezeui") == 0) {
              Serial.println("[FREEZE_TEST] arming ui_task hang");
              Serial.flush();
              g_bench_freeze_ui = true;
#endif
            } else if (strcmp(usb_buf, "resetreason") == 0) {
              // After a watchdog reset this is how the freeze stays diagnosable
              // rather than looking like an ordinary power cycle.
              const esp_reset_reason_t rr = esp_reset_reason();
              const char* n = "?";
              switch (rr) {
                case ESP_RST_POWERON:  n = "POWERON";  break;
                case ESP_RST_SW:       n = "SW";       break;
                case ESP_RST_PANIC:    n = "PANIC";    break;
                case ESP_RST_INT_WDT:  n = "INT_WDT";  break;
                case ESP_RST_TASK_WDT: n = "TASK_WDT"; break;
                case ESP_RST_WDT:      n = "OTHER_WDT";break;
                case ESP_RST_DEEPSLEEP:n = "DEEPSLEEP";break;
                case ESP_RST_BROWNOUT: n = "BROWNOUT"; break;
                case ESP_RST_EXT:      n = "EXT";      break;
                // No #ifdef around these: ESP_RST_USB and friends are enum
                // constants, not macros, so `#ifdef ESP_RST_USB` is always false
                // and silently drops the case. That is why a port-open reset kept
                // reporting as "? (11)" instead of USB.
                case ESP_RST_SDIO:     n = "SDIO";      break;
                case ESP_RST_USB:      n = "USB";       break;  // esptool / port open
                case ESP_RST_JTAG:     n = "JTAG";      break;
                case ESP_RST_EFUSE:    n = "EFUSE";     break;
                case ESP_RST_PWR_GLITCH: n = "PWR_GLITCH"; break;
                case ESP_RST_CPU_LOCKUP: n = "CPU_LOCKUP"; break;
                default: break;
              }
              Serial.printf("[RESET_REASON] %s (%d)\n", n, (int)rr);
#if HALO_SPOOL_TEST
            } else if (strncmp(usb_buf, "spoolcaps", 9) == 0) {
              SdCardLease sd_lease;
              if (!sd_lease || !LCD_SD_SPOOL_ENABLED || !lcd_sd_init()) {
                Serial.println("[SD] command unavailable");
              } else {
                // Bench: run the cap enforcement that normally only fires when a new
                // image arrives (lcd_img_rx_begin -> lcd_spool_enforce_caps).
                //
                // This calls the production function, not a copy of it. Reaching it
                // through a real transfer needs a capture whose upload fails while
                // the card is already at 40 slots, which is a lot of setup standing
                // between the test and the one thing under test — eviction order.
                //
                // Usage: spoolcaps [now_epoch]   (default: the last sender epoch)
                lcd_sd_init();
                const char* a = usb_buf + 9;
                while (*a == ' ') a++;
                const uint32_t now_e = (*a >= '0' && *a <= '9')
                                       ? (uint32_t)strtoul(a, NULL, 10)
                                       : g_img_rx_meta_epoch;
                uint32_t before_tot = 0;
                const uint32_t before = lcd_sd_spool_count(&before_tot);
                const uint32_t ev_c0 = g_spool_evicted_count, ev_a0 = g_spool_evicted_age;
                Serial.printf("[SPOOLCAPS] before=%lu now_epoch=%lu max_slots=%d max_age_s=%lu\n",
                              (unsigned long)before, (unsigned long)now_e,
                              (int)LCD_SPOOL_MAX_SLOTS, (unsigned long)LCD_SPOOL_MAX_AGE_S);
                lcd_spool_enforce_caps(now_e);
                uint32_t after_tot = 0;
                const uint32_t after = lcd_sd_spool_count(&after_tot);
                Serial.printf("[SPOOLCAPS] after=%lu evicted_age=%lu evicted_slot=%lu\n",
                              (unsigned long)after,
                              (unsigned long)(g_spool_evicted_age - ev_a0),
                              (unsigned long)(g_spool_evicted_count - ev_c0));

              }            } else if (strcmp(usb_buf, "spoolclear") == 0) {
              SdCardLease sd_lease;
              if (!sd_lease || !LCD_SD_SPOOL_ENABLED || !lcd_sd_init()) {
                Serial.println("[SD] command unavailable");
              } else {
                // Delete every spooled image + sidecar. Needed before testing the
                // automatic drain: the card holds synthetic `spooltest` payloads,
                // and letting the real drain upload those would create junk
                // check-ins in the owner's actual kitchen.
                uint32_t removed = 0;
                // Mount first: the card mounts lazily, so on a fresh boot opendir()
                // fails and this reported "spool dir not open" — which reads as "the
                // card is empty" and silently did nothing while stale images were
                // still there waiting to be drained and uploaded.
                lcd_sd_init();
                // Sweep repeatedly. The victim array holds 32 paths, so a single
                // pass clears at most 32 files — with 45 slots on the card (2 files
                // each) one `spoolclear` left 58 behind while reporting "removed 32",
                // which reads as success. Anything still on the card gets uploaded
                // by the next drain, so a partial clear is the exact failure this
                // command exists to prevent.
                bool sweep_open = true;
                for (uint32_t pass = 0; pass < 16; pass++) {
                  DIR* cd = opendir(LCD_SD_SPOOL_DIR);
                  if (!cd) { sweep_open = (pass > 0); break; }
                  struct dirent* ce;
                  static char victims[32][64];
                  uint32_t nv = 0;
                  while ((ce = readdir(cd)) != NULL && nv < 32) {
                    const char* dot = strrchr(ce->d_name, '.');
                    if (!dot) continue;
                    if (strcmp(dot, ".jpg") && strcmp(dot, ".json") && strcmp(dot, ".part")) continue;
                    snprintf(victims[nv], sizeof(victims[0]), LCD_SD_SPOOL_DIR "/%s", ce->d_name);
                    nv++;
                  }
                  closedir(cd);   // delete AFTER closing: removing entries mid-walk is undefined
                  if (nv == 0) break;
                  for (uint32_t i = 0; i < nv; i++) {
                    if (remove(victims[i]) == 0) removed++;
                  }
                }
                if (!sweep_open) {
                  Serial.println("[SPOOLCLEAR] spool dir not open");
                } else {
                  uint32_t left_tot = 0;
                  Serial.printf("[SPOOLCLEAR] removed %lu file(s), %lu slot(s) left\n",
                                (unsigned long)removed,
                                (unsigned long)lcd_sd_spool_count(&left_tot));
                }
              }
#endif  // HALO_SPOOL_TEST (spoolcaps, spoolclear)
            } else if (strcmp(usb_buf, "linkstats") == 0) {
              link_ack_print_stats();
            } else if (strcmp(usb_buf, "sdtest") == 0) {
              lcd_sd_selftest();
            } else if (strcmp(usb_buf, "spoolverify") == 0) {
              SdCardLease sd_lease;
              if (!sd_lease || !LCD_SD_SPOOL_ENABLED || !lcd_sd_init()) {
                Serial.println("[SD] command unavailable");
              } else {
                // Verify the spooltest image landed byte-for-byte. A matching byte
                // COUNT is what promotes .part -> .jpg, and that is not the same as
                // matching CONTENT: a framing bug that swaps, drops or repeats
                // bytes can preserve length exactly. The Sense fills the test image
                // with (i*31+7)&0xFF, so every byte is checkable.
                // Spool files are named by LCD slot number, not job_id (job_id
                // repeats after a Sense reboot), so verify the newest slot.
                char vpath[64] = {0};
                {
                  uint32_t newest = 0;
                  DIR* vd = opendir(LCD_SD_SPOOL_DIR);
                  if (vd) {
                    struct dirent* ve;
                    while ((ve = readdir(vd)) != NULL) {
                      const char* dot = strrchr(ve->d_name, '.');
                      if (!dot || strcmp(dot, ".jpg") != 0) continue;
                      uint32_t s = (uint32_t)strtoul(ve->d_name, NULL, 10);
                      if (s > newest) newest = s;
                    }
                    closedir(vd);
                  }
                  if (newest) snprintf(vpath, sizeof(vpath), LCD_SD_SPOOL_DIR "/%lu.jpg",
                                       (unsigned long)newest);
                }
                FILE* vf = vpath[0] ? fopen(vpath, "rb") : NULL;
                if (!vf) {
                  Serial.println("[SPOOLVERIFY] no spooled .jpg - run spooltest on the Sense first");
                } else {
                  Serial.printf("[SPOOLVERIFY] checking %s\n", vpath);
                  static uint8_t vbuf[512];
                  size_t voff = 0, vn;
                  long first_bad = -1;
                  uint32_t bad = 0;
                  while ((vn = fread(vbuf, 1, sizeof(vbuf), vf)) > 0) {
                    for (size_t i = 0; i < vn; i++) {
                      if (vbuf[i] != (uint8_t)(((voff + i) * 31 + 7) & 0xFF)) {
                        if (first_bad < 0) first_bad = (long)(voff + i);
                        bad++;
                      }
                    }
                    voff += vn;
                  }
                  fclose(vf);
                  Serial.printf("[SPOOLVERIFY] bytes=%u expected=%u mismatches=%lu first_bad=%ld -> %s\n",
                                (unsigned)voff, (unsigned)(180 * 1024), (unsigned long)bad, first_bad,
                                (voff == 180 * 1024 && bad == 0) ? "PASS" : "FAIL");
                }

              }            } else if (strcmp(usb_buf, "testmode") == 0) {
              test_mode_arm(TEST_MODE_BUDGET_MS);
              resetActivityTimer();
              Serial.println("[TEST_MODE] enabled (1 hour of running time, survives reboot, not power-off)");
            } else if (strcmp(usb_buf, "testmodeoff") == 0) {
              test_mode_clear("usb command");
            } else if (strcmp(usb_buf, "factoryreset") == 0) {
              Serial.println("[FACTORY_RESET] Clearing all provisioning and test data...");
              // Clear provisioning data (WiFi SSID/pass, owner, provisioned flag)
              {
                nvs_handle_t h;
                if (nvs_open("provisioning", NVS_READWRITE, &h) == ESP_OK) {
                  nvs_erase_all(h);
                  nvs_commit(h);
                  nvs_close(h);
                }
                Serial.println("[FACTORY_RESET] Provisioning data cleared");
              }
              // Clear test mode
              {
                Preferences prefs;
                if (prefs.begin("test_cfg", false)) {
                  prefs.clear();
                  prefs.end();
                }
                test_mode_clear("factory reset");  // also wipes the RTC budget
                Serial.println("[FACTORY_RESET] Test mode cleared");
              }
              // Clear error log
              errlog_clear();
              Serial.println("[FACTORY_RESET] Error log cleared");
              // Clear OTA state
              {
                Preferences prefs;
                if (prefs.begin("lcd_ota", false)) {
                  prefs.clear();
                  prefs.end();
                }
                Serial.println("[FACTORY_RESET] OTA state cleared");
              }
              // Clear WiFi creds stored on LCD side
              {
                Preferences prefs;
                if (prefs.begin("wifi_cfg", false)) {
                  prefs.clear();
                  prefs.end();
                }
                Serial.println("[FACTORY_RESET] WiFi config cleared");
              }
              // Clear persistent list/menu data
              {
                Preferences prefs;
                if (prefs.begin("halo_list", false)) {
                  prefs.clear();
                  prefs.end();
                }
                Serial.println("[FACTORY_RESET] List data cleared");
              }
              Serial.println("[FACTORY_RESET] Complete. Device is ready for customer provisioning.");
              Serial.println("[FACTORY_RESET] Power cycle or let device sleep to finalize.");
            } else if (strcmp(usb_buf, "wake") == 0) {
              Serial.println("[USB_CMD] Force-waking Sense...");
              request_sense_wake("usb_force_wake");
              resetActivityTimer();
              Serial.printf("[WAKE] Pulse sent, sense_state=%d awake_confirmed=%d\n",
                            (int)sense_state, sense_awake_confirmed ? 1 : 0);
            } else if (strcmp(usb_buf, "ui") == 0) {
              Serial.printf("[UI_STATE] screen=%d ota_screen=%d ota_locked=%d lvgl_running=%d "
                            "sleep_transition=%d idle_dark=%d backlight=%d "
                            "ui_task=%s test_mode=%d manual_ota=%d\n",
                            (int)ui_screen_state, g_ota_screen_active ? 1 : 0,
                            ota_locked ? 1 : 0, g_lvgl_running ? 1 : 0,
                            g_sleep_transition ? 1 : 0, g_idle_screen_dark ? 1 : 0,
                            g_backlight_duty,
                            ui_task_handle ? "alive" : "dead",
                            g_test_mode_active ? 1 : 0,
                            g_manual_ota_override ? 1 : 0);
            } else if (strcmp(usb_buf, "help") == 0) {
              Serial.println("[USB_CMD] Available commands:");
              Serial.println("  ota   - trigger OTA check");
              Serial.println("  fwinfo - print cached Sense fw + fast version request (INPUT_SENSE_FW)");
              Serial.println("  wake  - send INPUT_WAKE");
              Serial.println("  sleep - send INPUT_SLEEP");
              Serial.println("  ping  - send INPUT_PING");
              Serial.println("  wifi     - dump WiFi summaries");
              Serial.println("  scan     - WiFi network scan (via Sense)");
              Serial.println("  wifitest - WiFi cold-start test (disconnect+scan+reconnect)");
              Serial.println("  diag     - enable diagnostic mode (5 min, no sleep)");
              Serial.println("  diagoff  - disable diagnostic mode");
              Serial.println("  errors      - dump error log");
              Serial.println("  clearerrors - clear error log");
              Serial.println("  testerrors  - inject test errors (7 entries, tests full pipeline)");
              Serial.println("  testmode    - disable sleep for 1h of running time (survives reboot; cleared by power-off/OTA)");
              Serial.println("  testmodeoff - disable test mode, clear NVS flag");
              Serial.println("  ui         - dump UI state (screen, OTA flags, panel, task)");
              Serial.println("  list       - enter the shopping list screen (auto-revalidates)");
              Serial.println("  refresh    - trigger a list refresh (usb_refresh)");
              Serial.println("  pull       - trigger the pull-to-refresh path (usb_pull)");
              Serial.println("  scroll <±n> - post a scroll delta (selection move / CCW overscroll)");
              Serial.println("  del N      - delete the N-th visible list item (0-based)");
              Serial.println("  deltouch   - full touch-path delete: open overlay, tap real Delete button");
              Serial.println("  liststate  - one-line [LISTSTATE] JSON dump (e2e harness)");
              Serial.println("  home       - return to the main menu");
              Serial.println("  help     - show this help");
              Serial.println("  {\"type\":\"INPUT_*\",...} - send JSON command");

            // --- JSON commands ---
            } else if (usb_buf[0] == '{') {
              StaticJsonDocument<512> cmd_doc;
              DeserializationError err = deserializeJson(cmd_doc, usb_buf);
              if (err) {
                Serial.printf("[USB_CMD] JSON parse error: %s\n", err.c_str());
              } else {
                const char* cmd_type = cmd_doc["type"] | (const char*)nullptr;
                if (!cmd_type) {
                  Serial.println("[USB_CMD] WARNING: JSON missing 'type' field");
                } else if (strcmp(cmd_type, "INPUT_MENU_SELECT") == 0) {
                  // INPUT_MENU_SELECT needs full JSON with menu_item, menu_index
                  StaticJsonDocument<512> fwd_doc;
                  const uint32_t fwd_msg_id = get_next_msg_id();
                  fwd_doc["type"]       = "INPUT_MENU_SELECT";
                  fwd_doc["ver"]        = PROTOCOL_VERSION;
                  fwd_doc["msg_id"]     = fwd_msg_id;
                  fwd_doc["ts"]         = millis();
                  if (cmd_doc.containsKey("menu_item"))
                    fwd_doc["menu_item"]  = cmd_doc["menu_item"];
                  if (cmd_doc.containsKey("menu_index"))
                    fwd_doc["menu_index"] = cmd_doc["menu_index"];
                  char fwd_buf[512];
                  serializeJson(fwd_doc, fwd_buf, sizeof(fwd_buf));
                  Serial.printf("[USB_CMD] forwarding INPUT_MENU_SELECT via uart_send_json: %s\n", fwd_buf);
                  uart_send_json(fwd_buf);
                  // Track it like the real touch path does. Without this the
                  // bench harness would exercise an untracked send and prove
                  // nothing about the retransmit layer that ships.
                  link_ack_track(fwd_msg_id, "INPUT_MENU_SELECT", fwd_buf);
                } else if (strcmp(cmd_type, "INPUT_DISCARD_OPTIONS") == 0 ||
                           strcmp(cmd_type, "INPUT_EXPIRY_DATE") == 0) {
                  // These need full JSON with payload fields
                  StaticJsonDocument<512> fwd_doc;
                  fwd_doc["type"]    = cmd_type;
                  fwd_doc["ver"]     = PROTOCOL_VERSION;
                  fwd_doc["msg_id"]  = get_next_msg_id();
                  fwd_doc["ts"]      = millis();
                  // Copy all payload fields
                  for (JsonPair kv : cmd_doc.as<JsonObject>()) {
                    const char* k = kv.key().c_str();
                    if (strcmp(k, "type") != 0 && strcmp(k, "ver") != 0 &&
                        strcmp(k, "msg_id") != 0 && strcmp(k, "ts") != 0) {
                      fwd_doc[k] = kv.value();
                    }
                  }
                  char fwd_buf[512];
                  serializeJson(fwd_doc, fwd_buf, sizeof(fwd_buf));
                  Serial.printf("[USB_CMD] forwarding %s via uart_send_json: %s\n", cmd_type, fwd_buf);
                  uart_send_json(fwd_buf);
                } else if (strncmp(cmd_type, "INPUT_", 6) == 0) {
                  // All other INPUT_* types — simple forward
                  Serial.printf("[USB_CMD] forwarding %s via uart_send_input_message\n", cmd_type);
                  uart_send_input_message(cmd_type);
                } else if (strcmp(cmd_type, "OTA_CHECK") == 0) {
                  // Non-INPUT type that needs full JSON forwarding
                  StaticJsonDocument<512> fwd_doc;
                  fwd_doc["type"]   = "OTA_CHECK";
                  fwd_doc["ver"]    = PROTOCOL_VERSION;
                  fwd_doc["msg_id"] = get_next_msg_id();
                  fwd_doc["ts"]     = millis();
                  if (cmd_doc.containsKey("reason"))
                    fwd_doc["reason"] = cmd_doc["reason"];
                  if (cmd_doc.containsKey("allow_reboot"))
                    fwd_doc["allow_reboot"] = cmd_doc["allow_reboot"];
                  char fwd_buf[512];
                  serializeJson(fwd_doc, fwd_buf, sizeof(fwd_buf));
                  Serial.printf("[USB_CMD] forwarding OTA_CHECK via uart_send_json: %s\n", fwd_buf);
                  uart_send_json(fwd_buf);
                } else {
                  Serial.printf("[USB_CMD] WARNING: unrecognized type '%s'\n", cmd_type);
                }
              }

            // --- Unrecognized plain text ---
            } else {
              Serial.printf("[USB_CMD] WARNING: unrecognized command '%s' (try 'help')\n", usb_buf);
            }

            usb_pos = 0;
          }
        } else if (usb_pos < (int)(sizeof(usb_buf) - 1)) {
          usb_buf[usb_pos++] = c;
        } else {
          // Buffer overflow — discard
          Serial.println("[USB_CMD] WARNING: input too long, discarding");
          usb_pos = 0;
        }
      }
    }

    lcd_id1_service(); // one local USB opportunity; no wait or activity change

    // 4) Diagnostic mode service — keep awake and ping Sense every 2s
    if (diag_mode) {
      unsigned long now_dm = millis();
      if (now_dm >= diag_mode_end_ms) {
        diag_mode = false;
        Serial.println("[DIAG] mode expired (5 min)");
      } else if ((now_dm - diag_last_ping_ms) >= 2000) {
        diag_last_ping_ms = now_dm;
        // Reset LCD idle timer to prevent sleep
        last_user_activity_ms = now_dm;
        // Ping Sense to keep it awake and get PONG response
        uart_send_input_message("INPUT_PING");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(yielded_early ? 1 : 5));
  }
}

#endif // LCD_UART_TASK_H
