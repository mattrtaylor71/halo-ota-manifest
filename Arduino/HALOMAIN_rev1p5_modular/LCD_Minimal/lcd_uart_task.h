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

static void uart_task(void *arg) {
  Serial.println("[UART] UART task started");
  
  for (;;) {
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
    if (deferred_awake_tx_valid) {
      if (sense_ready_for_control_tx()) {
        tx_msg_send_now(&deferred_awake_tx_msg);
        deferred_awake_tx_valid = false;
        deferred_awake_tx_last_ping_ms = 0;
        tx_count++;
      } else {
        deferred_awake_tx_service();
        yielded_early = true;
      }
    }
    if (!deferred_awake_tx_valid) {
      while (uart_tx_queue != NULL && xQueueReceive(uart_tx_queue, &tx_msg, 0) == pdTRUE) {
        if (tx_msg_requires_awake_proof(&tx_msg) && !sense_ready_for_control_tx()) {
          deferred_awake_tx_msg = tx_msg;
          deferred_awake_tx_valid = true;
          deferred_awake_tx_last_ping_ms = 0;
          Serial.printf("[UART] deferred_until_awake type=%s awake=%d synced=%d recent=%d\n",
                        tx_msg.type,
                        sense_awake_confirmed ? 1 : 0,
                        link_synced ? 1 : 0,
                        sense_recently_heard(SENSE_CONTROL_READY_WINDOW_MS) ? 1 : 0);
          deferred_awake_tx_service();
          yielded_early = true;
          break;
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
      
      if (c == '\n' || c == '\r') {
        if (uart_rx_line_pos > 0) {
          uart_rx_line_buffer[uart_rx_line_pos] = '\0';
          uart_rx_completed_lines_seen++;
          uart_process_received_message(uart_rx_line_buffer);
          uart_rx_line_pos = 0;
          uart_rx_partial_started_ms = 0;
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
      Serial.printf("[UART_DIAG] partial_line age_ms=%lu len=%d preview=%s\n",
                    now_ms - uart_rx_partial_started_ms,
                    uart_rx_line_pos,
                    preview);
      uart_rx_last_partial_log_ms = now_ms;
    }
    if ((deferred_awake_tx_valid || uart_rx_valid_msgs_seen == 0) &&
        (now_ms - uart_rx_last_summary_ms) >= 1000) {
      int available_now = senseSerial.available();
      unsigned long proof_age_ms = last_proof_of_life_ms > 0 ? (now_ms - last_proof_of_life_ms) : 0xFFFFFFFFUL;
      unsigned long rx_age_ms = last_sense_rx_ms > 0 ? (now_ms - last_sense_rx_ms) : 0xFFFFFFFFUL;
      Serial.printf("[UART_DIAG] summary avail=%d rx_gpio=%d raw_bytes=%lu lines=%lu valid=%lu line_pos=%d awake=%d synced=%d rx_age_ms=%lu proof_age_ms=%lu deferred=%d\n",
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
                    deferred_awake_tx_valid ? 1 : 0);
      uart_rx_last_summary_ms = now_ms;
    }
    
    vTaskDelay(pdMS_TO_TICKS(yielded_early ? 1 : 5));
  }
}

#endif // LCD_UART_TASK_H
