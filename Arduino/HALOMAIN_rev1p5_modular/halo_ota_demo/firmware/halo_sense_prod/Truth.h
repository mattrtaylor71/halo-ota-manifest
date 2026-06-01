// Truth logger: Single-line system state dump
// 
// Format:
// [TRUTH] reason=<reason> fw=<version> build=<build_id> prov_state=<state> provisioned=<0/1> owner=<0/1>
// setup=<0/1> wifi=<status> ip=<ip> rssi=<int> part=<app0/app1> boot=<app0/app1> ota_en=<0/1>
// ship=<0/1> ota_apply=<0/1> why=<reason> ota_guard=<reason> ota_state=<state> manifest=<ok|none|err>
// mver=<version|-> uart=<0/1> mqtt=<connected|disconnected> mqtt_last_rx_ms=<ms> mqtt_last_pub_ms=<ms>
// mqtt_fail_count=<n> mqtt_pub_inflight=<n> mqtt_cmd_q=<n> desired_sense=<ver|-> desired_lcd=<ver|->
// desired_force=<0/1> desired_age_s=<s> lcd_fw=<ver|-> lcd_fw_age_s=<s|-> ota_intent=<0/1> ota_sched_enable=<0/1> next_ota_epoch=<epoch>
// wake_cause=<int> wake_cause_str=<str> maintenance_mode=<0/1> maintenance_in_window=<0/1>
// maint_scheduled=<0/1> maint_start_epoch=<epoch> maint_end_epoch=<epoch> maint_wake_epoch=<epoch>
// boot_count=<n> reboot_loop=<0/1> uptime_ms=<ms> reset_reason=<str> heap_free=<bytes> heap_min=<bytes> heap_largest=<bytes>
// health=<ok|warn> health_flags=<csv|-> lcd_ota_result=<str|-> lcd_maint_ack=<0/1> lcd_maint_ack_age_s=<s|->
// maint_sync_pending=<0/1> maint_sync_attempts=<n>

#ifndef TRUTH_H
#define TRUTH_H

#include <Arduino.h>

// Forward declarations
struct OtaUrlConfig;
struct TruthManifestState;

// OTA config accessor (defined in Truth.cpp)
const OtaUrlConfig* ota_get_config();
OtaUrlConfig* ota_get_config_mutable();  // Mutable accessor for resolveOtaManifestUrl

// OTA status functions (defined in Truth.cpp)
const char* ota_disabled_reason();
bool ota_is_enabled();

// Manifest state tracking
struct TruthManifestState {
  enum Status { NONE, OK, ERR };
  Status status;
  char version[32];
  
  TruthManifestState();
  void setOk(const char* ver);
  void setErr();
  void reset();
};

// Manifest state accessor (defined in Truth.cpp)
TruthManifestState& truth_get_manifest_state();

// UART sync helpers (defined in Truth.cpp)
namespace Truth {
  bool getUartSyncEstablished();
  void setUartSyncEstablished(bool value);
  void evaluateOtaApply(bool& apply_allowed, char* why_buf, size_t why_buf_size);
}

// Main TRUTH logger function (defined in Truth.cpp)
void dump_system_truth(const char* reason);

// Build TRUTH JSON payload
bool build_truth_json(const char* reason, char* out, size_t out_len);

// Build DIAG JSON payload (pre-sleep diagnostics)
bool build_diag_json(const char* reason, char* out, size_t out_len);

// Maintenance policy (weak in Truth.cpp; override in Sense firmware)
// maintenance_mode: true when we purposely woke for maintenance
// maintenance_in_window: true when current time is within scheduled window (epoch + schedule + grace)
bool truth_get_maintenance_mode();
bool truth_get_maintenance_in_window();
const char* truth_get_sched_fetch_state();
int32_t truth_get_sched_fetch_http_code();
int32_t truth_get_sched_fetch_age_s();
const char* truth_get_sched_fetch_request_id();
const char* truth_get_sched_last_event();
int32_t truth_get_sched_last_event_age_s();
const char* truth_get_sched_last_event_request_id();
const char* truth_get_lcd_fw_version();
int32_t truth_get_lcd_fw_age_s();
const char* truth_get_lcd_running_state();
bool truth_get_lcd_maint_ack();
int32_t truth_get_lcd_maint_ack_age_s();
int32_t truth_get_lcd_maint_ack_remaining_s();
int32_t truth_get_lcd_maint_ack_wake_in_s();
const char* truth_get_lcd_maint_ack_request_id();
const char* truth_get_lcd_maint_ack_status();
bool truth_get_lcd_maint_ack_persisted();
uint64_t truth_get_lcd_maint_ack_start_epoch();
uint32_t truth_get_lcd_maint_ack_duration_sec();
uint32_t truth_get_lcd_maint_ack_grace_before_sec();
uint32_t truth_get_lcd_maint_ack_grace_after_sec();
bool truth_get_maint_sync_pending();
uint32_t truth_get_maint_sync_attempts();
const char* truth_get_maint_sync_resolution();
const char* truth_get_maint_last_tx_request_id();
int32_t truth_get_maint_last_tx_age_s();
int32_t truth_get_maint_last_tx_remaining_s();
int32_t truth_get_maint_last_tx_wake_in_s();
bool truth_get_maint_last_tx_clear();
bool truth_get_maint_last_tx_link_recent();
uint32_t truth_get_boot_count();
bool truth_get_reboot_loop_detected();
uint32_t truth_get_uart_tx_count();
uint32_t truth_get_uart_rx_count();
const char* truth_get_last_uart_tx_type();
const char* truth_get_last_uart_rx_type();
const char* truth_get_last_action();
int32_t truth_get_last_action_age_ms();
const char* truth_get_lcd_diag_wake();
const char* truth_get_lcd_diag_screen();
const char* truth_get_lcd_diag_input();
const char* truth_get_lcd_diag_last_tx();
const char* truth_get_lcd_diag_last_rx();
int32_t truth_get_lcd_diag_input_age_ms();
int32_t truth_get_lcd_diag_sense_rx_age_ms();
uint32_t truth_get_lcd_diag_uart_tx();
uint32_t truth_get_lcd_diag_uart_rx();
int32_t truth_get_lcd_diag_age_ms();
const char* truth_get_last_error_stage();
int32_t truth_get_last_error_code();
const char* truth_get_last_error_text();
int32_t truth_get_last_error_age_ms();
const char* truth_get_recent_action_log();
const char* truth_get_camera_timeline();
uint32_t truth_get_upload_cache_count();
const char* truth_get_upload_cache_last_result();
const char* truth_get_upload_cache_last_reason();
int32_t truth_get_upload_cache_last_age_ms();
uint32_t truth_get_upload_cache_last_retries();
const char* truth_get_last_stage();
int32_t truth_get_last_stage_code();
int32_t truth_get_last_stage_uptime_ms();
int32_t truth_get_prev_clean_shutdown();
uint32_t truth_get_crash_count();
const char* truth_get_last_crash_stage();
int32_t truth_get_last_crash_stage_code();
int32_t truth_get_last_crash_stage_uptime_ms();
int32_t truth_get_last_crash_reason();
int32_t truth_get_last_crash_wake_cause();

#endif // TRUTH_H
