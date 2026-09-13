#pragma once
#include "SenseDurablePolicyState.h"

// Synchronous main-task coordinator adapter. The canonical Record and scratch
// are owned by State.h; this object holds only the live invocation and borrowed
// manifest. It never survives a call as an active operation after reboot.
namespace sense_policy {
#if HALO_DURABLE_OTA_POLICY
// Invocation-local proof, captured before any network/flash work. It is never
// an authority across reset and consumes no persistent key or policy bytes.
struct RetryBaseline {
  char fw[32]{},part[16]{};
  uint32_t boot=0,part_size=0;
};
struct Work {
  bool live=false,finished=false,legacy=false,normal=false;
  uint32_t original_start=0,original_budget=0,phase_start=0,phase_budget=0;
  const OtaManifest* manifest=nullptr;
  RetryBaseline retry_baseline;
  durable_ota::Failure failure=durable_ota::Failure::TEMPORARY;
  uint32_t stage=0;int32_t error=0;
};
static Work work;
static bool boot_reconciled=false;
// A completed policy transaction only closes diagnostic capture in RAM.
// Its journal drains/seals later under the existing optional export deadline;
// no diagnostic result or storage tail changes the committed policy outcome.
static void diagnostic_terminal(const durable_ota::Record& r) {
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  if(r.phase==durable_ota::Phase::RESOLVED)
    sense_diag_close_campaign(r.campaign,r.origin);
#else
  (void)r;
#endif
}
static void diagnostic_observed_pair(const durable_ota::Record& r) {
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  const uint32_t elapsed=uint32_t(millis()-work.original_start);
  if(elapsed<work.original_budget)
    sense_diag_rebind_campaign(r.campaign,r.origin,r.target.version,
      r.target.sha256,r.target.bytes,work.original_budget-elapsed);
#else
  (void)r;
#endif
}
static uint32_t remaining() {
  if(!work.live||work.finished)return 0;
  const uint32_t elapsed=uint32_t(millis()-work.original_start);
  const uint32_t phase_elapsed=uint32_t(millis()-work.phase_start);
  if(elapsed>=work.original_budget||phase_elapsed>=work.phase_budget)return 0;
  uint32_t n=work.original_budget-elapsed;
  if(work.phase_budget-phase_elapsed<n)n=work.phase_budget-phase_elapsed;
  const auto* r=current();const auto c=fresh_clock(work.normal);
  if(!r||!durable_ota::clock_valid(*r,c)||!durable_ota::active_phase(*r)||c.epoch>=r->active_deadline)return 0;
  const uint64_t epoch_left=uint64_t(r->active_deadline-c.epoch)*1000ULL;
  return epoch_left<n?uint32_t(epoch_left):n;
}
static bool commit(const durable_ota::Record& r,const void* begin_owner=nullptr) {
  return commit_candidate(r,work.original_start,work.original_budget,false,begin_owner);
}
static void clamp_pair() {
  if(!work.live)return;
  const uint32_t elapsed=uint32_t(millis()-g_lcd_work_budget.started_ms);
  const uint32_t left=remaining();
  // Keep the existing original pair start. A phase grant may expose its still
  // unused original budget, but can never extend that original deadline.
  g_lcd_work_budget.limit_ms=elapsed+left;
}
static bool hex_sha(const char* in,uint8_t(&out)[32]) {
  if(!in||strlen(in)!=64)return false;
  for(unsigned i=0;i<32;++i){unsigned value=0;for(unsigned j=0;j<2;++j){char c=in[i*2+j];unsigned n;
    if(c>='0'&&c<='9')n=c-'0';else if(c>='a'&&c<='f')n=c-'a'+10;else if(c>='A'&&c<='F')n=c-'A'+10;else return false;value=value*16+n;}out[i]=uint8_t(value);}
  return durable_ota::nonzero(out,32);
}
static bool matches_sense(const durable_ota::Target&t,const OtaManifest&m) {
  uint8_t sha[32];return !strcmp(t.version,m.version)&&!strcmp(t.url,m.url)&&t.bytes==m.size&&
      hex_sha(m.sha256,sha)&&!memcmp(t.sha256,sha,32);
}
static bool peer_valid(const char* expected=nullptr) {
  return g_lcd_query_boot_ready&&g_lcd_query_peer_boot_id&&g_lcd_ota_query_resp_fw[0]&&
    !strcmp(g_lcd_query_running_state,"VALID")&&g_lcd_query_running_part[0]&&
    !strcmp(g_lcd_query_running_part,g_lcd_query_boot_part)&&
    (!expected||compareSemver(g_lcd_ota_query_resp_fw,expected)>=0);
}
// The paired coordinator has just rechecked this exact nonce/owner/boot proof.
// Retain only the fields needed to reject a different baseline at retry time.
static void remember_retry_baseline() {
  if(!g_peer_gate.active||!g_peer_gate.entered||!g_peer_gate.ready||!g_peer_gate.locked||
     g_peer_gate.legacy||!g_peer_gate.owner[0]||
     uint32_t(millis()-g_peer_gate.proof_ms)>=2000||
     int32_t(millis()-g_peer_gate.deadline_ms)>=0||!peer_valid()||
     g_lcd_query_peer_boot_id!=g_peer_gate.peer_boot||
     strcmp(g_lcd_query_coord_id,g_peer_gate.challenge)||
     strcmp(g_lcd_query_coord_owner,g_peer_gate.owner)||!g_lcd_query_coord_lease_ms||
     g_lcd_query_coord_lease_ms>120000||!g_lcd_ota_query_resp_part_size)return;
  auto& b=work.retry_baseline;
  strlcpy(b.fw,g_lcd_ota_query_resp_fw,sizeof(b.fw));
  strlcpy(b.part,g_lcd_query_running_part,sizeof(b.part));
  b.boot=g_lcd_query_peer_boot_id;b.part_size=g_lcd_ota_query_resp_part_size;
}
static bool retry_transport_ready() {
  return sense_lcd_ota_retry_safe()&&!g_lcd_ota_proxy_owns_uart&&!g_lcd_ota_task_running;
}
static bool retry_peer_ready(const LcdOtaQuerySnapshot& p,const durable_ota::Record& r) {
  if(!retry_transport_ready())return false;
  if(!p.correlated||!p.peer_boot_id||!p.boot_ready||strcmp(p.running_state,"VALID")||
     !p.running_part[0]||!strcmp(p.running_part,"?")||strcmp(p.running_part,p.boot_part))return false;
  if(compareSemver(p.fw,r.target.peer_version)>=0)return true;
  const auto& b=work.retry_baseline;
  // A failed write may leave the same VALID image running. The caller proves
  // cleanup before querying; this fresh nonce must still match the exact
  // invocation-local boot. Plain OTA_LOCK clears the preflight lease, and a
  // long transfer can outlive it. An exactly unowned peer is safe here; a
  // different live owner or inconsistent owner/lease tuple is not. Neither
  // an empty lease nor the query alone proves cleanup. Begin charges remain spent.
  const bool original_owner=!strcmp(p.coord_owner,g_peer_gate.owner)&&
    p.coord_lease_ms&&p.coord_lease_ms<=120000;
  const bool unowned=!p.coord_owner[0]&&!p.coord_lease_ms;
  return b.boot&&p.peer_boot_id==b.boot&&
    !strcmp(p.fw,b.fw)&&!strcmp(p.running_part,b.part)&&p.part_size==b.part_size&&
    compareSemver(b.fw,r.target.peer_version)<0&&g_peer_gate.entered&&g_peer_gate.locked&&
    !g_peer_gate.legacy&&g_peer_gate.owner[0]&&(original_owner||unowned);
}
static bool image_matches(const esp_partition_t* part,const durable_ota::Target&t,uint32_t started,uint32_t budget) {
  if(!part||!t.bytes||t.bytes>part->size)return false;
  // Hash the same full BIN extent named by the immutable manifest. The app
  // descriptor's ELF digest is not interchangeable with that artifact hash.
  mbedtls_sha256_context sha;mbedtls_sha256_init(&sha);mbedtls_sha256_starts(&sha,0);
  bool ok=true;
  for(uint32_t offset=0;offset<t.bytes;){
    if(uint32_t(millis()-started)>=budget){ok=false;break;}
    const uint32_t n=(t.bytes-offset)<512?t.bytes-offset:512;
    if(esp_partition_read(part,offset,state_scratch,n)!=ESP_OK){ok=false;break;}
    mbedtls_sha256_update(&sha,state_scratch,n);offset+=n;
  }
  uint8_t digest[32];if(ok)mbedtls_sha256_finish(&sha,digest);mbedtls_sha256_free(&sha);
  return ok&&uint32_t(millis()-started)<budget&&!memcmp(digest,t.sha256,32);
}
static bool running_matches(const durable_ota::Target&t,uint32_t started,uint32_t budget) {
  return nvs_capacity_image_valid()&&!strcmp(t.version,kFirmwareVersion)&&
    image_matches(esp_ota_get_running_partition(),t,started,budget)&&nvs_capacity_image_valid();
}
static bool rollback_matches(const durable_ota::Record&r,uint32_t started,uint32_t budget) {
  if(!r.attempt_ordinal||!r.attempt_begins[0]||!nvs_capacity_image_valid())return false;
  const esp_partition_t* invalid=esp_ota_get_last_invalid_partition();
  const esp_partition_t* running=esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  return invalid&&running&&invalid->address!=running->address&&
    esp_ota_get_state_partition(invalid,&state)==ESP_OK&&(state==ESP_OTA_IMG_INVALID||state==ESP_OTA_IMG_ABORTED)&&
    // The SDK app descriptor contains the Arduino builder version, not the
    // HALO release. Join the exact immutable full BIN hash instead.
    image_matches(invalid,r.target,started,budget);
}
// Calendar identity survives consumption of the UART notice. This selects a
// bound origin only; normal_entry still requires its actual due time/day.
static uint32_t normal_calendar_due() {
  if(g_calendar_timer_origin.id[0]&&g_calendar_timer_origin.bound)
    return g_calendar_timer_origin.target_epoch;
  const CoordinatorCreditState base=coord_credit_base();
  auto observed=[&](const NightlyCreditOrigin& origin){
    if(!origin.bound||!origin.id[0]||!origin.target_epoch)return false;
    if(g_lcd_timer_notice.pending&&!strcmp(g_lcd_timer_notice.schedule,origin.id))return true;
    return g_boot_ota_pending&&g_lcd_timer_origin.boot_id&&
      g_lcd_timer_origin.boot_id==g_lcd_timer_seen_boot&&
      g_lcd_timer_origin.wake==int(ESP_SLEEP_WAKEUP_TIMER)&&
      !strcmp(g_lcd_timer_origin.schedule,origin.id)&&g_peer_gate.active&&
      g_peer_gate.ready&&!g_peer_gate.legacy&&g_peer_gate.peer_boot==g_lcd_timer_origin.boot_id;
  };
  if(observed(base.schedule))return base.schedule.target_epoch;
  if(observed(base.deferred))return base.deferred.target_epoch;
  return 0;
}
static bool normal_entry() {
  const auto c=fresh_clock();if(!c.fresh||!c.epoch)return false;
  const auto* bench=current();if(bench&&durable_ota::bench_active(*bench))return bench_deferred_due(*bench,c);
  // An early peer proof permits waiting, never a new day's allowance.
  const uint32_t due=normal_calendar_due();
  return due&&due/86400UL==c.epoch/86400UL&&c.epoch>=due;
}
static bool enter(const char* reason,bool retained_legacy) {
  if(work.live||!g_lcd_work_budget_live||!g_lcd_work_budget.remaining_ms()||!nvs_capacity_image_valid())return false;
  work={};work.original_start=g_lcd_work_budget.started_ms;work.original_budget=g_lcd_work_budget.limit_ms;
  work.normal=normal_entry();work.legacy=retained_legacy;
  remember_retry_baseline();
  if(!load_state(work.original_start,work.original_budget))return false;
  work.normal=normal_entry();
  bool old_expectation=false;
  if(!legacy_expectation(work.original_start,work.original_budget,old_expectation))return false;
  retained_legacy|=old_expectation;work.legacy=retained_legacy;
  const auto c=fresh_clock(work.normal);const uint32_t normal=next_normal_epoch();
  if(!c.fresh||!normal)return false;
  durable_ota::Record candidate{};
  const auto* r=current();bool resolved_now=false;
  bool bench_manual=false;
#if HALO_OTA_BENCH_PROFILE
  // Only the existing bounded manual-input latch enables this branch. A
  // reason string or an automatic wake never grants a manual test campaign.
  bench_manual=r&&durable_ota::bench_live(*r,c)&&halo_ota_manual_override_active()&&
    (r->phase==durable_ota::Phase::RESOLVED||r->phase==durable_ota::Phase::BENCH_ABORTED||
     r->phase==durable_ota::Phase::BENCH_READY);
  if(bench_manual&&(!g_coord_credit_loaded||!g_coord_credit_mutations||retained_legacy||
      unresolved_legacy()||!peer_valid()||halo_primary_user_work_busy()))return false;
#endif
  if(r&&durable_ota::bench_active(*r)&&(r->phase==durable_ota::Phase::BENCH_ABORTED||
      r->phase==durable_ota::Phase::BENCH_READY||r->phase==durable_ota::Phase::RESOLVED||!durable_ota::bench_live(*r,c))&&!bench_manual)return false;
  if(r&&!boot_reconciled&&!bench_manual){
    boot_reconciled=true;
    // A verified new boot may resolve its exact target. Unknown interrupted
    // elapsed time stays charged; no historical calendar credit is emitted.
    if(r->phase!=durable_ota::Phase::RESOLVED&&
       r->phase!=durable_ota::Phase::DISCOVERY&&
       durable_ota::target_valid(r->target)&&peer_valid(r->target.peer_version)&&
       running_matches(r->target,work.original_start,work.original_budget)) {
      if(!durable_ota::resolve(*r,c,r->target,true,true,true,candidate)||
         !commit_candidate(candidate,work.original_start,work.original_budget))return false;
      diagnostic_observed_pair(candidate);
      diagnostic_terminal(candidate);
      resolved_now=true;
      Serial.println("[OTA_POLICY] target_valid resolution=observed_pair credit=unchanged");
    } else if((r->phase==durable_ota::Phase::APPLY||r->phase==durable_ota::Phase::PREFLIGHT)&&
              rollback_matches(*r,work.original_start,work.original_budget)) {
      if(!durable_ota::finish(*r,c,r->reserved_work_ms,true,durable_ota::Failure::ROLLBACK,0,0,normal,nullptr,candidate)||
         !commit_candidate(candidate,work.original_start,work.original_budget))return false;
      Serial.println("[OTA_POLICY] target_quarantined evidence=exact_invalid_image rollback_baseline=policy_aware");
    } else if(r->phase==durable_ota::Phase::ARM_PENDING) {
      if(!durable_ota::close_fast(*r,c,normal,candidate)||
         !commit_candidate(candidate,work.original_start,work.original_budget))return false;
    } else if(durable_ota::active_phase(*r)) {
      if(!durable_ota::reconcile_reset(*r,c,normal,candidate)||
         !commit_candidate(candidate,work.original_start,work.original_budget))return false;
    }
    r=current();
  }
  if(r&&r->phase==durable_ota::Phase::RESOLVED&&(resolved_now||retained_legacy)){
    // Only the original checked coordinator resolves its legacy credit/debt.
    // A later discovery cannot silently replace that still-pending origin.
    if(g_coord_pending[0]&&!strcmp(g_coord_pending,r->origin)&&peer_valid(r->target.peer_version)&&
       running_matches(r->target,work.original_start,work.original_budget))
      (void)ota_peer_schedule_complete();
    return false; // no speculative second GET/update after observing success
  }
  uint8_t campaign[16];for(unsigned i=0;i<4;++i){uint32_t v=esp_random();memcpy(campaign+i*4,&v,4);}
  char origin[64]={};strlcpy(origin,g_coord_pending[0]?g_coord_pending:reason?reason:"ordinary",sizeof(origin));
  bool first=absent();bool admitted=false;
  if(bench_manual){
    admitted=durable_ota::bench_manual_discovery(*r,c,true,retained_legacy,false,
      g_lcd_work_budget.remaining_ms(),origin,campaign,candidate)==durable_ota::Admission::ALLOWED;
  } else if(first){
    admitted=retained_legacy?
      durable_ota::start_legacy_discovery(origin,campaign,c,true,g_coord_pending[0],true,peer_valid(),false,candidate):
      durable_ota::start_discovery(origin,campaign,c,false,false,candidate);
  } else if(r) {
    // Keep the original unresolved identity regardless of a newer latest file
    // or a manual trigger. Only normal maintenance can replenish a new day.
    if(r->phase==durable_ota::Phase::DEFERRED&&!durable_ota::bench_active(*r)&&c.normal_maintenance&&c.epoch/86400UL>r->budget_day){
      if(!durable_ota::rollover(*r,c,candidate)||!commit_candidate(candidate,work.original_start,work.original_budget))return false;
      r=current();
    }
    if(r->phase==durable_ota::Phase::DISCOVERY||r->phase==durable_ota::Phase::RESOLVED){
      const bool bench_discovery=durable_ota::bench_active(*r)&&r->phase==durable_ota::Phase::DISCOVERY;
      if(bench_discovery&&(!g_coord_credit_loaded||!g_coord_credit_mutations||retained_legacy||
          unresolved_legacy()||!peer_valid()||halo_primary_user_work_busy()))return false;
      admitted=durable_ota::reserve_discovery(*r,c,retained_legacy,false,candidate,origin,campaign,
          halo_ota_manual_override_active())==durable_ota::Admission::ALLOWED;
    }
    else if(r->one_shot.phase==durable_ota::OneShotPhase::ARMED)
      admitted=durable_ota::one_shot_reserve(*r,c,g_coord_sense_boot_id,true,false,candidate)==durable_ota::Admission::ALLOWED;
    else admitted=durable_ota::reserve_preflight(*r,c,false,candidate)==durable_ota::Admission::ALLOWED;
  }
  if(!admitted||!commit_candidate(candidate,work.original_start,work.original_budget,first)){
    Serial.printf("[OTA_POLICY] defer phase=%u storage=%u normal=%u\n",current()?unsigned(current()->phase):255,unsigned(state_status),unsigned(work.normal));return false;
  }
  // This reservation was created in this boot, after checked settlement.
  if(bench_manual)boot_reconciled=true;
  work.live=true;work.phase_start=millis();work.phase_budget=current()->reserved_work_ms;
  clamp_pair();return remaining()!=0;
}
static bool manifest_matches(const OtaManifest& m) {
  if(!work.live||!remaining())return false;work.manifest=&m;
  const auto* r=current();return r&&(r->phase==durable_ota::Phase::DISCOVERY||matches_sense(r->target,m));
}
static bool bind_pair(const OtaManifest& lcd,const char* current_lcd) {
  if(!work.live||!work.manifest||!remaining()||!peer_valid(current_lcd))return false;
  const auto* r=current();if(!r)return false;
  durable_ota::Target target{};const auto& sense=*work.manifest;
  strlcpy(target.version,sense.version,sizeof(target.version));strlcpy(target.url,sense.url,sizeof(target.url));target.bytes=sense.size;
  if(!hex_sha(sense.sha256,target.sha256))return false;
  const bool update=compareSemver(lcd.version,current_lcd)>0;
  strlcpy(target.peer_version,lcd.version,sizeof(target.peer_version));
  target.peer_bytes=lcd.size;if(!hex_sha(lcd.sha256,target.peer_sha256))return false;
  if(!durable_ota::target_valid(target)||containsDisallowedHost(target.url)||!isAllowedOtaHost(target.url)||
     (update&&(!lcd.size||lcd.size>g_lcd_ota_query_resp_part_size||containsDisallowedHost(lcd.url)||!isAllowedOtaHost(lcd.url))))return false;
  durable_ota::Record candidate{};
  if(r->phase==durable_ota::Phase::DISCOVERY){
    const bool newer=compareSemver(sense.version,kFirmwareVersion)>0||update;
    if(r->deferred_path&&work.legacy){
      if(!durable_ota::bind_legacy_discovery(*r,fresh_clock(work.normal),target,true,
         !strcmp(r->origin,g_coord_pending),nvs_capacity_image_valid(),peer_valid(),candidate)||!commit(candidate))return false;
      r=current();
      // Existing downgrade/no-update resolver may prove supersession below.
      // A genuinely newer target waits until the next checked normal day.
      if(!newer)return true;
    } else {
      if(!newer)return true; // read-only no-update; finish closes accounting only
      if(!durable_ota::bind_discovery(*r,fresh_clock(work.normal),target,true,newer,candidate)||!commit(candidate))return false;
    }
    r=current();
  } else {
    // Once a peer was updated, its manifest identity remains the immutable
    // target even though this boot's fresh query now makes it a no-op.
    if(!matches_sense(r->target,sense)||strcmp(r->target.peer_version,lcd.version)||
       (r->target.peer_bytes&&(r->target.peer_bytes!=lcd.size||!hex_sha(lcd.sha256,target.peer_sha256)||
        memcmp(r->target.peer_sha256,target.peer_sha256,32))))return false;
  }
  if(r->phase==durable_ota::Phase::APPLY)return true;
  if(r->phase!=durable_ota::Phase::PREFLIGHT)return false;
  const uint32_t elapsed=uint32_t(millis()-work.phase_start);
  const uint32_t original_elapsed=uint32_t(millis()-work.original_start);
  if(original_elapsed>=work.original_budget||
     !durable_ota::reserve_apply(*r,fresh_clock(work.normal),elapsed,work.original_budget-original_elapsed,true,candidate)||!commit(candidate))return false;
  work.phase_start=millis();work.phase_budget=current()->reserved_work_ms;clamp_pair();return remaining()!=0;
}
static bool reserve_board_begin(unsigned board,const void* owner=nullptr) {
  if(!work.live||work.finished||!remaining()||!current()||current()->phase!=durable_ota::Phase::APPLY)return false;
  durable_ota::Record candidate{};
  return durable_ota::reserve_begin(*current(),fresh_clock(work.normal),board,candidate)&&commit(candidate,owner)&&remaining();
}
static bool self_begin(void* owner,SenseOtaApplier::FailureStage,uint32_t started,uint32_t budget) {
  if(owner!=&work||!work.live||!g_ota_apply_in_progress||uint32_t(millis()-started)>=budget)return false;
  SelfBeginLease lease(owner);return lease.active()&&reserve_board_begin(0,owner)&&uint32_t(millis()-started)<budget;
}
static SenseOtaApplier::BeginAdmission begin_admission{&work,self_begin};
static void classify(SenseOtaApplier::Result result) {
  const auto& f=g_ota_applier.getFailureSnapshot();work.stage=uint32_t(f.stage);work.error=f.sdk_error;
  // Coarse FAILED_WRITE and temporary HTTP/resource errors never quarantine.
  if(result==SenseOtaApplier::RESULT_FAILED_WRITE&&f.stage==SenseOtaApplier::FailureStage::END&&f.sdk_error==ESP_ERR_OTA_VALIDATE_FAILED)
    work.failure=durable_ota::Failure::EXACT_VALIDATION;
}
// Every failure is settled before a terminal unlock. Reserve the entire
// bounded arm exchange inside the still-live original work budget BEFORE its
// first query. A failed/lost ACK cannot mint a second arm or refund that work.
static bool arm_waiting() {
  return work.live&&!work.finished&&current()&&current()->phase==durable_ota::Phase::ARM_PENDING;
}
static void finish() {
  if(!work.live||work.finished)return;
  const auto* r=current();if(!r){work.finished=true;return;}
  const auto c=fresh_clock(work.normal);const uint32_t normal=next_normal_epoch();
  if(!normal||!durable_ota::clock_valid(*r,c)){work.finished=true;return;}
  durable_ota::Record candidate{};
  uint32_t elapsed=uint32_t(millis()-work.phase_start);
  if(r->phase==durable_ota::Phase::DISCOVERY){
    if(durable_ota::close_discovery(*r,c,elapsed,true,normal,candidate))commit(candidate);
    work.finished=true;return;
  }
  if(r->phase!=durable_ota::Phase::PREFLIGHT&&r->phase!=durable_ota::Phase::APPLY){work.finished=true;return;}
  const uint32_t left=remaining();
  const uint32_t arm_start=millis(); // persistence belongs to the charged five seconds
  const bool time_ok=left>=5000,user_idle=!self_retry_user_busy();
  const bool transport_ok=retry_transport_ready(),peer_ok=peer_valid();
  const bool can_arm=time_ok&&user_idle&&transport_ok&&peer_ok;
  // gates: time/user-idle/transport/peer. transport: clean-mode/free-UART/no-task.
  const unsigned gates=(time_ok?1U:0U)|(user_idle?2U:0U)|(transport_ok?4U:0U)|(peer_ok?8U:0U);
  const unsigned transport=(sense_lcd_ota_retry_safe()?1U:0U)|
    (!g_lcd_ota_proxy_owns_uart?2U:0U)|(!g_lcd_ota_task_running?4U:0U);
  // Capture the decision after policy settlement. Optional evidence cannot
  // extend the reserved exchange or change debt, credit, or its proof gates.
  auto arm_diagnostic=[&](const char* outcome){
    char detail[144];
    snprintf(detail,sizeof(detail),"A1 gen=%lu phase=%u gates=%u transport=%u left=%lu outcome=%s",
      current()?(unsigned long)current()->generation:0,current()?unsigned(current()->phase):0,
      gates,transport,(unsigned long)left,outcome);
    sense_lcd_terminal_store(detail,0);
    Serial.printf("[OTA_POLICY] %s\n",detail);
  };
  char request[64]={};
  if(can_arm)snprintf(request,sizeof(request),"retry_%08lx_%08lx_%lu",
      (unsigned long)r->created,(unsigned long)r->campaign[0],(unsigned long)r->attempt_ordinal);
  if(!durable_ota::finish(*r,c,elapsed+(can_arm?5000:0),true,work.failure,work.stage,work.error,
       normal,can_arm?request:nullptr,candidate)||
       !(can_arm?commit_candidate(candidate,arm_start,5000):commit(candidate))){work.finished=true;arm_diagnostic("settle_failed");return;}
  r=current();
  if(!r||r->phase!=durable_ota::Phase::ARM_PENDING){work.finished=true;arm_diagnostic("not_armed");return;}
  // No renewed pair deadline: the five-second exchange is the charge just
  // reserved above, and still must fit the invocation's original bound.
  // Check cleanup before starting/polling: a query response itself confirms
  // JSON mode, so it must never rehabilitate an unconfirmed failed transfer.
  auto control_live=[&](){return uint32_t(millis()-arm_start)<5000&&
    uint32_t(millis()-work.phase_start)<work.phase_budget&&
    uint32_t(millis()-work.original_start)<work.original_budget&&!self_retry_user_busy()&&retry_transport_ready()&&
    sense_time_has_fresh_sync();};
  char challenge[40];snprintf(challenge,sizeof(challenge),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
  LcdOtaQuerySnapshot peer{};bool ready=false;
  if(control_live()&&sense_lcd_ota_query_start(challenge,5000-uint32_t(millis()-arm_start)))while(control_live()){
    pump_uart_rx_once();const auto result=sense_lcd_ota_query_poll(peer);
    if(result==LCD_QUERY_READY){ready=retry_peer_ready(peer,*r);break;}
    if(result==LCD_QUERY_TIMEOUT)break;delay(10);
  }
  const auto now=fresh_clock();
  if(ready&&control_live()&&durable_ota::clock_valid(*r,now)&&uint64_t(now.epoch)+15<r->arm_epoch){
    MaintenanceWindow mw{};mw.duration_sec=mw.grace_before_sec=mw.grace_after_sec=0;
    mw.start_epoch=r->arm_epoch;strlcpy(mw.request_id,r->arm_id,sizeof(mw.request_id));
    g_self_retry_arm={};g_self_retry_arm.started_ms=arm_start;g_self_retry_arm.budget_ms=5000;
    g_self_retry_arm.peer_boot_id=peer.peer_boot_id;g_self_retry_arm.start_epoch=r->arm_epoch;
    g_self_retry_arm.remaining_s=r->arm_epoch-now.epoch;
    strlcpy(g_self_retry_arm.request_id,r->arm_id,sizeof(g_self_retry_arm.request_id));
    strlcpy(g_self_retry_arm.challenge,challenge,sizeof(g_self_retry_arm.challenge));g_self_retry_arm.waiting=true;
    send_maint_window(&mw,g_self_retry_arm.remaining_s,g_self_retry_arm.remaining_s,false,challenge,peer.peer_boot_id);
    while(control_live()&&!g_self_retry_arm.matched){pump_uart_rx_once();delay(10);}
    if(control_live()&&g_self_retry_arm.matched&&
       durable_ota::confirm_arm(*r,fresh_clock(),r->arm_id,r->arm_epoch,peer.peer_boot_id,peer.peer_boot_id,true,true,candidate)&&
       commit_candidate(candidate,arm_start,5000)&&control_live()){
      Serial.printf("[OTA_POLICY] retry_armed due=%lu expiry=%lu origin=%s target=%s\n",
        (unsigned long)current()->fast_due,(unsigned long)current()->fast_expiry,current()->origin,current()->target.version);
      g_self_retry_arm.waiting=false;work.finished=true;arm_diagnostic("armed");return;
    }
    g_self_retry_arm.waiting=false;
  }
  r=current();if(r&&control_live()&&durable_ota::close_fast(*r,fresh_clock(),normal,candidate))
    (void)commit_candidate(candidate,arm_start,5000);
  Serial.printf("[OTA_POLICY] retry_arm_unverified phase=%u pending_reconciliation=%u\n",
    current()?unsigned(current()->phase):0,current()&&current()->phase==durable_ota::Phase::ARM_PENDING?1:0);
  work.finished=true;arm_diagnostic(ready?"arm_unverified":"peer_unverified");
}
struct WorkCleanup {~WorkCleanup(){finish();work.live=false;work.manifest=nullptr;}};
#else
inline uint32_t remaining(){return g_lcd_work_budget.remaining_ms();}
inline bool enter(const char*,bool){return true;}
inline bool manifest_matches(const OtaManifest&){return true;}
inline bool bind_pair(const OtaManifest&,const char*){return true;}
inline bool reserve_board_begin(unsigned,const void* = nullptr){return true;}
inline void classify(SenseOtaApplier::Result){}
inline void finish(){}
struct WorkCleanup{};
#endif
} // namespace sense_policy

#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_allocated_held(){return sense_policy::allocated_held(sense_policy::state_scratch);}
static bool halo_policy_network_admitted(){return sense_policy::remaining()!=0;}
static bool halo_policy_lcd_begin(){return sense_policy::reserve_board_begin(1);}
static const char* halo_policy_lcd_manifest_version(){
  const auto* r=sense_policy::current();
  return r&&r->phase!=durable_ota::Phase::DISCOVERY&&durable_ota::target_valid(r->target)?r->target.peer_version:nullptr;
}
static bool halo_policy_arm_waiting(){return sense_policy::arm_waiting();}
static bool halo_policy_diagnostic_identity(const OtaManifest&m,uint8_t(&campaign)[16],char(&origin)[64],uint32_t&ordinal){
  const auto*r=sense_policy::current();
  if(!sense_policy::work.live||!r||!durable_ota::target_valid(r->target)||!sense_policy::matches_sense(r->target,m))return false;
  memcpy(campaign,r->campaign,16);memcpy(origin,r->origin,64);ordinal=r->attempt_ordinal;return true;
}
// Supplied immutable target is already persisted/admitted; this is not a
// fetched manifest or proof of the peer target. DISCOVERY has no such identity.
static void halo_policy_diagnostic_prefetch(uint32_t left){
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  sense_diag_suspend_capture();
  const auto*r=sense_policy::current();
  if(!left||!sense_policy::work.live||sense_policy::work.finished||!r||
      r->phase!=durable_ota::Phase::PREFLIGHT||!durable_ota::target_valid(r->target))return;
  sense_diag_begin_identity(r->target.version,r->target.sha256,r->target.bytes,
      r->origin,r->campaign,r->attempt_ordinal,left);
#else
  (void)left;
#endif
}
static bool halo_policy_diagnostic_peer_identity(const OtaManifest&m,uint8_t(&campaign)[16],char(&origin)[64]){
  const auto*r=sense_policy::current();uint8_t sha[32];
  if(!sense_policy::work.live||sense_policy::work.finished||!r||!durable_ota::target_valid(r->target)||
     strcmp(r->target.peer_version,m.version)||r->target.peer_bytes!=m.size||
     !sense_policy::hex_sha(m.sha256,sha)||memcmp(r->target.peer_sha256,sha,32))return false;
  memcpy(campaign,r->campaign,16);memcpy(origin,r->origin,64);return true;
}
static bool halo_policy_diagnostic_close_retained(const uint8_t*campaign,const char*origin){
  const auto*r=sense_policy::current();
  return r&&campaign&&origin&&durable_ota::target_valid(r->target)&&
    (r->phase==durable_ota::Phase::RESOLVED||r->phase==durable_ota::Phase::BENCH_ABORTED||memcmp(r->campaign,campaign,16)||strcmp(r->origin,origin));
}
static bool halo_policy_bind_pair(const OtaManifest&m,const char*fw){
  const bool admitted=sense_policy::bind_pair(m,fw);
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
  if(admitted&&sense_policy::work.manifest)sense_diag_begin(*sense_policy::work.manifest,sense_policy::remaining());
#endif
  return admitted;
}
static bool halo_policy_resolve_pair(){
  using namespace sense_policy;
  const auto* r=current();if(!work.live||!r)return false;
  if(r->phase==durable_ota::Phase::DISCOVERY){
#if HALO_OTA_BENCH_PROFILE
    // Called only after the existing pair/policy check returned success. Commit the
    // observed no-update outcome before optional reporting/unlock; an earlier
    // failure or reset stays DISCOVERY and cannot acquire this terminal state.
    if(durable_ota::bench_active(*r)){
      if(!remaining()||!peer_valid())return false;
      durable_ota::Record candidate{};
      if(!durable_ota::close_discovery(*r,fresh_clock(work.normal),
          uint32_t(millis()-work.phase_start),true,next_normal_epoch(),candidate,true)||
          !commit(candidate))return false;
      work.finished=true;
    }
#endif
    return true; // no target completion or calendar credit
  }
  if(!peer_valid(r->target.peer_version)||!running_matches(r->target,work.original_start,work.original_budget))return false;
  durable_ota::Record candidate{};
  if(!durable_ota::resolve(*r,fresh_clock(),r->target,true,true,true,candidate)||!commit(candidate))return false;
  diagnostic_observed_pair(candidate);
  diagnostic_terminal(candidate);
  work.finished=true;return true;
}
static bool halo_policy_notice_due(const char* id){
  using namespace sense_policy;
  const auto* r=current();const auto c=fresh_clock();
  if(!id||!r||!durable_ota::clock_valid(*r,c))return false;
  if(durable_ota::bench_live(*r,c)&&bench_retry_phase(*r)){
    char request[64];return bench_deferred_request(*r,request)&&!strcmp(id,request)&&uint64_t(c.epoch)+15>=r->not_before;
  }
  if(r->phase==durable_ota::Phase::ARMED&&!strcmp(id,r->arm_id))
    return uint64_t(c.epoch)+15>=r->fast_due&&c.epoch<r->fast_expiry;
  if(r->one_shot.phase==durable_ota::OneShotPhase::ARMED){
    char request[64];return durable_ota::one_shot_request(*r,request)&&!strcmp(id,request)&&
      uint64_t(c.epoch)+15>=r->one_shot.due&&c.epoch<r->one_shot.expiry;
  }
  return false;
}
// Observational only: last readiness decision in this actual boot. No NVS or
// second record; an unavailable clock is never replaced by retained wall time.
struct PolicyReadinessObservation {
  const char* decision="unobserved";
  uint32_t epoch=0,due=0,at_ms=0,left_ms=0;
  bool fresh=false,accepted_origin=false;
};
static PolicyReadinessObservation g_policy_readiness;
static void halo_policy_note_readiness(const char* decision,durable_ota::Clock c,
                                      uint32_t due=0,bool accepted=false) {
  const uint32_t now=millis();
  int32_t left=g_boot_ota_pending?int32_t(g_boot_ota_deadline_ms-now):0;
  if(g_peer_gate.active){const int32_t peer_left=int32_t(g_peer_gate.deadline_ms-now);
    if(peer_left<left)left=peer_left;}
  g_policy_readiness={decision,c.epoch,due,now,left>0?uint32_t(left):0,c.fresh,accepted};
}
static bool halo_policy_accepted_lcd_origin(const char* request) {
  // ota_peer_service consumes the mailbox before readiness. Its accepted
  // episode origin survives that consumption and is tied to the queried boot.
  // Retained debt may already have queued coord_recovery before that notice
  // arrives. Keep its original deadline and reason; the verified timer origin,
  // not the queue label, authorizes waiting for this arm's due time.
  return request&&request[0]&&g_boot_ota_pending&&g_lcd_timer_origin.boot_id&&
    g_lcd_timer_origin.boot_id==g_lcd_timer_seen_boot&&
    g_lcd_timer_origin.wake==int(ESP_SLEEP_WAKEUP_TIMER)&&
    !strcmp(g_lcd_timer_origin.schedule,request)&&g_peer_gate.active&&
    g_peer_gate.ready&&!g_peer_gate.legacy&&
    g_peer_gate.peer_boot==g_lcd_timer_origin.boot_id;
}
static bool halo_policy_boot_ready(){
  using namespace sense_policy;
  if(!nvs_capacity_image_valid()){
    halo_policy_note_readiness("local_not_valid",fresh_clock());return false;
  }
  // Storage consumes this original readiness opportunity. Re-sample both
  // deadlines after load; an early peer wake never receives a new interval.
  const uint32_t load_start=millis();
  const int32_t load_left=int32_t(g_boot_ota_deadline_ms-load_start);
  if(load_left<=0){halo_policy_note_readiness("deadline",fresh_clock());return false;}
  if(!load_state(load_start,uint32_t(load_left))){
    halo_policy_note_readiness("storage_wait",fresh_clock());return false;
  }
  const uint32_t now_ms=millis();
  int32_t readiness_left=int32_t(g_boot_ota_deadline_ms-now_ms);
  if(g_peer_gate.active){const int32_t peer_left=int32_t(g_peer_gate.deadline_ms-now_ms);
    if(peer_left<readiness_left)readiness_left=peer_left;}
  if(readiness_left<=0){halo_policy_note_readiness("deadline",fresh_clock());return false;}
  const auto* r=current();
  const auto c=fresh_clock(normal_entry());
  if(!r){halo_policy_note_readiness(absent()?"no_record":"storage_wait",c);return absent();}
  if(!durable_ota::clock_valid(*r,c)){halo_policy_note_readiness("clock_wait",c);return false;}
  bool wait=durable_ota::bench_active(*r)&&(!durable_ota::bench_live(*r,c)||r->phase==durable_ota::Phase::BENCH_READY||r->phase==durable_ota::Phase::BENCH_ABORTED||r->phase==durable_ota::Phase::RESOLVED);
  uint32_t due=0,expiry=0;bool accepted=false;
  const bool normal_deferred=!durable_ota::bench_active(*r)&&
    (r->phase==durable_ota::Phase::DEFERRED||
     (r->phase==durable_ota::Phase::DISCOVERY&&!durable_ota::active_phase(*r)));
  const uint32_t calendar_due=normal_deferred?normal_calendar_due():0;
  uint32_t calendar_wait_due=calendar_due;
  // The correlated preflight lock can precede the separate TIMER notice.
  // Give a missing notice only the existing <=15s lead to arrive. This
  // persisted arm is WAIT evidence only: normal_entry still requires the
  // actual timer origin at due, and neither original deadline is renewed.
  if(normal_deferred&&!calendar_wait_due&&!g_lcd_timer_notice.pending&&
     !g_lcd_timer_origin.boot_id&&!g_lcd_timer_seen_boot){
    const CoordinatorCreditState base=coord_credit_base();
    if(base.schedule.bound&&base.schedule.id[0])
      calendar_wait_due=base.schedule.target_epoch;
  }
  if(normal_deferred){
    due=r->not_before;
    if(calendar_wait_due>due)due=calendar_wait_due;
  }
  if(r->phase==durable_ota::Phase::DEFERRED||
     (durable_ota::bench_active(*r)&&bench_retry_phase(*r))){
    wait|=!c.normal_maintenance||c.epoch<r->not_before;
    if(durable_ota::bench_active(*r)){
      due=r->not_before;expiry=r->bench.until;char request[64];
      accepted=bench_deferred_request(*r,request)&&halo_policy_accepted_lcd_origin(request);
    }
  }
  if(r->phase==durable_ota::Phase::DISCOVERY&&!durable_ota::active_phase(*r))wait|=!c.normal_maintenance||c.epoch<r->not_before;
  if(r->phase==durable_ota::Phase::ARMED){
    due=r->fast_due;expiry=r->fast_expiry;accepted=halo_policy_accepted_lcd_origin(r->arm_id);
    wait|=c.epoch<due;
  }
  const bool one_shot=r->one_shot.phase==durable_ota::OneShotPhase::ARMED;
  if(one_shot){due=r->one_shot.due;expiry=r->one_shot.expiry;}
  const bool expired=(expiry&&c.epoch>=expiry)||
    (durable_ota::bench_active(*r)&&!durable_ota::bench_live(*r,c));
  if(due&&c.epoch<due&&!expired){
    // The LCD wakes up to 15s early. Its separate timer notice can lose a race
    // with the peer lock or fresh-time service. A persisted shipping arm and
    // current correlated peer suffice to WAIT inside that lead interval;
    // they do not authorize work before due or renew either deadline.
    const bool armed_peer_wait=r->phase==durable_ota::Phase::ARMED&&
      !durable_ota::bench_active(*r)&&!one_shot&&
      due-c.epoch<=durable_ota::kPeerLead&&g_boot_ota_pending&&
      g_peer_gate.active&&g_peer_gate.ready&&!g_peer_gate.legacy&&g_peer_gate.peer_boot;
    const bool calendar_peer_wait=normal_deferred&&calendar_wait_due&&!one_shot&&
      due-c.epoch<=durable_ota::kPeerLead&&g_boot_ota_pending&&
      g_peer_gate.active&&g_peer_gate.ready&&!g_peer_gate.legacy&&g_peer_gate.peer_boot;
    if((one_shot||accepted||armed_peer_wait||calendar_peer_wait)&&
       uint64_t(due-c.epoch)*1000<uint32_t(readiness_left)){
      halo_policy_note_readiness("wait_due",c,due,accepted);return false;
    }
    wait=true;
  }
  if(expired||r->phase==durable_ota::Phase::QUARANTINED)wait=true;
  if(wait){
    halo_policy_note_readiness(expired?"expired":"not_due",c,due,accepted);
    Serial.printf("[OTA_POLICY] readiness_deferred phase=%u target=%s\n",unsigned(r->phase),r->target.version);
    ota_peer_cancel("policy_not_due");boot_ota_finish("policy_not_due");
    g_peer_episode_finished=true;g_ota_check_done=true;return false;
  }
  halo_policy_note_readiness("ready",c,due,accepted);return true;
}
static uint32_t halo_policy_timer_delta(){
  using namespace sense_policy;
  const auto* r=current();const auto c=fresh_clock();
  if(!r||!durable_ota::clock_valid(*r,c))return 0;
  // Selecting normal after a missed opportunity explicitly closes acceleration
  // while preserving the immutable target and next-day deferred eligibility.
  const bool missed_fast=r->phase==durable_ota::Phase::ARMED&&c.epoch>=r->fast_due&&
    (!g_boot_ota_pending||g_peer_episode_finished||g_ota_check_done);
  const bool expired_fast=(r->phase==durable_ota::Phase::ARMED||r->phase==durable_ota::Phase::ARM_PENDING)&&c.epoch>=r->fast_expiry;
  const bool missed_one=r->one_shot.phase==durable_ota::OneShotPhase::ARMED&&
    (c.epoch>=r->one_shot.expiry||(c.epoch>=r->one_shot.due&&(!g_boot_ota_pending||g_peer_episode_finished||g_ota_check_done)));
  if(missed_fast||expired_fast||missed_one){
    const uint32_t normal=next_normal_epoch();durable_ota::Record candidate{};
    const bool changed=missed_one?durable_ota::one_shot_close(*r,c,normal,candidate):
      durable_ota::close_fast(*r,c,normal,candidate);
    if(changed&&commit_candidate(candidate,millis(),1500))Serial.println("[OTA_POLICY] short_window_closed deferred=normal");
    return 0;
  }
  const uint32_t deferred=bench_deferred_delta(*r,c);if(deferred)return deferred;
  const uint32_t retry=durable_ota::timer_delta(*r,c);if(retry)return retry;
  const auto one=durable_ota::one_shot_timer(*r,c);
  return one.state==durable_ota::OneShotSelection::WAIT?one.delta_s:0;
}
#endif
#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_short_due(){
  const auto* r=sense_policy::current();const auto c=sense_policy::fresh_clock();
  if(!r||!durable_ota::clock_valid(*r,c))return false;
  if(sense_policy::bench_deferred_due(*r,c))return true;
  if(durable_ota::bench_active(*r)&&!durable_ota::bench_live(*r,c))return false;
  if(r->phase==durable_ota::Phase::ARMED)return c.epoch>=r->fast_due&&c.epoch<r->fast_expiry;
  return r->one_shot.phase==durable_ota::OneShotPhase::ARMED&&c.epoch>=r->one_shot.due&&c.epoch<r->one_shot.expiry;
}
// Observational boot classification only: no SDK VALID prerequisite, policy
// cache/admission/write, wall-clock assumption or credit. Failure keeps15s.
// The caller owns the main task and latches this once before requesting DNS.
static uint32_t halo_policy_ntp_budget(uint32_t started) {
#if HALO_DURABLE_OTA_POLICY
  using namespace sense_policy;
  uint32_t cap=55000;
  if(g_boot_ota_pending) {
    if(int32_t(g_boot_ota_deadline_ms-started)<=0) return 0;
    const uint32_t left=g_boot_ota_deadline_ms-started;
    if(left<cap) cap=left;
  }
  const uint32_t ordinary=cap<15000?cap:15000;
  if(work.live || self_retry_user_busy()) return ordinary;
  StorageScope scope(started,cap<1500?cap:1500);
  if(!scope.active()) return ordinary;
  durable_ota::Record observed{};bool allowed=false;
  durable_ota::StorageStatus status{};
  const auto read=durable_ota::load_nvs(scope.guard(),true,observed,allowed,
                                      state_scratch,status);
  if(read!=durable_ota::ReadResult::PRESENT || !allowed ||
     !durable_ota::active_phase(observed) ||
     (durable_ota::bench_active(observed) && HALO_OTA_BENCH_PROFILE!=1)) return ordinary;
  return cap;
#else
  (void)started;return 15000;
#endif
}

static void halo_policy_service_boot(){
  static bool queued=false,load_started=false;
  static uint32_t load_start=0;
  if(queued||!nvs_capacity_image_valid()||sense_policy::work.live)return;
  if(!load_started){load_started=true;load_start=millis();}
  if(!sense_policy::state_loaded&&!sense_policy::load_state(load_start,1500))return;
  const auto* r=sense_policy::current();if(!r){queued=true;return;}
  const auto c=sense_policy::fresh_clock(sense_policy::normal_entry());
  if(!durable_ota::clock_valid(*r,c)){halo_policy_note_readiness("clock_wait",c);return;}
  const bool due=halo_policy_short_due()||durable_ota::active_phase(*r)||
      ((r->phase==durable_ota::Phase::DEFERRED||r->phase==durable_ota::Phase::DISCOVERY)&&
       c.normal_maintenance&&c.epoch>=r->not_before);
  if(!due){if(!g_boot_ota_pending&&!g_peer_episode_finished&&!g_ota_check_done)
    halo_policy_note_readiness("before_due",c,r->phase==durable_ota::Phase::ARMED?r->fast_due:r->not_before);return;}
  // Queue once into the existing main-loop120s readiness path. No new timer,
  // task, or peer wake is created, and active policy work is not admitted here.
  queued=true;if(!g_boot_ota_pending&&!g_peer_episode_finished&&!g_ota_check_done)
    boot_ota_queue("policy_recovery");
}
#endif
#if HALO_DURABLE_OTA_POLICY
// Called only AFTER the existing checked downgrade-policy/legacy resolver.
// Records that the target is superseded, not that its historical OTA succeeded.
static bool halo_policy_resolve_superseded(){
  using namespace sense_policy;const auto* r=current();
  if(!work.live||!r)return false;
  if(r->phase==durable_ota::Phase::DISCOVERY)return halo_policy_resolve_pair(); // checked read-only policy completion
  if(!durable_ota::target_valid(r->target)||!nvs_capacity_image_valid()||
     compareSemver(kFirmwareVersion,r->target.version)<=0||!peer_valid(r->target.peer_version))return false;
  durable_ota::Record candidate{};if(!durable_ota::next(*r,fresh_clock(),candidate))return false;
  durable_ota::clear_active(candidate);candidate.phase=durable_ota::Phase::RESOLVED;
  if(candidate.one_shot.phase!=durable_ota::OneShotPhase::NONE)candidate.one_shot.phase=durable_ota::OneShotPhase::CLOSED;
  if(!durable_ota::shape(candidate)||!commit(candidate))return false;
  diagnostic_terminal(candidate);
  Serial.println("[OTA_POLICY] legacy_superseded credit=unchanged transfer_success=0");work.finished=true;return true;
}
#endif

#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_bench_deferred_arm(uint32_t&due,uint32_t&delta,char*id,size_t capacity){
 const auto*r=sense_policy::current();if(!r||!id||capacity<64)return false;
 delta=sense_policy::bench_deferred_delta(*r,sense_policy::fresh_clock());if(!delta)return false;
 char request[64];if(!sense_policy::bench_deferred_request(*r,request))return false;
 due=r->not_before;strlcpy(id,request,capacity);return true;
}
#endif
