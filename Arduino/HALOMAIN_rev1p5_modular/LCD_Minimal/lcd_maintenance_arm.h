#ifndef LCD_MAINTENANCE_ARM_H
#define LCD_MAINTENANCE_ARM_H

class LcdMaintenanceStorageGuard {
 public:
  LcdMaintenanceStorageGuard() { xSemaphoreTakeRecursive(mutex(), portMAX_DELAY); }
  ~LcdMaintenanceStorageGuard() { xSemaphoreGiveRecursive(mutex()); }
 private:
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  friend class LcdMaintenanceStorageTryGuard;
#endif
  static SemaphoreHandle_t mutex() {
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t handle = xSemaphoreCreateRecursiveMutexStatic(&storage);
    return handle;
  }
};

#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
// Optional diagnostics join the existing OTA/arm/sleep owner without waiting.
class LcdMaintenanceStorageTryGuard {
 public:
  bool acquire() {
    if (owned_) return true;
    owned_ = xSemaphoreTakeRecursive(LcdMaintenanceStorageGuard::mutex(), 0) == pdTRUE;
    return owned_;
  }
  ~LcdMaintenanceStorageTryGuard() {
    if (owned_) xSemaphoreGiveRecursive(LcdMaintenanceStorageGuard::mutex());
  }
  LcdMaintenanceStorageTryGuard() = default;
  LcdMaintenanceStorageTryGuard(const LcdMaintenanceStorageTryGuard&) = delete;
  LcdMaintenanceStorageTryGuard& operator=(const LcdMaintenanceStorageTryGuard&) = delete;
 private:
  bool owned_ = false;
};
#endif

// One NVS value is the authoritative arm OR disarm. Never fall back to old
// per-key state when this key exists but is invalid: that resurrects old arms.
struct LcdMaintenanceArm {
  uint32_t magic, format, armed, wake_s, remaining_s, start_epoch;
  uint32_t duration_s, grace_before_s, grace_after_s;
  char request_id[64];
  uint32_t crc;
};
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS && HALO_LCD_SLEEP_WITNESS
static void lcd_sleep_witness_arm_committed(const LcdMaintenanceArm& arm);
#endif
static bool g_lcd_arm_storage_fault = false;
static const char* LCD_MAINT_ARM_KEY = "arm_v1";
static const uint32_t LCD_MAINT_ARM_MAGIC = 0x4c4d4131UL;
static uint32_t lcd_arm_crc(const LcdMaintenanceArm& a) {
  uint32_t crc = 0xffffffffUL;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&a);
  for (size_t i = 0; i < offsetof(LcdMaintenanceArm, crc); ++i) {
    crc ^= p[i];
    for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320UL & (0UL - (crc & 1)));
  }
  return ~crc;
}
static void lcd_arm_seal(LcdMaintenanceArm& a) {
  a.magic = LCD_MAINT_ARM_MAGIC; a.format = 1; a.crc = lcd_arm_crc(a);
}
static bool lcd_arm_valid(const LcdMaintenanceArm& a) {
  if (a.magic != LCD_MAINT_ARM_MAGIC || a.format != 1 || a.armed > 1 ||
      !memchr(a.request_id, 0, sizeof(a.request_id)) || a.crc != lcd_arm_crc(a)) return false;
  if (!a.armed) {
    return !a.wake_s && !a.remaining_s && !a.start_epoch && !a.duration_s &&
           !a.grace_before_s && !a.grace_after_s && !a.request_id[0];
  }
  if (!a.wake_s || a.wake_s > INT32_MAX / 1000UL ||
      a.remaining_s > INT32_MAX / 1000UL) return false;
  if (a.start_epoch && a.start_epoch < 1700000000UL) return false;
  return uint64_t(a.start_epoch) + a.duration_s + a.grace_after_s <= UINT32_MAX;
}
static LcdMaintenanceArm lcd_arm_from_ram() {
  LcdMaintenanceArm a = {};
  a.armed = g_lcd_maintenance_timer_armed ? 1 : 0;
  if (a.armed) {
    a.wake_s = g_lcd_maintenance_wake_in_s;
    a.remaining_s = g_lcd_maintenance_remaining_s;
    a.start_epoch = (uint32_t)g_lcd_maintenance_start_epoch;
    a.duration_s = g_lcd_maintenance_duration_sec;
    a.grace_before_s = g_lcd_maintenance_grace_before_sec;
    a.grace_after_s = g_lcd_maintenance_grace_after_sec;
    strlcpy(a.request_id, g_lcd_maintenance_request_id, sizeof(a.request_id));
  }
  lcd_arm_seal(a); return a;
}
static void lcd_arm_publish(const LcdMaintenanceArm& a) {
  g_lcd_maintenance_timer_armed = a.armed;
  g_lcd_maintenance_wake_in_s = a.wake_s;
  g_lcd_maintenance_remaining_s = a.remaining_s;
  g_lcd_maintenance_start_epoch = a.start_epoch;
  g_lcd_maintenance_duration_sec = a.duration_s;
  g_lcd_maintenance_grace_before_sec = a.grace_before_s;
  g_lcd_maintenance_grace_after_sec = a.grace_after_s;
  strlcpy(g_lcd_maintenance_request_id, a.request_id, sizeof(g_lcd_maintenance_request_id));
}
static bool lcd_arm_read(Preferences& p, LcdMaintenanceArm& a) {
  return p.getBytesLength(LCD_MAINT_ARM_KEY) == sizeof(a) &&
      p.getBytes(LCD_MAINT_ARM_KEY, &a, sizeof(a)) == sizeof(a) && lcd_arm_valid(a);
}
static bool lcd_arm_commit(const LcdMaintenanceArm& a,const LcdNvsDeadline& deadline=LcdNvsDeadline()) {
  if (!lcd_arm_valid(a)) return false;
  LcdMaintenanceStorageGuard guard;
  LcdNvsLease capacity_lease;
  size_t available=0;
  const size_t peak=lcd_nvs_blob_entries(sizeof(a))+1;
  // arm_v1 is unchanged and remains readable by the previous firmware. It may
  // be cleared after boot selection; only any new-codec/diagnostic reclamation
  // path additionally needs the active VALID barrier.
  if (!deadline.live() || !capacity_lease || g_lcd_nvs_uncertain || !lcd_nvs_available(available) ||
      (available<peak && !lcd_nvs_prepare_essential(peak,deadline))) {
    g_lcd_arm_storage_fault=true;return false;
  }
  Preferences p;
  if (!deadline.live() || !p.begin(LCD_MAINT_PREF_NAMESPACE, false)) {
    g_lcd_arm_storage_fault = true; return false;
  }
  LcdMaintenanceArm prior = {};
  const bool prior_valid = lcd_arm_read(p, prior);
  if(!deadline.live()){p.end();g_lcd_arm_storage_fault=true;return false;}
  const size_t written = p.putBytes(LCD_MAINT_ARM_KEY, &a, sizeof(a));
  LcdMaintenanceArm readback = {};
  const bool readable = lcd_arm_read(p, readback);
  const bool committed = written == sizeof(a) && readable &&
      memcmp(&a, &readback, sizeof(a)) == 0;
  p.end();
  // Preferences returns zero when nvs_commit fails. Same-handle readback alone
  // cannot prove durability after that error. Keep the prior verified value.
  // A late SUCCESSFUL call is still a commit; callers must not invent rollback.
  if (committed) {
    lcd_arm_publish(readback);
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS && HALO_LCD_SLEEP_WITNESS
    lcd_sleep_witness_arm_committed(readback);
#endif
  }
  else if (prior_valid) lcd_arm_publish(prior);
  else { LcdMaintenanceArm empty = {}; lcd_arm_seal(empty); lcd_arm_publish(empty); }
  g_lcd_arm_storage_fault = !committed;
  return committed;
}
static bool lcd_clear_persisted_maintenance_state(const char* reason,const LcdNvsDeadline& deadline=LcdNvsDeadline()) {
  LcdMaintenanceArm disarm = {}; lcd_arm_seal(disarm);
  const bool ok = lcd_arm_commit(disarm,deadline);
  Serial.printf("[LCD_MAINT_NVS] disarm verified=%d reason=%s\n", ok ? 1 : 0, reason ? reason : "unknown");
  return ok;
}
static bool lcd_persist_maintenance_state(const char* reason) {
  LcdMaintenanceArm a = lcd_arm_from_ram();
  const bool ok = lcd_arm_commit(a);
  Serial.printf("[LCD_MAINT_NVS] save verified=%d reason=%s\n", ok ? 1 : 0, reason ? reason : "unknown");
  return ok;
}
static bool lcd_restore_persisted_maintenance_state(const char* reason) {
  LcdMaintenanceStorageGuard guard;
  Preferences p;
  // Never trust retained RTC data when authoritative storage cannot be read.
  LcdMaintenanceArm empty = {}; lcd_arm_seal(empty); lcd_arm_publish(empty);
  g_lcd_arm_storage_fault = true;
  if (!p.begin(LCD_MAINT_PREF_NAMESPACE, true)) return false;
  LcdMaintenanceArm a = {};
  const bool has_record = p.isKey(LCD_MAINT_ARM_KEY);
  if (has_record) {
    const bool valid = lcd_arm_read(p, a); p.end();
    if (!valid) {
      // Corrupt authoritative state suppresses both RTC and legacy fallback.
      LcdMaintenanceArm empty = {}; lcd_arm_seal(empty); lcd_arm_publish(empty);
      Serial.println("[LCD_MAINT_NVS] corrupt arm record; legacy fallback refused");
      return false;
    }
    lcd_arm_publish(a);
  } else {
    // One-time migration of the complete old representation. Missing or
    // inconsistent legacy values never become a verified arm.
    const bool legacy_present = p.isKey(LCD_MAINT_PREF_KEY_VALID);
    if (legacy_present && p.getType(LCD_MAINT_PREF_KEY_VALID) != PT_U8) { p.end(); return false; }
    if (legacy_present && p.getUChar(LCD_MAINT_PREF_KEY_VALID, 2) > 1) { p.end(); return false; }
    const bool legacy_valid = p.getBool(LCD_MAINT_PREF_KEY_VALID, false);
    if (legacy_valid && p.getType(LCD_MAINT_PREF_KEY_ARMED) != PT_U8) { p.end(); return false; }
    if (legacy_valid && p.getUChar(LCD_MAINT_PREF_KEY_ARMED, 2) > 1) { p.end(); return false; }
    if (legacy_valid && p.getBool(LCD_MAINT_PREF_KEY_ARMED, false)) {
      const char* numeric_keys[] = {LCD_MAINT_PREF_KEY_WAKE_S, LCD_MAINT_PREF_KEY_REMAIN_S,
          LCD_MAINT_PREF_KEY_START, LCD_MAINT_PREF_KEY_DUR, LCD_MAINT_PREF_KEY_GB, LCD_MAINT_PREF_KEY_GA};
      for (const char* key : numeric_keys) {
        if (p.getType(key) != PT_U32) { p.end(); return false; }
      }
      if (p.getType(LCD_MAINT_PREF_KEY_REQ_ID) != PT_STR) { p.end(); return false; }
      a.armed = 1;
      a.wake_s = p.getUInt(LCD_MAINT_PREF_KEY_WAKE_S, 0);
      a.remaining_s = p.getUInt(LCD_MAINT_PREF_KEY_REMAIN_S, 0);
      a.start_epoch = p.getUInt(LCD_MAINT_PREF_KEY_START, 0);
      a.duration_s = p.getUInt(LCD_MAINT_PREF_KEY_DUR, 0);
      a.grace_before_s = p.getUInt(LCD_MAINT_PREF_KEY_GB, 0);
      a.grace_after_s = p.getUInt(LCD_MAINT_PREF_KEY_GA, 0);
      String id = p.getString(LCD_MAINT_PREF_KEY_REQ_ID, "");
      if (id.length() >= sizeof(a.request_id)) { p.end(); return false; }
      strlcpy(a.request_id, id.c_str(), sizeof(a.request_id));
    }
    p.end(); lcd_arm_seal(a);
    if (!lcd_arm_valid(a) || !lcd_arm_commit(a)) return false;
  }
  g_lcd_arm_storage_fault = false;
  Serial.printf("[LCD_MAINT_NVS] restore verified=1 reason=%s armed=%d start_epoch=%lu request_id=%s\n",
      reason ? reason : "unknown", a.armed ? 1 : 0, (unsigned long)a.start_epoch, a.request_id);
  char detail[80]; snprintf(detail, sizeof(detail), "clk=%d rid=%s", lcd_time_valid() ? 1 : 0, a.request_id);
  lcd_errlog_store_with_context("lcd", "maint", "RESTORE", (int)a.armed, detail);
  return a.armed != 0;
}
#endif
