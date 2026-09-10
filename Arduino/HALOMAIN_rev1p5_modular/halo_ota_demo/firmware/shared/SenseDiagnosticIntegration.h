#pragma once
// Included by the production wrapper after its existing owners are declared.
// Observational only: none of these results admit, cancel or retry an OTA.
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
#include "DiagnosticNvs.h"
#include "DiagnosticHandoff.h"
#include "ManifestOutcome.h"
#include <esp_app_desc.h>
#ifndef HALO_DIAG_RETIRE_SNAPSHOT_20260908
#define HALO_DIAG_RETIRE_SNAPSHOT_20260908 0
#endif

static uint32_t g_diag_started_ms=0,g_diag_budget_ms=0;
static bool g_diag_uncertain=false,g_diag_ready=false,g_diag_attempt_open=false;
static bool g_diag_boot_seen=false,g_diag_capture_enabled=false,g_diag_seal_requested=false;
static uint8_t g_diag_attempt=0,g_diag_attempt_id[16]{};
static uint32_t g_diag_attempt_context_crc=0,g_diag_attempt_expected=0;
// One pending observation, not a second journal or a retry authority. It is
// volatile until the existing bounded diagnostic owner can commit it.
struct SenseManifestPending {
  ManifestOutcome outcome{};
  uint8_t attempt_id[16]{};
  uint64_t boot=0,epoch=0;
  uint32_t uptime=0,context_crc=0,expected=0,free=0,largest=0,minimum=0;
  uint32_t running=0,selected=0,image_state=0;
  uint8_t slot=0,quality=0;
  bool pending=false;
};
static_assert(sizeof(SenseManifestPending)<=104,"bounded deferred manifest sample");
static SenseManifestPending g_diag_manifest_pending;
static void sense_diag_flush_manifest(uint32_t remaining);

static uint64_t g_diag_timer_us=0,g_diag_timer_selected_epoch=0;
static int32_t g_diag_timer_sdk=0;
static bool g_diag_timer_observed=false;
static uint8_t g_diag_scratch[halo_diag::MAX_RECORD];

static bool sense_diag_safe(void*) {
  return g_diag_budget_ms && (uint32_t)(millis()-g_diag_started_ms)<g_diag_budget_ms &&
      !g_diag_uncertain && !g_nvs_reclaim_uncertain && !g_ota_apply_in_progress &&
      !g_lcd_ota_task_running && !g_lcd_ota_proxy_owns_uart &&
      !halo_primary_user_work_busy() &&
      !upload_inflight && !http_inflight && !voice_recording_active &&
      !scan_ui_inflight && !dish_scan_inflight && !foreground_active &&
      !current_job.active && !upload_queue_count() &&
      !(op_queue && uxQueueMessagesWaiting(op_queue)) &&
      !halo_provisioning_active() && !xPortInIsrContext();
}
static halo_diag::NvsGuard g_diag_guard{
  nullptr,
  [](void*){return !g_optional_nvs_writer.test_and_set(std::memory_order_acquire);},
  [](void*){g_optional_nvs_writer.clear(std::memory_order_release);},
  sense_diag_safe,[](void*){return true;},
  [](void*){return nvs_capacity_image_valid();},
  [](void*){
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
    return halo_policy_allocated_held(); // typed+CRC proof under this SAME NVS lease
#else
    return false;
#endif
  },
  [](void*,bool retirement){g_diag_uncertain=true;if(retirement)g_nvs_reclaim_uncertain.store(true);}
};
static halo_diag::NvsAdapter g_diag_adapter(halo_diag::Board::Sense,g_diag_guard,true);
static halo_diag::Store g_diag_store=g_diag_adapter.store();
static halo_diag::Journal g_diag_journal(g_diag_store,g_diag_scratch);

struct SenseDiagnosticScope {
  bool active=false;
  explicit SenseDiagnosticScope(uint32_t budget=1500) {
    if(g_diag_budget_ms || !budget)return;
    g_diag_started_ms=millis();g_diag_budget_ms=budget<1500?budget:1500;
    active=true;
  }
  ~SenseDiagnosticScope(){if(active)g_diag_budget_ms=0;}
};
static void sense_diag_result(const char* event,halo_diag::Result r) {
  Serial.printf("[OTA_DIAG] event=%s result=%u active=%u uncertain=%u\n",
      event,(unsigned)r,g_diag_ready?1:0,g_diag_uncertain?1:0);
}
static bool sense_diag_ensure_open() {
  if(g_diag_ready)return true;
  if(!sense_diag_safe(nullptr) || !g_coord_sense_boot_id)return false;
  if(!(HALO_DIAG_RETIRE_SNAPSHOT_20260908==1
      ?g_diag_adapter.qualify_and_retire(true,g_diag_scratch,sizeof(g_diag_scratch),true)
      :g_diag_adapter.qualify_without_retirement())) {
    static uint8_t reported=255;const uint8_t why=uint8_t(g_diag_adapter.last_qualification());
    if(reported!=why){reported=why;Serial.printf("[OTA_DIAG] qualification=%u active=0 uncertain=%u\n",why,g_diag_uncertain?1:0);}
    return false;
  }
  const auto r=g_diag_journal.open();
  g_diag_ready=r==halo_diag::Result::Ok;g_diag_capture_enabled=false;
  if(g_diag_ready&&g_diag_journal.sealed())g_diag_seal_requested=true;
  sense_diag_result("open",r);
  return g_diag_ready || r==halo_diag::Result::Empty || r==halo_diag::Result::Incomplete;
}
static bool sense_diag_hex(const char* text,uint8_t (&out)[32]) {
  if(!text || strnlen(text,65)!=64)return false;
  auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
  for(unsigned i=0;i<32;++i){int a=digit(text[2*i]),b=digit(text[2*i+1]);if(a<0||b<0)return false;out[i]=uint8_t((a<<4)|b);}
  return halo_diag::nonzero(out,32);
}
static bool sense_diag_context_decode(const uint8_t* raw,halo_diag::Context& out) {
  if(!halo_diag::valid_context(raw))return false;
  const uint8_t*p=raw+halo_diag::HEADER;out={};
  memcpy(out.journal,p,16);memcpy(out.campaign,p+16,16);memcpy(out.target_sha,p+32,32);
  memcpy(out.target_version,p+64,32);memcpy(out.origin,p+96,64);memcpy(out.device_mac,p+160,6);
  out.board=(halo_diag::Board)p[166];out.target_hash_kind=(halo_diag::HashKind)p[167];
  out.expected_bytes=halo_diag::get32(p+168);out.created_epoch=halo_diag::get64(p+172);
  memcpy(out.owner_binding_sha,p+180,32);return true;
}
static void sense_diag_sha(const void* a,size_t an,const void* b,size_t bn,uint8_t (&out)[32]) {
  mbedtls_sha256_context ctx;mbedtls_sha256_init(&ctx);mbedtls_sha256_starts(&ctx,0);
  mbedtls_sha256_update(&ctx,(const unsigned char*)a,an);
  mbedtls_sha256_update(&ctx,(const unsigned char*)b,bn);
  mbedtls_sha256_finish(&ctx,out);mbedtls_sha256_free(&ctx);
}
static halo_diag::Result sense_diag_state(halo_diag::Stage stage,bool orphan=false) {
  if((!orphan&&(!g_diag_capture_enabled||!g_diag_journal.capture_enabled()||!g_diag_ready))||!sense_diag_safe(nullptr)||!g_coord_sense_boot_id)return halo_diag::Result::Busy;
  halo_diag::State state{};
  state.boot_id=g_coord_sense_boot_id;state.epoch=sense_now_epoch();state.uptime_ms=millis();
  if(!halo_diag::text_ok(kBuildId,sizeof(state.build)))return halo_diag::Result::Busy;
  memcpy(state.build,kBuildId,strlen(kBuildId)+1);
  state.stage=stage;state.attempt=g_diag_attempt;
  state.timer_us=g_diag_timer_us;state.timer_sdk=g_diag_timer_sdk;
  state.timer_flags=g_diag_timer_observed?1:0;
  state.selected_epoch=g_diag_timer_selected_epoch;
  if(!halo_diag::text_ok(kFirmwareVersion,sizeof(state.fw)))return halo_diag::Result::Busy;
  memcpy(state.fw,kFirmwareVersion,strlen(kFirmwareVersion)+1);
  const esp_app_desc_t* desc=esp_app_get_description();
  if(desc){memcpy(state.image_hash,desc->app_elf_sha256,32);state.hash_kind=halo_diag::HashKind::CompiledElf;}
  const esp_partition_t* running=esp_ota_get_running_partition();
  const esp_partition_t* boot=esp_ota_get_boot_partition();
  esp_ota_img_states_t image_state=ESP_OTA_IMG_UNDEFINED;
  state.image_state_sdk=running?esp_ota_get_state_partition(running,&image_state):ESP_ERR_NOT_FOUND;
  state.image_state=(uint8_t)image_state;
  state.running_offset=running?running->address:0;state.partition_size=running?running->size:0;
  state.boot_offset=boot?boot->address:0;state.wake=(uint8_t)esp_sleep_get_wakeup_cause();state.reset=(uint8_t)esp_reset_reason();
  state.time_quality=sense_time_has_fresh_sync()?halo_diag::TimeQuality::Fresh:halo_diag::TimeQuality::Unknown;
  state.internal_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  state.internal_largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  state.internal_min=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  if(orphan){state.flags=halo_diag::STATE_ORPHAN_CLOSURE;const auto r=g_diag_journal.close_orphan(state);if(r==halo_diag::Result::Ok)g_diag_ready=true;sense_diag_result("orphan_closure",r);return r;}
  const auto result=g_diag_journal.state(state);sense_diag_result("state",result);return result;
}
static void sense_diag_open_boot() {
  // setup may run while PENDING_VERIFY. A denied open is retried from the
  // existing post-health loop; only a successful open consumes this boot hook.
  if(g_diag_boot_seen || !nvs_capacity_image_valid())return;
  SenseDiagnosticScope scope;if(!scope.active)return;
  if(sense_diag_ensure_open()) {
    g_diag_boot_seen=true;
    if(g_diag_ready)sense_diag_state(halo_diag::Stage::Boot);
  }
}
#if defined(HALO_DIAGNOSTIC_ADMISSION) && HALO_DIAGNOSTIC_ADMISSION
static void halo_diag_admission_state_blocked(const halo_diag::Context&);
static bool halo_diag_admission_retention_pending();
#endif
// Both preflight and later apply use precisely this admitted identity.
static void sense_diag_begin_identity(const char*version,const uint8_t*target_hash,
    uint32_t expected,const char*origin,const uint8_t*policy_campaign,
    uint32_t policy_ordinal,uint32_t remaining_ms) {
  const uint32_t operation_start=millis();
  sense_diag_flush_manifest(remaining_ms);
  const uint32_t spent=uint32_t(millis()-operation_start);
  g_diag_attempt_open=false;g_diag_capture_enabled=false;
  if(g_diag_manifest_pending.pending){sense_diag_result("manifest_pending",halo_diag::Result::Busy);return;}
  if(spent>=remaining_ms)return;
  SenseDiagnosticScope scope(remaining_ms-spent);if(!scope.active||!sense_diag_ensure_open())return;
  halo_diag::Context ctx{};
  if(!target_hash||!halo_diag::nonzero(target_hash,32)||!expected||
      !halo_diag::text_ok(version,sizeof(ctx.target_version))||
      !halo_diag::text_ok(origin,sizeof(ctx.origin)))return;
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
  if(!policy_campaign||!halo_diag::nonzero(policy_campaign,16))return;
#endif
  uint8_t raw[halo_diag::MAX_RECORD];size_t len=0;
  const auto found=g_diag_journal.read_retained(0,raw,sizeof(raw),len);
  if(found==halo_diag::Result::Ok) {
    if(!sense_diag_context_decode(raw,ctx) || ctx.board!=halo_diag::Board::Sense ||
        memcmp(ctx.target_sha,target_hash,32) || ctx.expected_bytes!=expected ||
        strcmp(ctx.target_version,version) || strcmp(ctx.origin,origin)
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
        || memcmp(ctx.campaign,policy_campaign,16)
#endif
        ) {
      g_diag_seal_requested=true;sense_diag_state(halo_diag::Stage::JournalBefore,true);sense_diag_result("context_conflict",halo_diag::Result::Busy);return;
    }
  } else if(found==halo_diag::Result::Empty) {
    esp_fill_random(ctx.journal,sizeof(ctx.journal));
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
    memcpy(ctx.campaign,policy_campaign,16);
#else
    uint8_t campaign[32];sense_diag_sha(origin,strlen(origin),target_hash,32,campaign);memcpy(ctx.campaign,campaign,16);
#endif
    memcpy(ctx.target_sha,target_hash,32);memcpy(ctx.target_version,version,strlen(version)+1);
    if(!halo_diag::text_ok(origin,sizeof(ctx.origin)))return;
    memcpy(ctx.origin,origin,strlen(origin)+1);
    if(esp_read_mac(ctx.device_mac,ESP_MAC_ETH)!=ESP_OK)return;
    ctx.board=halo_diag::Board::Sense;ctx.target_hash_kind=halo_diag::HashKind::ExpectedBin;
    ctx.expected_bytes=expected;ctx.created_epoch=sense_now_epoch();
    char owner[64]{};if(ProvisioningState::loadOwnerId(owner,sizeof(owner))&&owner[0])sense_diag_sha(owner,strlen(owner),ctx.device_mac,6,ctx.owner_binding_sha);
  } else {sense_diag_result("context_read",found);return;}
  if(!g_diag_ready){const auto made=g_diag_journal.initialize(ctx);g_diag_ready=made==halo_diag::Result::Ok;sense_diag_result("initialize",made);if(!g_diag_ready)return;}
  if(!g_diag_journal.capture_enabled()){g_diag_seal_requested=true;sense_diag_result("capture_closed",halo_diag::Result::Busy);return;}
  // Read exact initialized context CRC for a later RAM-only observation.
  if(g_diag_journal.read_retained(0,raw,sizeof(raw),len)!=halo_diag::Result::Ok)return;
  g_diag_attempt_context_crc=halo_diag::record_crc(raw,0);g_diag_attempt_expected=expected;
  g_diag_capture_enabled=true;g_diag_seal_requested=false;
  for(uint8_t attempt=0;attempt<2;++attempt){
    auto present=g_diag_journal.read_retained(uint8_t(1+attempt),raw,sizeof(raw),len);
    if(present==halo_diag::Result::Empty){g_diag_attempt=attempt;
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
      memcpy(g_diag_attempt_id,policy_campaign,12);halo_diag::put32(g_diag_attempt_id+12,policy_ordinal);
#else
      esp_fill_random(g_diag_attempt_id,16);
#endif
      g_diag_attempt_open=true;break;}
    if(present!=halo_diag::Result::Ok){sense_diag_result("failure_slot",present);return;}
  }
  const auto admission_state=sense_diag_state(halo_diag::Stage::Admission);
#if defined(HALO_DIAGNOSTIC_ADMISSION) && HALO_DIAGNOSTIC_ADMISSION
  if(admission_state==halo_diag::Result::Busy)halo_diag_admission_state_blocked(ctx);
#endif
}
static void sense_diag_suspend_capture(){g_diag_capture_enabled=false;g_diag_attempt_open=false;}
static void sense_diag_begin(const OtaManifest& manifest,uint32_t remaining_ms) {
  sense_diag_suspend_capture();
  char origin[64]{};uint8_t campaign[16]{},sha[32];uint32_t ordinal=0;
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
  if(!halo_policy_diagnostic_identity(manifest,campaign,origin,ordinal)){
    g_diag_capture_enabled=false;g_diag_attempt_open=false;return;
  }
#else
  strlcpy(origin,g_coord_pending,sizeof(origin));
#endif
  if(!sense_diag_hex(manifest.sha256,sha))return;
  sense_diag_begin_identity(manifest.version,sha,manifest.size,origin,campaign,ordinal,remaining_ms);
}
// Read/export readiness never implies a current operation's capture identity.
static bool sense_diag_capture_current(){return g_diag_capture_enabled&&g_diag_journal.capture_enabled();}
// Conservative RAM notification: closes any retained capture; it never claims
// that a different retained campaign resolved. The later handoff contains the
// OLD exact context. There is no NVS write or renewed budget in this callback.
static void sense_diag_close_campaign(const uint8_t*,const char*){
  g_diag_capture_enabled=false;g_diag_attempt_open=false;g_diag_seal_requested=true;
}
static void sense_diag_service_seal(uint32_t remaining){
  const uint32_t operation_start=millis();sense_diag_flush_manifest(remaining);
  const uint32_t spent=uint32_t(millis()-operation_start);
  if(spent>=remaining||g_diag_manifest_pending.pending||!g_diag_seal_requested)return;SenseDiagnosticScope scope(remaining-spent);if(!scope.active||!sense_diag_ensure_open())return;
  sense_diag_state(halo_diag::Stage::JournalBefore,true);if(!g_diag_ready)return;
  const auto r=g_diag_journal.seal();if(r==halo_diag::Result::Ok||r==halo_diag::Result::Already)g_diag_seal_requested=false;sense_diag_result("seal",r);
}
static halo_diag::Result sense_diag_retire(uint32_t remaining){
  if(g_diag_manifest_pending.pending)return halo_diag::Result::Busy;
  SenseDiagnosticScope scope(remaining);if(!scope.active||!sense_diag_ensure_open()||!g_diag_ready)return halo_diag::Result::Busy;
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_OTA_POLICY
  if(halo_diag_admission_retention_pending())return halo_diag::Result::Busy;
#endif
  const auto r=g_diag_journal.retire_if_ready();if(r==halo_diag::Result::Empty){g_diag_ready=false;g_diag_capture_enabled=false;g_diag_attempt_open=false;g_diag_seal_requested=false;memset(g_diag_attempt_id,0,sizeof(g_diag_attempt_id));}
  return r;
}
// Optional actual boot evidence only after the policy owner proves this exact
// old context belongs to its current resolved pair. No context creation or
// historical failure reconstruction; a previous unACKed timer may returnBusy.
static void sense_diag_rebind_campaign(const uint8_t campaign[16],const char*origin,const char*version,const uint8_t sha[32],uint32_t size,uint32_t remaining){
  g_diag_capture_enabled=false;if(!campaign||!origin||!version||!sha||!size)return;
  SenseDiagnosticScope scope(remaining);if(!scope.active||!sense_diag_ensure_open()||!g_diag_ready||!g_diag_journal.capture_enabled())return;
  uint8_t raw[256];size_t n=0;if(g_diag_journal.read_retained(0,raw,sizeof(raw),n)!=halo_diag::Result::Ok)return;halo_diag::Context ctx;
  if(!sense_diag_context_decode(raw,ctx)||memcmp(ctx.campaign,campaign,16)||strcmp(ctx.origin,origin)||strcmp(ctx.target_version,version)||memcmp(ctx.target_sha,sha,32)||ctx.expected_bytes!=size)return;
  g_diag_capture_enabled=true;sense_diag_state(halo_diag::Stage::Boot);
}
static void sense_diag_note_stage(halo_diag::Stage stage,uint32_t remaining_ms) {
  SenseDiagnosticScope scope(remaining_ms);if(scope.active)sense_diag_state(stage);
}
static void sense_diag_failure(SenseOtaApplier::Result result) {
  SenseDiagnosticScope scope;if(!scope.active||!g_diag_ready||!g_diag_capture_enabled||!g_diag_journal.capture_enabled()||!g_diag_attempt_open||!sense_diag_safe(nullptr))return;
  auto snapshot=g_ota_applier.getFailureSnapshot();halo_diag::Failure failure{};
  memcpy(failure.attempt_id,g_diag_attempt_id,16);failure.attempt_ordinal=g_diag_attempt;
  failure.boot_id=g_coord_sense_boot_id;failure.epoch=sense_now_epoch();failure.uptime_ms=millis();failure.stage=halo_diag::Stage::Failure;
  // flags: bit0=Sense applier detail; bits8..15=exact FailureStage;
  // bits16..23=unchanged coarse Result, bits24..25=record capture time quality.
  // Unknown HTTP/cleanup SDK stay zero.
  failure.flags=(snapshot.stage!=SenseOtaApplier::FailureStage::NONE?1u:0u)|(uint32_t(snapshot.stage)<<8)|(uint32_t(result)<<16)|(uint32_t(sense_time_has_fresh_sync()?halo_diag::TimeQuality::Fresh:halo_diag::TimeQuality::Unknown)<<24);
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
  // Bit26: attempt_id is campaign-prefix12 + little-endian policy ordinal32.
  // failure.attempt_ordinal remains the pinned diagnostic slot index0/1.
  failure.flags|=1u<<26;
#endif
  failure.sdk=snapshot.sdk_error;failure.accepted=snapshot.offset;
  uint8_t context[halo_diag::MAX_RECORD];size_t len=0;
  if(g_diag_journal.read_retained(0,context,sizeof(context),len)!=halo_diag::Result::Ok)return;
  failure.expected=halo_diag::get32(context+halo_diag::HEADER+168);
  if(snapshot.stage!=SenseOtaApplier::FailureStage::NONE){
    failure.internal_free=snapshot.free_heap;failure.internal_largest=snapshot.largest_internal;failure.internal_min=snapshot.minimum_internal;
  }
  if(!halo_diag::text_ok(kBuildId,sizeof(failure.build)))return;memcpy(failure.build,kBuildId,strlen(kBuildId)+1);
  if(!halo_diag::text_ok(kFirmwareVersion,sizeof(failure.fw)))return;memcpy(failure.fw,kFirmwareVersion,strlen(kFirmwareVersion)+1);
  const esp_app_desc_t* desc=esp_app_get_description();if(desc){memcpy(failure.image_hash,desc->app_elf_sha256,32);failure.hash_kind=halo_diag::HashKind::CompiledElf;}
  const esp_partition_t* running=esp_ota_get_running_partition();const esp_partition_t* boot=esp_ota_get_boot_partition();
  failure.running_offset=running?running->address:0;failure.boot_offset=boot?boot->address:0;
  esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;failure.image_state=(uint32_t)state;if(running&&esp_ota_get_state_partition(running,&state)==ESP_OK)failure.image_state=state;
  sense_diag_result("failure",g_diag_journal.failure(failure));g_diag_attempt_open=false;
}
// Capture before policy finish/release can change clocks or partition state.
// No NVS/allocations here. The first pending observation is never overwritten.
static void sense_diag_manifest_failure(const ManifestOutcome& outcome,uint32_t remaining) {
  if(outcome.stage==ManifestStage::None||g_diag_manifest_pending.pending||
     !g_diag_ready||!g_diag_capture_enabled||!g_diag_journal.capture_enabled()||
     !g_diag_attempt_open||!g_coord_sense_boot_id)return;
  auto& p=g_diag_manifest_pending;p=SenseManifestPending{};p.outcome=outcome;
  memcpy(p.attempt_id,g_diag_attempt_id,16);p.slot=g_diag_attempt;p.boot=g_coord_sense_boot_id;
  p.epoch=sense_now_epoch();p.uptime=millis();p.context_crc=g_diag_attempt_context_crc;p.expected=g_diag_attempt_expected;
  p.quality=uint8_t(sense_time_has_fresh_sync()?halo_diag::TimeQuality::Fresh:halo_diag::TimeQuality::Unknown);
  // Return-boundary heap samples, explicitly not the earlier HTTP allocation peak.
  p.free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  p.largest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  p.minimum=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  const esp_partition_t* running=esp_ota_get_running_partition();const esp_partition_t* selected=esp_ota_get_boot_partition();
  p.running=running?running->address:0;p.selected=selected?selected->address:0;
  esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;p.image_state=uint32_t(state);
  if(running&&esp_ota_get_state_partition(running,&state)==ESP_OK)p.image_state=uint32_t(state);
  p.pending=true;g_diag_attempt_open=false;
  sense_diag_flush_manifest(remaining);
}
static void sense_diag_flush_manifest(uint32_t remaining) {
  auto& p=g_diag_manifest_pending;if(!p.pending||!remaining)return;
  SenseDiagnosticScope scope(remaining);if(!scope.active||!sense_diag_safe(nullptr)||
      p.boot!=g_coord_sense_boot_id||!g_diag_ready||!g_diag_journal.capture_enabled())return;
  uint8_t raw[halo_diag::MAX_RECORD];size_t n=0;
  if(g_diag_journal.read_retained(0,raw,sizeof(raw),n)!=halo_diag::Result::Ok||
      halo_diag::record_crc(raw,0)!=p.context_crc)return;
  halo_diag::Failure f{};memcpy(f.attempt_id,p.attempt_id,16);f.attempt_ordinal=p.slot;
  f.boot_id=p.boot;f.epoch=p.epoch;f.uptime_ms=p.uptime;f.stage=halo_diag::Stage::Http;
  f.http=p.outcome.http_status;f.sdk=p.outcome.transport;f.cleanup_sdk=p.outcome.tls_error;
  // bit2 = manifest observation: sdk means HTTPClient transport result;
  // cleanup_sdk means secure-client lastError (NOT a cleanup result).
  // bit3 original caller deadline elapsed after fetch; bits8..15 stage,
  // bits16..19 attempts, bits20..23 parser code.
  f.flags=4u|(p.outcome.deadline_after?8u:0u)|(uint32_t(p.outcome.stage)<<8)|(uint32_t(p.outcome.attempts)<<16)|
      (uint32_t(p.outcome.parser_code)<<20)|(uint32_t(p.quality)<<24);
#if defined(HALO_DURABLE_OTA_POLICY) && HALO_DURABLE_OTA_POLICY
  f.flags|=1u<<26; // original policy ordinal; preflight may legitimately be 0
#endif
  f.expected=p.expected;f.internal_free=p.free;f.internal_largest=p.largest;f.internal_min=p.minimum;
  f.running_offset=p.running;f.boot_offset=p.selected;f.image_state=p.image_state;
  if(!halo_diag::text_ok(kBuildId,sizeof(f.build))||!halo_diag::text_ok(kFirmwareVersion,sizeof(f.fw)))return;
  memcpy(f.build,kBuildId,strlen(kBuildId)+1);memcpy(f.fw,kFirmwareVersion,strlen(kFirmwareVersion)+1);
  // These compile-time source values cannot change within the saved same boot.
  const esp_app_desc_t* desc=esp_app_get_description();if(desc){memcpy(f.image_hash,desc->app_elf_sha256,32);f.hash_kind=halo_diag::HashKind::CompiledElf;}
  const auto result=g_diag_journal.failure(f);sense_diag_result("manifest_failure",result);
  if(result==halo_diag::Result::Ok||result==halo_diag::Result::Already)p.pending=false;
}
static void sense_diag_note_timer_sdk(uint64_t timer_us,int32_t sdk_result) {
  g_diag_timer_us=timer_us;g_diag_timer_sdk=sdk_result;g_diag_timer_observed=true;
  g_diag_timer_selected_epoch=sense_time_has_fresh_sync()?uint64_t(sense_now_epoch())+timer_us/1000000ULL:0;
}
static void sense_diag_presleep() {
  const uint32_t operation_start=millis();sense_diag_flush_manifest(1500);
  const uint32_t spent=uint32_t(millis()-operation_start);if(spent>=1500)return;
  SenseDiagnosticScope scope(1500-spent);if(scope.active)sense_diag_state(halo_diag::Stage::PreSleep);
}
#endif
