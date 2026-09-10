#ifndef HALO_UART_JSON_PROTOCOL_H
#define HALO_UART_JSON_PROTOCOL_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

namespace HaloUartJson {

static constexpr int kProtocolVersion = 1;

static constexpr const char kFieldVersion[] = "ver";
static constexpr const char kFieldType[] = "type";
static constexpr const char kFieldMsgId[] = "msg_id";
static constexpr const char kFieldTimestamp[] = "ts";

// LCD -> Sense
static constexpr const char kTypeSync[] = "SYNC";
static constexpr const char kTypeInputPing[] = "INPUT_PING";
static constexpr const char kTypeInputWake[] = "INPUT_WAKE";
static constexpr const char kTypeInputScroll[] = "INPUT_SCROLL";
static constexpr const char kTypeInputTouch[] = "INPUT_TOUCH";
static constexpr const char kTypeInputLongPressStart[] = "INPUT_LONG_PRESS_START";
static constexpr const char kTypeInputLongPressEnd[] = "INPUT_LONG_PRESS_END";
static constexpr const char kTypeInputDelete[] = "INPUT_DELETE";
static constexpr const char kTypeInputMenuPress[] = "INPUT_MENU_PRESS";
static constexpr const char kTypeInputSleep[] = "INPUT_SLEEP";
static constexpr const char kTypeInputResetWifi[] = "INPUT_RESET_WIFI";
static constexpr const char kTypeInputFwInfo[] = "INPUT_FW_INFO";
static constexpr const char kTypeInputOtaCheck[] = "INPUT_OTA_CHECK";
static constexpr const char kTypeInputMenuSelect[] = "INPUT_MENU_SELECT";
static constexpr const char kTypeInputExpiryDate[] = "INPUT_EXPIRY_DATE";
static constexpr const char kTypeInputDiscardOptions[] = "INPUT_DISCARD_OPTIONS";
static constexpr const char kTypeInputRetry[] = "INPUT_RETRY";
static constexpr const char kTypeMaintWindowAck[] = "MAINT_WINDOW_ACK";
static constexpr const char kTypeWifiStatus[] = "WIFI_STATUS";
static constexpr const char kTypeWifiCredsAck[] = "WIFI_CREDS_ACK";
static constexpr const char kTypeWifiOnAck[] = "WIFI_ON_ACK";
static constexpr const char kTypeOtaCheckAck[] = "OTA_CHECK_ACK";
static constexpr const char kTypeOtaCheckResult[] = "OTA_CHECK_RESULT";
static constexpr const char kTypeLcdOtaDone[] = "LCD_OTA_DONE";
static constexpr const char kTypeSleepDeny[] = "SLEEP_DENY";
static constexpr const char kTypeLcdDiag[] = "LCD_DIAG";

// Sense -> LCD
static constexpr const char kTypeSyncAck[] = "SYNC_ACK";
static constexpr const char kTypePong[] = "PONG";
static constexpr const char kTypeLinkHb[] = "LINK_HB";
static constexpr const char kTypeUiList[] = "UI_LIST";
static constexpr const char kTypeUiStatus[] = "UI_STATUS";
static constexpr const char kTypeUiMealResult[] = "UI_MEAL_RESULT";
static constexpr const char kTypeUiToast[] = "UI_TOAST";
static constexpr const char kTypeUiVoiceResponse[] = "UI_VOICE_RESPONSE";
static constexpr const char kTypeSenseDiag[] = "SENSE_DIAG";
static constexpr const char kTypeSenseSleepIntent[] = "SENSE_SLEEP_INTENT";
static constexpr const char kTypeSleepReady[] = "SLEEP_READY";
static constexpr const char kTypeReleaseWake[] = "RELEASE_WAKE";
static constexpr const char kTypeFwInfo[] = "FW_INFO";
static constexpr const char kTypeStatusSync[] = "STATUS_SYNC";
static constexpr const char kTypeMaintWindow[] = "MAINT_WINDOW";
static constexpr const char kTypeWifiCreds[] = "WIFI_CREDS";
static constexpr const char kTypeWifiOn[] = "WIFI_ON";
static constexpr const char kTypeOtaLock[] = "OTA_LOCK";
static constexpr const char kTypeOtaUnlock[] = "OTA_UNLOCK";
static constexpr const char kTypeOtaCheck[] = "OTA_CHECK";
static constexpr const char kTypeProvisionQr[] = "PROVISION_QR";
static constexpr const char kTypeProvisionStatus[] = "PROVISION_STATUS";
static constexpr const char kTypeSenseOtaActive[] = "SENSE_OTA_ACTIVE";
static constexpr const char kTypeSenseOtaIdle[] = "SENSE_OTA_IDLE";

// Legacy / compatibility messages still recognized by production LCD code.
static constexpr const char kTypeSleepAck[] = "SLEEP_ACK";
static constexpr const char kTypeInputSleepAck[] = "INPUT_SLEEP_ACK";
static constexpr const char kTypeSleepBusy[] = "SLEEP_BUSY";
static constexpr const char kTypeUiVoiceItems[] = "UI_VOICE_ITEMS";
static constexpr const char kTypeOtaApplyRequired[] = "OTA_APPLY_REQUIRED";

enum class EnvelopeError {
  kOk = 0,
  kMissingVersion,
  kInvalidVersion,
  kMissingType,
  kMissingMsgId,
  kMissingTimestamp,
};

inline EnvelopeError validateEnvelope(const JsonDocument& doc, int* actual_version = nullptr) {
  if (!doc.containsKey(kFieldVersion)) {
    return EnvelopeError::kMissingVersion;
  }
  const int ver = doc[kFieldVersion] | 0;
  if (actual_version) {
    *actual_version = ver;
  }
  if (ver != kProtocolVersion) {
    return EnvelopeError::kInvalidVersion;
  }
  if (!doc.containsKey(kFieldType)) {
    return EnvelopeError::kMissingType;
  }
  if (!doc.containsKey(kFieldMsgId)) {
    return EnvelopeError::kMissingMsgId;
  }
  if (!doc.containsKey(kFieldTimestamp)) {
    return EnvelopeError::kMissingTimestamp;
  }
  return EnvelopeError::kOk;
}

template <typename TDoc>
inline void initEnvelope(TDoc& doc, const char* type, uint32_t msg_id, uint32_t ts_ms) {
  doc[kFieldVersion] = kProtocolVersion;
  doc[kFieldType] = type;
  doc[kFieldMsgId] = msg_id;
  doc[kFieldTimestamp] = ts_ms;
}

inline const char* getType(const JsonDocument& doc) {
  return doc[kFieldType] | "";
}

inline bool isInputType(const char* type) {
  return type && strncmp(type, "INPUT_", 6) == 0;
}

inline bool isUserActionInputType(const char* type) {
  return isInputType(type) &&
         strcmp(type, kTypeInputSleep) != 0 &&
         strcmp(type, kTypeInputPing) != 0;
}

}  // namespace HaloUartJson

#endif  // HALO_UART_JSON_PROTOCOL_H
