#pragma once
// Included after the actual coordinator and one-shot helper. All calls remain
// on its existing main-task parser/loop/sleep paths.
#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_bench_command(JsonDocument&doc){return sense_bench::command(doc);}
static bool halo_policy_one_shot_command(JsonDocument& doc){
  const uint32_t started=millis();
  sense_one_shot::Request request{};
  if(!sense_one_shot::parse(doc.as<JsonObjectConst>(),request)){
    Serial.println("[OTA_ONE_SHOT] refused schema_or_disabled");return false;
  }
  // This is the command's one original, charged control opportunity. It cannot
  // borrow a running coordinator/peer lease or renew an existing request.
  if(g_boot_ota_pending||g_peer_gate.active||g_ota_check_in_progress||g_ota_check_requested||
     sense_action_inflight()){
    Serial.println("[OTA_ONE_SHOT] refused coordinator_busy");return false;
  }
  const auto result=sense_one_shot::execute(request,started,durable_ota::kPreflightMs);
  Serial.printf("[OTA_ONE_SHOT] result=%u\n",unsigned(result));
  return result==sense_one_shot::Result::Armed;
}
static bool halo_policy_one_shot_ack(uint32_t remaining,uint32_t wake,bool clear,const char*request,
    const char*status,bool persisted,uint64_t due,uint32_t duration,uint32_t before,uint32_t after,
    const char*challenge,uint32_t peer_boot){
  return sense_one_shot::accept_ack(remaining,wake,clear,request,status,persisted,due,duration,before,after,challenge,peer_boot);
}
static int32_t halo_policy_arm_timer(uint32_t&seconds,uint32_t normal_or_user_seconds){
  // Nothing that can pump/report/block is placed between this proof and the
  // actual SDK call. Shorten to the still-future absolute target if clock time
  // advanced since the earlier candidate selection; never postpone it.
  const uint32_t started=millis();
  const auto proof=sense_one_shot::select_timer();
  // Reconstruct from independent user/calendar selection and CURRENT durable
  // proof. An earlier short candidate may have been closed by co-scheduling.
  seconds=normal_or_user_seconds;
  const auto* retry=sense_policy::current();const auto selected=sense_policy::fresh_clock();
  uint32_t retry_delta=retry?durable_ota::timer_delta(*retry,selected):0;
  if(retry){const uint32_t bench=sense_policy::bench_deferred_delta(*retry,selected);if(bench&&(!retry_delta||bench<retry_delta))retry_delta=bench;}
  if(retry_delta&&retry_delta<seconds)seconds=retry_delta;
  if(proof.selection==durable_ota::OneShotSelection::DUE||
     proof.selection==durable_ota::OneShotSelection::EXPIRED||
     proof.selection==durable_ota::OneShotSelection::CLOCK){
    // The earlier short candidate may have expired while settling sleep.
    // Keep the independently selected calendar/drain/pin interval, never an
    // obsolete acceleration. Closing below preserves the deferred target.
    seconds=normal_or_user_seconds;
  }
  if(proof.selection==durable_ota::OneShotSelection::WAIT&&proof.microseconds<uint64_t(seconds)*1000000ULL)
    seconds=uint32_t(proof.microseconds/1000000ULL);
  const uint64_t actual=uint64_t(seconds)*1000000ULL;
  const esp_err_t error=esp_sleep_enable_timer_wakeup(actual);
  if(proof.selection==durable_ota::OneShotSelection::WAIT&&actual==proof.microseconds){
    const auto result=sense_one_shot::note_timer(proof,actual,error,started,1500);
    const auto* r=sense_policy::current();const auto after=sense_policy::fresh_clock();
    const bool confirmed=error==ESP_OK&&result==sense_one_shot::Result::TimerRecorded&&uint32_t(millis()-started)<1500&&
      r&&r->generation==proof.generation+1&&r->one_shot.phase==durable_ota::OneShotPhase::ARMED&&
      r->one_shot.timer_recorded&&r->one_shot.timer_us==actual&&r->one_shot.timer_sdk==error&&
      r->one_shot.arm_boot==g_coord_sense_boot_id&&after.fresh&&after.epoch>=proof.selected.epoch&&
      after.epoch<r->one_shot.due;
    Serial.printf("[OTA_ONE_SHOT] timer_result=%u readback=%u sdk=%ld us=%llu\n",
      unsigned(result),unsigned(confirmed),(long)error,(unsigned long long)actual);
    if(!confirmed){
      // A successful SDK call alone is not a durable accelerated arm. Never
      // leave that short timer selected after a failed proof or readback.
      if(r&&uint32_t(millis()-started)<1500){
        durable_ota::Record closed{};
        if(durable_ota::one_shot_close(*r,sense_policy::fresh_clock(),sense_policy::next_normal_epoch(),closed))
          (void)sense_policy::commit_candidate(closed,started,1500);
      }
      seconds=normal_or_user_seconds;
      const esp_err_t replacement=esp_sleep_enable_timer_wakeup(uint64_t(seconds)*1000000ULL);
      if(replacement!=ESP_OK)(void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
      Serial.printf("[OTA_ONE_SHOT] unverified_timer_replaced sdk=%ld us=%llu deferred=normal\n",
        (long)replacement,(unsigned long long)uint64_t(seconds)*1000000ULL);
      return replacement;
    }
  } else if(proof.selection==durable_ota::OneShotSelection::DUE||proof.selection==durable_ota::OneShotSelection::EXPIRED){
    // Explicitly retire a missed accelerated opportunity. This does not label
    // its old SDK argument as verified or erase its normally deferred target.
    (void)halo_policy_timer_delta();
    Serial.println("[OTA_ONE_SHOT] timer_opportunity_missed deferred=normal");
  }
  return error;
}
static void halo_policy_append_report(JsonDocument&doc){
  doc["ota_clock_fresh"]=sense_time_has_fresh_sync();
  doc["ota_ready_decision"]=g_policy_readiness.decision;
  doc["ota_ready_epoch"]=g_policy_readiness.epoch;
  doc["ota_ready_due"]=g_policy_readiness.due;
  doc["ota_ready_ms"]=g_policy_readiness.at_ms;
  doc["ota_ready_left_ms"]=g_policy_readiness.left_ms;
  doc["ota_ready_fresh"]=g_policy_readiness.fresh;
  doc["ota_ready_origin"]=g_policy_readiness.accepted_origin;
  const auto* r=sense_policy::current();
  doc["ota_policy_storage"]=unsigned(sense_policy::state_status);
  if(r){
    doc["ota_policy_phase"]=unsigned(r->phase);doc["ota_policy_generation"]=r->generation;
    doc["ota_policy_origin"]=r->origin;doc["ota_policy_target"]=r->target.version;
    // Direct causal identity; a boot counter is not this random boot token.
    if(g_coord_sense_boot_id)doc["ota_policy_sense_boot"]=g_coord_sense_boot_id;
    static const char digits[]="0123456789abcdef";
    char campaign[33];for(unsigned i=0;i<16;++i){campaign[2*i]=digits[r->campaign[i]>>4];campaign[2*i+1]=digits[r->campaign[i]&15];}campaign[32]=0;
    if(durable_ota::nonzero(r->campaign,16))doc["ota_policy_campaign"]=campaign;
    if(durable_ota::target_valid(r->target)){
      char sha[65];for(unsigned i=0;i<32;++i){sha[2*i]=digits[r->target.sha256[i]>>4];sha[2*i+1]=digits[r->target.sha256[i]&15];}sha[64]=0;
      doc["ota_policy_target_sha256"]=sha;doc["ota_policy_target_bytes"]=r->target.bytes;
    }
    doc["ota_policy_day"]=r->budget_day;doc["ota_policy_network_windows"]=r->network_windows;
    doc["ota_policy_apply_attempts"]=r->day_attempts;doc["ota_policy_sense_begins"]=r->begins[0];doc["ota_policy_lcd_begins"]=r->begins[1];
    doc["ota_policy_attempt_ordinal"]=r->attempt_ordinal;
    doc["ota_policy_work_remaining_ms"]=r->work_remaining_ms;doc["ota_policy_reserved_ms"]=r->reserved_work_ms;
    doc["ota_policy_retry_due"]=r->fast_due;doc["ota_policy_retry_expiry"]=r->fast_expiry;doc["ota_policy_next_normal"]=r->not_before;
  }
  sense_one_shot::append_report(doc);sense_bench::append_report(doc);
}
#endif
