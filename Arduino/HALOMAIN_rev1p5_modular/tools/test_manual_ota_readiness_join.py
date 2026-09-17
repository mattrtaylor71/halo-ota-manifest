"""Actual manual request -> readiness -> primary-owner wait -> durable entry.

Reuses the existing recovery/NVS boundary harness. Executes production request,
clock/deadline cleanup, boot_ready, user-owner predicate, coordinator preparation,
enter and codec. No hardware/network. Optional frozen source is a negative control.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import tempfile
import test_ota_discovery_recovery as recovery
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root, negative=None):
    wrapper = (root / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino').read_text()
    runtime = (root / 'halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h').read_text()
    request_source = wrapper if negative is None else (negative / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino').read_text()
    source = recovery.harness(root).split('int main(){', 1)[0]
    source = source.replace('static bool manual_request=true;static bool halo_ota_manual_override_active(){return manual_request;}',
                            'static bool manual_request=true;bool halo_ota_manual_override_active();')
    source = source.replace('static bool user_busy=false;static bool halo_primary_user_work_busy(){return user_busy;}', r'''
static bool user_busy=false;
struct Job{bool active=false;}current_job;
static bool upload_inflight=false,http_inflight=false,voice_recording_active=false;
static bool scan_ui_inflight=false,dish_scan_inflight=false,foreground_active=false,upload_worker_has_parked_job=false;
static unsigned upload_count=0,op_count=0;static void* op_queue=nullptr;
static unsigned upload_queue_count(){return upload_count;}
static unsigned uxQueueMessagesWaiting(void*){return op_count;}
static bool halo_provisioning_active(){return false;}
''' + definition(wrapper, 'static bool halo_primary_user_work_busy() {'))
    source = source.replace('static void ota_peer_cancel(const char*){events.push_back("completion_cancel");}',
                            'static void ota_peer_cancel(const char*);')
    source = source.replace('static void boot_ota_finish(const char*){g_boot_ota_pending=false;events.push_back("boot_finish");}',
                            'static void boot_ota_finish(const char*);')
    source = source.replace('void printf(const char*,T...){}', 'void printf(const char*f,T...a){char b[256];snprintf(b,sizeof(b),f,a...);events.push_back(b);}')
    source += r'''
#define LOG_INFO(...) do{}while(0)
static bool g_manual_ota_override=false,g_manual_ota_joined_readiness=false;
static unsigned long g_manual_ota_override_until_ms=0,g_manual_ota_request_ms=0,g_manual_ota_wifi_retry_ms=0;
static const unsigned long MANUAL_OTA_OVERRIDE_TTL_MS=300000;
static bool g_ota_check_requested=false,g_ota_check_in_progress=false,g_ota_apply_in_progress=false;
static bool g_lcd_ota_task_running=false,g_lcd_ota_proxy_owns_uart=false;
static bool g_lcd_ota_request_active=false,g_ota_pending_verify_active=false;
static bool g_ota_simple_proof_started=false,g_ota_simple_proof_done=false;
static bool g_self_retry_boot=false,g_self_retry_execution=false,g_ota_skip_logged=false,g_lcd_ota_done=false,g_lcd_ota_attempted_this_window=false;
static char g_lcd_work_schedule[64]{},g_last_ota_result[32]="none";
static bool g_maintenance_mode=false,g_maintenance_handled=false;
static const char* g_boot_ota_reason="coord_recovery";
static std::string wire_result;
static bool is_time_valid(){return clock_fresh;}
namespace OtaIntent{
 static unsigned updates=0,clears=0;static bool force=false;
 static void updateDesired(const char*,const char*,bool f,bool,uint32_t,const char*){++updates;force=f;}
 static void clearForceAndCheck(){++clears;force=false;}
}
static void ota_set_last_result(const char*s){strlcpy(g_last_ota_result,s,sizeof(g_last_ota_result));events.push_back(std::string("result:")+s);}
static void ota_peer_send_lock(bool release){if(release){wire_result=g_last_ota_result;events.push_back("terminal_unlock");}}
static void uart_send_sense_diag(const char*,const char*,const char*,int,const char*){}
static bool set_lcd_ota_due_nvs(bool b){debt_value=b;return true;}
'''
    source = source.replace('struct{char detail[64]{},reason[64]{};int code=0;}g_boot_ota_begin_record;',
                            'struct{char detail[64]{},reason[64]{};int code=0;bool repeat_pending=false,noop_reported=false;}g_boot_ota_begin_record;')
    source = source.replace('bool active=true,ready=true,entered=false,locked=true,legacy=false;',
                            'bool active=true,ready=true,entered=false,locked=true,legacy=false,querying=false;')
    for sig in ('static void manual_ota_override_set(', 'static void manual_ota_override_clear(',
                'bool halo_ota_manual_override_active() {', 'static bool manual_ota_joined_readiness_active()',
                'static bool ota_peer_accept_new_request() {', 'static bool manual_ota_join_readiness(',):
        source += definition(wrapper, sig) + '\n'
    source += definition(request_source, 'void halo_prod_request_manual_ota(') + '\n'
    for sig in ('static void boot_ota_finish(', 'static void ota_peer_cancel(const char* reason) {'):
        source += definition(wrapper, sig) + '\n'
    source += 'namespace sense_policy {\n' + definition(runtime, 'static const char* refusal_result(') + '\n}\n'
    source += definition(wrapper, 'static bool ota_primary_work_ready()') + '\n'
    source += r'''
static uint32_t g_lcd_timer_seen_boot=0;
static struct{bool pending=false;char schedule[64]{};}g_lcd_timer_notice;
static struct{uint32_t boot_id=0;int wake=0;char schedule[64]{};}g_lcd_timer_origin;
static constexpr int ESP_SLEEP_WAKEUP_TIMER=4;
namespace sense_policy{
static uint32_t normal_calendar_due(){return 0;}
static bool bench_retry_phase(const durable_ota::Record&){return false;}
static bool bench_deferred_request(const durable_ota::Record&,char(&)[64]){return false;}
}
'''
    source += definition(runtime, 'struct PolicyReadinessObservation') + ';\nstatic PolicyReadinessObservation g_policy_readiness;\n'
    for sig in ('static void halo_policy_note_readiness(', 'static bool halo_policy_accepted_lcd_origin(', 'static bool halo_policy_boot_ready()'):
        source += definition(runtime, sig) + '\n'
    # Execute actual deadline branches, preserving their result/unlock ordering.
    nightly = definition(wrapper, 'static void nightly_maintenance_tick()')
    start = nightly.index('  if ((int32_t)(now_ms - g_boot_ota_deadline_ms) >= 0) {')
    branch = definition(nightly[start:], 'if ((int32_t)')
    source += 'static void boot_deadline(){const bool nightly=false;' + branch + '}\n'
    service = definition(wrapper, 'static void ota_peer_service() {')
    start = service.index('  if ((int32_t)(now - g_peer_gate.deadline_ms) >= 0 ||')
    source += 'static void peer_deadline(){const uint32_t now=millis();' + definition(service[start:], 'if ((int32_t)') + '}\n'
    # Extract source gates; credit and enter definitions themselves are already
    # actual production functions in the reused harness.
    common = definition(wrapper, 'static void maybeRunOtaCheck(')
    owner_gate = 'if (!ota_primary_work_ready()) return;'
    recheck = 'if (!ota_peer_ready() || !ota_primary_work_ready()) return;'
    assert common.index(owner_gate) < common.index('coord_credit_prepare_work(reason)') < common.index(recheck) < common.index('g_peer_gate.entered = true;')
    source += r'''
static unsigned prep_calls=0,entered_calls=0;static bool become_busy_during_prepare=false;
static void manual_pipeline(){
 if(!ota_peer_ready())return;
''' + owner_gate + r'''
 ++prep_calls;if(!coord_credit_prepare_work("mqtt_cmd"))return;
 if(become_busy_during_prepare)http_inflight=true;
''' + recheck + r'''
 g_peer_gate.entered=true;g_lcd_work_budget={now_ms,120000};g_lcd_work_budget_live=true;
 ++entered_calls;
 if(!sense_policy::enter("mqtt_cmd",true)){
  ota_set_last_result(sense_policy::refusal_result(halo_ota_manual_override_active()));
  g_ota_check_requested=false;OtaIntent::clearForceAndCheck();manual_ota_override_clear("policy_refused");
  g_ota_check_done=true;ota_peer_cancel("transaction_return");boot_ota_finish("check_started");return;
 }
 manual_ota_override_clear("ota_check_begin");
}
static void reset_join(){
 reset_recovery();g_manual_ota_override=g_manual_ota_joined_readiness=false;
 g_manual_ota_override_until_ms=g_manual_ota_request_ms=g_manual_ota_wifi_retry_ms=0;
 g_ota_check_requested=g_ota_check_in_progress=g_ota_apply_in_progress=false;
 g_lcd_ota_task_running=g_lcd_ota_proxy_owns_uart=false;g_self_retry_boot=g_self_retry_execution=false;
 g_lcd_ota_request_active=g_ota_pending_verify_active=g_ota_simple_proof_started=g_ota_simple_proof_done=false;
 g_peer_gate.entered=false;g_peer_gate.deadline_ms=now_ms+30000;g_boot_ota_deadline_ms=now_ms+60000;
 g_lcd_work_budget_live=g_peer_continue_work=false;current_job={};upload_inflight=http_inflight=false;
 voice_recording_active=scan_ui_inflight=dish_scan_inflight=foreground_active=upload_worker_has_parked_job=false;
 upload_count=op_count=0;op_queue=nullptr;OtaIntent::updates=OtaIntent::clears=0;OtaIntent::force=false;
 strcpy(g_last_ota_result,"none");wire_result.clear();prep_calls=entered_calls=0;become_busy_during_prepare=false;
}
'''
    worker = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    assignment = 'current_job = job;\n      current_job.active = true;'
    assert assignment in worker
    source += 'static void begin_list(){Job job{};' + assignment + '}\n'
    source += r'''
#define HALO_DEFER_UPLOADS_TO_SLEEP 1
static bool g_upload_flush_requested=false;
static unsigned long g_upload_hold_since_ms=0,UPLOAD_HOLD_MAX_MS=600000;
static const uint32_t UPLOAD_HOLD_HIGHWATER=8;
struct UploadJob{bool from_voice_sd=false,from_image_sd=false,from_persisted=false;};
static void* upload_queue=(void*)1;static constexpr int pdTRUE=1;
static UploadJob queue_head{};static bool peek_fails=false;
static unsigned peek_calls=0;
static int xQueuePeek(void*,UploadJob*job,int){++peek_calls;if(!upload_count||peek_fails)return 0;*job=queue_head;return pdTRUE;}
static struct{bool load(){return false;}}g_media_retry_user_paused;
static bool foreground_priority_active(uint32_t,const char**){return false;}
''' + definition(worker, 'static bool uploads_held_for_session(') + '\n'
    source += definition(wrapper, 'static bool ota_yield_automatic_readiness_to_fresh_media()') + '\n'
    early_gate = 'if (ota_yield_automatic_readiness_to_fresh_media()) return;'
    assert service.index(early_gate) < service.index('coord_credit_cancel_future_notice()')
    assert common.index(early_gate) < common.index('ota_peer_ready()')
    loop = definition(wrapper, 'void halo_prod_loop()')
    assert loop.index('ota_peer_service();') < loop.index('if (sense_action_inflight())')
    # Execute the exact service-entry block up to its existing coordinator
    # boundary. The old ordering reaches that boundary and leaves the sleep
    # hold live whenever queued media prevents peer readiness.
    prefix = service[service.index('{') + 1:service.index('  coord_credit_cancel_future_notice();')]
    source += r'''
static unsigned later_readiness_calls=0;
static void automatic_readiness_service(){
''' + prefix + r'''
 ++later_readiness_calls;
}
static void reset_automatic(){reset_join();queue_head={};peek_fails=false;peek_calls=0;
 upload_queue=(void*)1;upload_count=1;later_readiness_calls=0;g_upload_flush_requested=false;
}
'''

    if negative is not None:
        return source + r'''
int main(){reset_join();const auto before=sense_policy::store.bytes;halo_prod_request_manual_ota("manual");
 CHECK(!g_manual_ota_override&&!OtaIntent::force&&!g_ota_check_requested);
 CHECK(!halo_policy_boot_ready()&&!g_boot_ota_pending&&before==sense_policy::store.bytes);
 puts("REPRODUCED178: actual old request drops early manual; actual readiness closes without durable admission");}
'''
    return source + r'''
int main(){using namespace sense_policy;using namespace durable_ota;
 // Unentered automatic readiness yields before unrelated peer/clock work,
 // including an unavailable peer, stale clock, and an existing bounded lock.
 for(unsigned ready=0;ready<2;++ready){for(unsigned fresh=0;fresh<2;++fresh){
  reset_automatic();g_peer_gate.ready=ready;clock_fresh=fresh;
  const auto before=store.bytes;const auto credit_before=g_coord_credit;
  const auto target_before=state_record.target;const bool debt_before=debt_value;
  const auto boot_dead=g_boot_ota_deadline_ms,peer_dead=g_peer_gate.deadline_ms;
  const auto pending=std::string(g_coord_pending),completion=std::string(g_coord_completion_target);
  CHECK(uploads_held_for_session(nullptr));automatic_readiness_service();
  CHECK(later_readiness_calls==0&&!g_boot_ota_pending&&!g_peer_gate.active&&g_peer_episode_finished&&g_ota_check_done);
  CHECK(wire_result=="policy_deferred"&&at("result:policy_deferred")<at("terminal_unlock"));
  CHECK(upload_count==1&&!g_upload_flush_requested&&store.bytes==before);
  CHECK(!memcmp(&credit_before,&g_coord_credit,sizeof(credit_before))&&!memcmp(&target_before,&state_record.target,sizeof(target_before)));
  CHECK(debt_value==debt_before&&pending==g_coord_pending&&completion==g_coord_completion_target);
  CHECK(g_boot_ota_deadline_ms==boot_dead&&g_peer_gate.deadline_ms==peer_dead&&OtaIntent::clears==0&&OtaIntent::updates==0);
  CHECK(clock_fresh==bool(fresh));
  const auto event_count=events.size();CHECK(!ota_yield_automatic_readiness_to_fresh_media()&&events.size()==event_count);
 }}
 // Before the first peer query, fresh media needs no peer lock/round trip.
 reset_automatic();g_peer_gate.active=g_peer_gate.locked=g_peer_gate.ready=false;
 automatic_readiness_service();CHECK(later_readiness_calls==0&&wire_result.empty()&&!g_boot_ota_pending);
 // An old target awaiting recovery is retained byte-for-byte, including its
 // exhausted counters and reserved work; readiness is not resolution/credit.
 {reset();const auto old_target_record=state_record;reset_automatic();
  state_record=old_target_record;uint8_t raw[kRecordBytes];CHECK(encode(state_record,raw));
  store.bytes.assign(raw,raw+sizeof(raw));const auto before=store.bytes;
  strcpy(g_coord_completion_target,old_target_record.target.version);OtaIntent::force=true;
  automatic_readiness_service();CHECK(!g_boot_ota_pending&&later_readiness_calls==0);
  CHECK(store.bytes==before&&!memcmp(&state_record,&old_target_record,sizeof(state_record)));
  CHECK(!strcmp(g_coord_completion_target,old_target_record.target.version)&&debt_value&&OtaIntent::force&&OtaIntent::clears==0);
 }
 // All active ownership and explicit/manual requests retain their old path.
 for(unsigned i=0;i<18;++i){reset_automatic();switch(i){
  case 0:g_boot_ota_pending=false;break;case 1:local_valid=false;break;
  case 2:g_manual_ota_override=true;break;case 3:g_manual_ota_joined_readiness=true;break;
  case 4:g_ota_check_requested=true;break;case 5:g_peer_episode_finished=true;break;
  case 6:g_ota_check_done=true;break;case 7:g_peer_gate.entered=true;break;
  case 8:g_lcd_work_budget_live=true;break;case 9:g_peer_continue_work=true;break;
  case 10:g_self_retry_execution=true;break;case 11:g_ota_check_in_progress=true;break;
  case 12:g_ota_apply_in_progress=true;break;case 13:g_ota_pending_verify_active=true;break;
  case 14:g_lcd_ota_task_running=true;break;case 15:g_lcd_ota_proxy_owns_uart=true;break;
  case 16:g_lcd_ota_request_active=true;break;case 17:g_ota_simple_proof_started=true;break;}
  const auto before=store.bytes;const auto boot_pending=g_boot_ota_pending;
  const auto entered=g_peer_gate.entered,live=g_lcd_work_budget_live;
  automatic_readiness_service();CHECK(later_readiness_calls==1&&peek_calls==0&&events.empty());
  CHECK(store.bytes==before&&g_boot_ota_pending==boot_pending&&g_peer_gate.entered==entered&&g_lcd_work_budget_live==live&&upload_count==1);
 }
 // Saved custody, empty or racing/missing FIFO head cannot trigger this rule.
 for(unsigned i=0;i<7;++i){reset_automatic();switch(i){
  case 0:queue_head.from_voice_sd=true;break;case 1:queue_head.from_image_sd=true;break;
  case 2:queue_head.from_persisted=true;break;case 3:upload_count=0;break;
  case 4:upload_queue=nullptr;break;case 5:peek_fails=true;break;
  case 6:upload_count=0;upload_worker_has_parked_job=true;break;}
  const auto count=upload_count;automatic_readiness_service();
  CHECK(later_readiness_calls==1&&g_boot_ota_pending&&g_peer_gate.active&&events.empty()&&upload_count==count);
 }
 reset_automatic();halo_prod_request_manual_ota("manual");const auto attached_dead=g_manual_ota_override_until_ms;
 automatic_readiness_service();CHECK(later_readiness_calls==1&&g_manual_ota_joined_readiness&&OtaIntent::force&&g_boot_ota_pending);
 CHECK(g_manual_ota_override_until_ms==attached_dead&&OtaIntent::updates==1&&OtaIntent::clears==0);
 reset_join();begin_list();const auto bytes=store.bytes;const auto original=state_record;
 const uint32_t boot_dead=g_boot_ota_deadline_ms,peer_dead=g_peer_gate.deadline_ms;
 halo_prod_request_manual_ota("manual");CHECK(g_manual_ota_joined_readiness&&g_manual_ota_override&&OtaIntent::updates==1);
 const auto latch_dead=g_manual_ota_override_until_ms;
 for(unsigned i=0;i<5;++i){++now_ms;halo_prod_request_manual_ota("manual");}
 CHECK(g_boot_ota_deadline_ms==boot_dead&&g_peer_gate.deadline_ms==peer_dead&&g_manual_ota_override_until_ms==latch_dead&&OtaIntent::updates==1);
 CHECK(halo_policy_boot_ready());manual_pipeline();CHECK(prep_calls==0&&entered_calls==0&&store.bytes==bytes);
 current_job.active=false;http_inflight=true;manual_pipeline();CHECK(prep_calls==0&&store.bytes==bytes);
 http_inflight=false;g_peer_gate.proof_ms=now_ms;manual_pipeline();CHECK(entered_calls==1&&work.live);
 CHECK(state_record.network_windows==1&&state_record.day_attempts==0&&!memcmp(original.campaign,state_record.campaign,16));
 CHECK(g_boot_ota_deadline_ms==boot_dead&&g_peer_gate.deadline_ms==peer_dead&&!g_manual_ota_joined_readiness);
 // Busy arrival during coordinator persistence cannot enter a transaction.
 reset_join();halo_prod_request_manual_ota("manual");become_busy_during_prepare=true;
 manual_pipeline();CHECK(prep_calls==1&&entered_calls==0&&!g_peer_gate.entered&&state_record.network_windows==2);
 // Fresh wake-list concurrency also waits on a clean independent manual.
 reset_join();g_boot_ota_pending=false;g_peer_gate.active=false;begin_list();
 halo_prod_request_manual_ota("manual");CHECK(g_manual_ota_override&&!g_manual_ota_joined_readiness&&g_ota_check_requested);
 g_peer_gate.active=true;manual_pipeline();CHECK(prep_calls==0&&entered_calls==0);
 current_job.active=false;manual_pipeline();CHECK(work.live&&entered_calls==1);
 // Actual defer-until-sleep predicate makes one fresh queue item dependent on
 // session drain; readiness must promptly release without forcing that drain.
 reset_join();upload_count=1;g_upload_flush_requested=false;g_upload_hold_since_ms=0;
 CHECK(uploads_held_for_session(nullptr));halo_prod_request_manual_ota("manual");
 const auto held_bytes=store.bytes;manual_pipeline();
 CHECK(wire_result=="policy_deferred"&&upload_count==1&&!g_upload_flush_requested&&store.bytes==held_bytes&&!g_peer_gate.active);
 // Actual owner predicate covers every admitted user/HTTP/parked owner.
 for(unsigned i=0;i<9;++i){reset_join();halo_prod_request_manual_ota("manual");
  switch(i){case 0:upload_inflight=true;break;case 1:voice_recording_active=true;break;
   case 2:scan_ui_inflight=true;break;case 3:dish_scan_inflight=true;break;case 4:foreground_active=true;break;
   case 5:upload_worker_has_parked_job=true;break;case 6:upload_count=1;break;
   case 7:op_queue=(void*)1;op_count=1;break;case 8:current_job.active=true;break;}
  const auto before=store.bytes;manual_pipeline();CHECK(prep_calls==0&&entered_calls==0&&before==store.bytes);
  if(i==5||i==6){CHECK(wire_result=="policy_deferred"&&!g_peer_gate.active&&!g_boot_ota_pending&&!OtaIntent::force);CHECK(upload_worker_has_parked_job||upload_count==1);}
  else CHECK(g_peer_gate.active&&g_boot_ota_pending);

 }
 // Both real readiness timeout branches send truthful failure before unlock.
 for(unsigned peer=0;peer<2;++peer){reset_join();begin_list();halo_prod_request_manual_ota("manual");
  const auto before=store.bytes;now_ms=peer?g_peer_gate.deadline_ms:g_boot_ota_deadline_ms;
  if(peer)peer_deadline();else boot_deadline();
  CHECK(wire_result=="peer_unavailable"&&!g_manual_ota_override&&!g_manual_ota_joined_readiness&&!OtaIntent::force&&!g_ota_check_requested);
  CHECK(before==store.bytes&&entered_calls==0);
  CHECK(at("result:peer_unavailable")<at("terminal_unlock"));
 }
 // Future-calendar terminal refusal clears attached input without early credit.
 reset_join();halo_prod_request_manual_ota("manual");const auto future_bytes=store.bytes;
 ota_peer_cancel("calendar_future_rearm");boot_ota_finish("calendar_future_rearm");
 CHECK(wire_result=="policy_deferred"&&!g_manual_ota_override&&!OtaIntent::force&&store.bytes==future_bytes);
 // Wrap and repeated taps retain the original interval, including deadline0.
 reset_join();now_ms=0xfffffff0U;g_boot_ota_deadline_ms=20;g_peer_gate.deadline_ms=0;
 halo_prod_request_manual_ota("manual");CHECK(g_manual_ota_override_until_ms==0&&halo_ota_manual_override_active());
 now_ms=0xfffffff8U;halo_prod_request_manual_ota("manual");CHECK(g_manual_ota_override_until_ms==0&&OtaIntent::updates==1);
 now_ms=0;CHECK(!halo_ota_manual_override_active()&&!g_manual_ota_joined_readiness&&!OtaIntent::force);
 // Genuine in-progress/check/continuation ownership never gains new intent.
 for(unsigned i=0;i<7;++i){reset_join();switch(i){case 0:g_peer_gate.entered=true;break;
  case 1:g_ota_check_in_progress=true;break;case 2:g_ota_apply_in_progress=true;break;
  case 3:g_lcd_ota_task_running=true;break;case 4:g_lcd_ota_proxy_owns_uart=true;break;
  case 5:g_lcd_work_budget_live=true;break;case 6:g_peer_continue_work=true;break;}
  halo_prod_request_manual_ota("manual");CHECK(!g_manual_ota_override&&OtaIntent::updates==0);
 }
 // Fresh readiness cannot waive exhausted allowance or unresolved target due.
 reset_join();halo_prod_request_manual_ota("manual");state_record.budget_day=epoch/86400;state_record.budget_granted=state_record.high_water=epoch;strcpy(state_record.origin,g_coord_pending);
 uint8_t raw[kRecordBytes];CHECK(encode(state_record,raw));store.bytes.assign(raw,raw+sizeof(raw));
 CHECK(halo_policy_boot_ready());manual_pipeline();CHECK(wire_result=="policy_daily_limit"&&!OtaIntent::force&&!g_manual_ota_joined_readiness);
 reset_join();Record target_fixture;reset();target_fixture=state_record;reset_join();
 Record deferred;CHECK(finish(target_fixture,{epoch,true,false},target_fixture.reserved_work_ms,true,Failure::TEMPORARY,0,0,epoch+86400,nullptr,deferred));state_record=deferred;
 CHECK(encode(state_record,raw));store.bytes.assign(raw,raw+sizeof(raw));const auto target_before=state_record.target;
 halo_prod_request_manual_ota("manual");CHECK(halo_policy_boot_ready());manual_pipeline();
 CHECK(!work.live&&wire_result=="policy_deferred"&&!OtaIntent::force);
 CHECK(!memcmp(&target_before,&state_record.target,sizeof(target_before))&&state_record.phase==Phase::DEFERRED);
 // Named missing proof reports actual busy owner; success stays mask0.
 reset_join();g_peer_gate.entered=true;g_lcd_work_budget_live=true;current_job.active=true;
 CHECK(!enter("manual",true));bool named=false;for(const auto&e:events)if(e.find("missing=0x0100")!=std::string::npos)named=true;CHECK(named);
 printf("PASS %u manual/readiness/owner/deadline/policy/diagnostic checks\n",checks);
}
'''


def run(root, out, negative=None):
    out.mkdir(parents=True, exist_ok=False)
    src = out / 'test.cpp'; src.write_text(harness(root, negative))
    command = ['c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-I' + str(root / 'halo_ota_demo/firmware/shared'), str(src), '-o', str(out / 'test')]
    built = subprocess.run(command, capture_output=True, text=True, timeout=30)
    (out / 'compile.log').write_text(built.stdout + built.stderr)
    if built.returncode: raise RuntimeError('Compile failed: ' + str(out / 'compile.log'))
    result = subprocess.run([str(out / 'test')], capture_output=True, text=True, timeout=20)
    (out / 'run.log').write_text(result.stdout + result.stderr);print(result.stdout + result.stderr, end='')
    paths = ['halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino', 'halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h', 'halo_ota_demo/firmware/shared/DurableOtaDiscoveryRecovery.h', 'tools/test_manual_ota_readiness_join.py']
    receipt = {'passed': result.returncode == 0, 'source_root': str(root), 'negative_source': str(negative) if negative else None,
               'sources': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths}, 'hardware_actions': 0, 'network_actions': 0}
    (out / 'RESULT.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if result.returncode: raise RuntimeError('Test failed: ' + str(out / 'run.log'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__);parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path);parser.add_argument('--negative-source-root', type=Path)
    args = parser.parse_args()
    if args.out: run(args.source_root, args.out, args.negative_source_root)
    else:
        with tempfile.TemporaryDirectory(prefix='manual-ota-join-') as directory: run(args.source_root, Path(directory) / 'run', args.negative_source_root)
