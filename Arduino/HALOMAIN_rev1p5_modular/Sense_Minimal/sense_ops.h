/*
 * sense_ops.h
 *
 * Operation types, structs, and enums for Sense_Minimal.
 * Defines the OpJob pipeline types used by the operation manager.
 *
 * Extracted from Sense_Minimal.ino as modularization Step 2.
 */

#ifndef SENSE_OPS_H
#define SENSE_OPS_H

// ── Operation Manager types ──────────────────────────────────────────

enum OpType { OP_VOICE, OP_SCAN, OP_LIST_REFRESH };
enum OpPriority { PRI_USER, PRI_BG };
enum OpState { OP_IDLE, OP_RECORDING, OP_FINALIZE, OP_UPLOAD, OP_PARSE, OP_APPLY, OP_CAPTURED_READY, OP_DONE };

struct OpJob {
  OpType type;
  OpPriority pri;
  uint32_t job_id;
  uint32_t created_ts;
  OpState state;
  bool active;
  char mode[16];  // "dish", "discard", or "check-in" for SCAN operations, empty for others
  char expiry_date[16];  // "YYYY-MM-DD" format for check-in mode
  uint16_t quantity;
  bool add_to_shopping_list;
};

struct UploadJob {
  uint32_t job_id;
  bool is_voice;
  char mode[16];
  char expiry_date[16];
  uint16_t quantity;
  bool add_to_shopping_list;
  uint8_t* image_buf;
  size_t image_len;
  uint8_t retries;
  uint32_t created_ms;
  uint32_t created_epoch;
  bool from_persisted;
  struct CameraUploadMeta {
    uint8_t profile;
    uint8_t flash_enabled;
    uint8_t jpeg_quality;
    uint8_t reserved0;
    uint16_t actual_width;
    uint16_t actual_height;
    uint16_t configured_framesize;
    int16_t scene_luma;
    int16_t scene_green_ratio;
    uint32_t xclk_hz;
  } camera_meta;
};

// ── Operation type label ─────────────────────────────────────────────

static const char* op_type_name(OpType type) {
  switch (type) {
    case OP_VOICE:
      return "voice";
    case OP_SCAN:
      return "scan";
    case OP_LIST_REFRESH:
      return "list_refresh";
    default:
      return "unknown";
  }
}

#endif // SENSE_OPS_H
