"""Run actual recovery admission, enter(), codecs and commit/readback boundaries.

Uses the existing postboot harness's SDK/NVS doubles; no device or network.
The historical fixture contains OTA metadata only, never provisioning data.
"""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile

import test_postboot_policy_settlement as postboot
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]


def composition(source, runtime, wrapper):
    source = source.replace('static bool normal_entry(){return false;}', 'static bool normal_entry(){return normal_window;}')
    source = source.replace('if(typed_absent){', 'if(typed_absent&&!wrote){')
    source = source.replace('struct OtaManifest {};', '''struct OtaManifest {
 char version[32]="6.4.162",url[256]="https://example.com/sense162.bin";
 char sha256[65]="1111111111111111111111111111111111111111111111111111111111111111";
 uint32_t size=1800000;
};
static OtaManifest sense_manifest,lcd_manifest;
static std::vector<std::string> done_ids;
static bool proxy_ok=true,normal_window=false;
static bool containsDisallowedHost(const char*){return false;}
static bool isAllowedOtaHost(const char*){return true;}
static bool sense_lcd_ota_retry_safe(){return proxy_ok;}
''')
    source = source.replace('static bool get_lcd_ota_due_nvs(){return false;}',
                            'static bool get_lcd_ota_due_nvs();')
    source = source.replace('static bool ota_peer_schedule_completed(const char*){return already_completed;}',
                            '''static bool ota_peer_schedule_completed(const char* id){
 return already_completed||std::find(done_ids.begin(),done_ids.end(),id)!=done_ids.end();
}''')
    old_completion = definition(source, 'static CoordCompletion ota_peer_schedule_complete()')
    source = source.replace(old_completion, 'static CoordCompletion ota_peer_schedule_complete();')
    source = source.replace('static uint32_t remaining(){return g_lcd_work_budget.remaining_ms();}',
                            definition(runtime, 'static uint32_t remaining()'))
    source = source.replace('static void clamp_pair(){}', '''static void clamp_pair(){}
static bool commit(const durable_ota::Record&r){
 return commit_candidate(r,work.original_start,work.original_budget);
}
''')
    # This block runs in the completed production-function namespace, before
    # the existing host reset helpers. No framework/network implementation is
    # copied: only manifest/UART/atomic history storage boundaries are doubled.
    marker = '// Model the existing caller boundaries only:'
    pos = source.index(marker)
    extension = 'namespace sense_policy {\n'
    for signature in ('static bool hex_sha(', 'static bool matches_sense(', 'static bool bind_pair('):
        extension += definition(runtime, signature) + '\n'
    extension += '}\n'
    for signature in ('static bool halo_policy_resolve_pair()',
                      'static bool halo_policy_close_checked_discovery()',
                      'static bool halo_policy_resolve_superseded()'):
        extension += definition(runtime, signature) + '\n'
    extension += r'''
static const esp_partition_t* esp_ota_get_boot_partition(){return &running;}
static bool get_lcd_ota_due_nvs(){return debt_value;}
static bool g_peer_continue_work=false;
static char g_coord_schedule[64]{};
static bool halo_policy_short_due(){return false;}
static void ensure_timezone_pt(const char*){}
static bool coord_credit_retire_configured_timezone(){return tz_ok;}
static bool coord_credit_cancel_future_notice(){return false;}
static bool coord_credit_calendar_entry(const char*){return true;}
static void coord_credit_clock(uint64_t&n,bool&fresh,char(&tz)[64],bool&finished){
 n=epoch;fresh=clock_fresh;snprintf(tz,sizeof(tz),"%s",fixture_tz);finished=true;
}
static bool coord_credit_budget_open(){return g_lcd_work_budget_live&&g_lcd_work_budget.remaining_ms();}
static bool ota_storage_known_clear(){return debt_read_ok&&!debt_value;}
static bool coord_history_commit(){
 if(!legacy_write_ok)return false;
 events.push_back("done_ids");done_ids.push_back(g_coord_pending);++legacy_commits;
 g_coord_pending[0]=0;g_lcd_work_budget_live=false;return true;
}
struct Preferences{bool begin(const char*,bool){return true;}void remove(const char*){}void end(){}};
'''
    extension += definition(wrapper, 'static CoordinatorCreditState coord_credit_base()') + '\n'
    extension += definition(wrapper, 'static void coord_credit_publish()') + '\n'
    extension += r'''
static bool coord_credit_save(const CoordinatorCreditState&candidate){
 if(!legacy_write_ok)return false;
 uint8_t bytes[COORDINATOR_CREDIT_BYTES];CoordinatorCreditState decoded;
 if(!credit_encode(candidate,bytes)||!credit_decode(bytes,sizeof(bytes),decoded))return false;
 g_coord_credit=decoded;coord_credit_publish();return true;
}
'''
    extension += definition(wrapper, 'static bool coord_credit_prepare_work(') + '\n'
    extension += definition(wrapper, 'static CoordCompletion ota_peer_schedule_complete()') + '\n'
    extension += r'''
static char g_lcd_ota_result[64]{},g_lcd_ota_version[32]{};
static bool g_boot_ota_diag_context=false,g_boot_ota_begin_reported=false;
struct{char detail[64]{},reason[64]{};int code=0;}g_boot_ota_begin_record;
static bool prod_proxy_lcd_inline(bool report=true){
 (void)report;if(!proxy_ok||!sense_policy::bind_pair(lcd_manifest,g_lcd_ota_query_resp_fw))return false;
 if(compareSemver(lcd_manifest.version,g_lcd_ota_query_resp_fw)>0||!sense_policy::peer_valid())return false;
 strcpy(g_lcd_ota_result,"noop");strcpy(g_lcd_ota_version,g_lcd_ota_query_resp_fw);
 events.push_back("pair_verified");debt_value=false;return true;
}
'''
    extension += definition(wrapper, 'static bool complete_downgrade_policy_check(') + '\n'
    return source[:pos] + extension + source[pos:]


def harness(root):
    postboot.ROOT = root
    postboot.SHARED = root / 'halo_ota_demo/firmware/shared'
    postboot.WRAPPER = root / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino'
    postboot.RUNTIME = postboot.SHARED / 'SenseDurablePolicyRuntime.h'
    postboot.STATE = postboot.SHARED / 'SenseDurablePolicyState.h'
    runtime = postboot.RUNTIME.read_text()
    source = postboot.harness().split('int main(){', 1)[0]
    source = source.replace('#include <cassert>', '#include <cassert>\n#include <mutex>\n#include <cstdlib>\n#include "DurableOtaDiscoveryRecovery.h"')
    source = source.replace('char owner[32]="owner";', 'char owner[32]="owner",challenge[40]="nonce";')
    source = source.replace('static bool halo_ota_manual_override_active(){return false;}',
                            'static bool manual_request=true;static bool halo_ota_manual_override_active(){return manual_request;}')
    source = source.replace('static bool halo_primary_user_work_busy(){return false;}',
                            'static bool user_busy=false;static bool halo_primary_user_work_busy(){return user_busy;}')
    extra = r'''
static CoordinatorCreditState g_coord_credit;
static bool g_coord_credit_uncertain=false,g_ota_storage_uncertain=false;
static bool g_serial_install_uncertain=false,g_nvs_reclaim_uncertain=false;
static bool debt_read_ok=true,debt_value=true,tz_ok=true,already_completed=false;
static unsigned debt_reads=0;
static bool ota_storage_read_debt(bool& due){++debt_reads;due=debt_value;return debt_read_ok;}
static bool ota_peer_schedule_completed(const char*){return already_completed;}
static std::recursive_mutex g_time_mutex,config_mutex;
namespace ProvisioningState{static std::recursive_mutex& timezoneMutex(){return config_mutex;}}
static const char* fixture_tz="PST8PDT,M3.2.0,M11.1.0";
static bool coord_credit_timezone_matches_configuration(char (&out)[64]){
 snprintf(out,sizeof(out),"%s",fixture_tz);return tz_ok;
}
static char g_lcd_query_coord_id[40]="nonce",g_lcd_query_coord_owner[32]="owner";
static uint32_t g_lcd_query_coord_lease_ms=120000,g_lcd_ota_query_resp_part_size=2621440;
'''
    source = source.replace('namespace sense_policy {', extra + '\nnamespace sense_policy {', 1)
    old = 'static durable_ota::Admission reserve_proved_discovery_recovery(const durable_ota::Record&,\n durable_ota::Clock,bool,durable_ota::Record&){return durable_ota::Admission::LEGACY;}'
    assert source.count(old) == 1
    source = source.replace(old, '')
    source = source.replace('static void remember_retry_baseline(){}', 'static bool peer_valid(const char* expected=nullptr);\n' + definition(runtime, 'static void remember_retry_baseline()'))
    source = source.replace('static bool peer_valid(const char* expected=nullptr) {', 'static bool peer_valid(const char* expected) {')
    enter = definition(runtime, 'static bool enter(')
    source = source.replace(enter, definition(runtime, 'static durable_ota::Admission reserve_proved_discovery_recovery(') + '\n' + enter)
    source = composition(source, runtime, postboot.WRAPPER.read_text())
    return source + r'''
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %u: %s\n",unsigned(__LINE__),#x);exit(1);}}while(0)
static durable_ota::Record historical(){
 using namespace durable_ota;Record r{};r.generation=4;r.phase=Phase::DISCOVERY;
 r.campaign[0]=1;strcpy(r.origin,"mqtt_cmd");r.created=r.budget_granted=1789408929;
 r.high_water=1789413612;r.budget_day=20710;r.not_before=1789462800;
 r.network_windows=2;r.work_remaining_ms=2394349;CHECK(shape(r));return r;
}
static void reset_recovery(){
 reset();using namespace sense_policy;
 epoch=1789633600;now_ms=10000;manual_request=true;user_busy=false;
 kFirmwareVersion="6.4.178";strcpy(g_lcd_ota_query_resp_fw,"6.4.178");
 g_coord_credit_loaded=g_coord_credit_mutations=true;
 g_coord_credit_uncertain=g_ota_storage_uncertain=g_serial_install_uncertain=g_nvs_reclaim_uncertain=false;
 debt_read_ok=debt_value=tz_ok=true;already_completed=false;debt_reads=0;done_ids.clear();proxy_ok=true;normal_window=false;
 setenv("TZ",fixture_tz,1);tzset();
 strcpy(g_coord_pending,"nightly_20260915");g_coord_completion_target[0]=0;
 g_coord_credit={};strcpy(g_coord_credit.pending.id,g_coord_pending);
 strcpy(g_coord_credit.pending.timezone,fixture_tz);g_coord_credit.pending.bound=true;
 g_coord_credit.pending.target_epoch=1789462800;g_coord_credit.pending_credit_admitted=true;
 g_coord_credit.admitted_epoch=1789462800;
 strcpy(g_coord_credit.schedule.id,"nightly_20260917");g_coord_credit.schedule.bound=true;g_coord_credit.schedule_observed=true;
 strcpy(g_coord_credit.schedule.timezone,fixture_tz);g_coord_credit.schedule.target_epoch=1789635600;
 strcpy(g_coord_credit.deferred.id,"nightly_20260916");g_coord_credit.deferred.bound=true;
 strcpy(g_coord_credit.deferred.timezone,fixture_tz);g_coord_credit.deferred.target_epoch=1789549200;
 strcpy(g_lcd_query_coord_id,"nonce");strcpy(g_lcd_query_coord_owner,"owner");
 g_lcd_query_coord_lease_ms=120000;g_lcd_ota_query_resp_part_size=2621440;
 g_peer_gate.entered=true;strcpy(g_peer_gate.challenge,"nonce");
 state_record=historical();uint8_t b[durable_ota::kRecordBytes];CHECK(durable_ota::encode(state_record,b));
 store.bytes.assign(b,b+sizeof(b));store.wrote=false;events.clear();work={};boot_reconciled=false;
}
static durable_ota::DiscoveryRecoveryProof proof(){
 return {&g_coord_credit,g_coord_pending,g_coord_completion_target,true,true,true,true,true,true,true};
}
static void next_invocation(){
 using namespace sense_policy;work={};g_lcd_work_budget_live=true;
 g_lcd_work_budget={now_ms,120000};g_peer_gate.proof_ms=now_ms;g_peer_gate.deadline_ms=now_ms+120000;
}
static void composition_cases(){
 using namespace durable_ota;using namespace sense_policy;
 // Actual policy bind + complete_downgrade wrapper, host manifest/UART boundaries.
 reset_recovery();CHECK(enter("manual",true));events.clear();work.manifest=&sense_manifest;
 CHECK(complete_downgrade_policy_check("6.4.162","manual"));
 CHECK(!active_phase(state_record)&&target_empty(state_record.target)&&work.finished);
 CHECK(at("pair_verified")<at("policy_write")&&at("policy_readback")<at("done_ids"));
 CHECK(!g_coord_pending[0]&&!debt_value&&done_ids.size()==1);
 CHECK(state_record.network_windows==1&&state_record.day_attempts==0);
 // Actual claim logic uses today's admission epoch for0916. Caller retained
 // snapshot was false BEFORE prepare. The real enter must still adopt0916.
 ++epoch;++now_ms;next_invocation();CHECK(coord_credit_prepare_work("manual"));
 CHECK(!strcmp(g_coord_pending,"nightly_20260916")&&g_coord_credit.admitted_epoch==epoch);
 CHECK(enter("manual",false));CHECK(!strcmp(state_record.origin,g_coord_pending)&&state_record.network_windows==2);
 work.manifest=&sense_manifest;CHECK(complete_downgrade_policy_check("6.4.162","manual"));
 CHECK(done_ids.size()==2&&!g_coord_pending[0]);
 epoch=1789635601;++now_ms;next_invocation();CHECK(coord_credit_prepare_work("manual"));
 CHECK(!strcmp(g_coord_pending,"nightly_20260917"));
 CHECK(!enter("manual",false)&&last_admission==Admission::BUDGET);CHECK(done_ids.size()==2);
 epoch+=86400;++now_ms;next_invocation();CHECK(enter("manual",true));
 CHECK(state_record.network_windows==1&&!strcmp(state_record.origin,"nightly_20260917"));
 work.manifest=&sense_manifest;CHECK(complete_downgrade_policy_check("6.4.162","manual"));CHECK(done_ids.size()==3);
 // Failed accounting cannot complete pending. This uses actual readback commit.
 for(unsigned fault=0;fault<2;++fault){reset_recovery();CHECK(enter("manual",true));
  work.manifest=&sense_manifest;events.clear();if(fault)fail_readback=true;else fail_write=true;
  CHECK(!complete_downgrade_policy_check("6.4.162","manual"));CHECK(done_ids.empty()&&g_coord_pending[0]);
 }
 // Cut after checked close but before coordinator write: no postboot credit,
 // retain spent window and recheck both manifests within second charged grant.
 reset_recovery();CHECK(enter("manual",true));work.manifest=&sense_manifest;legacy_write_ok=false;
 CHECK(!complete_downgrade_policy_check("6.4.162","manual"));
 CHECK(work.finished&&!active_phase(state_record)&&done_ids.empty()&&g_coord_pending[0]);
 const auto cut_record=state_record;work={};boot_reconciled=false;legacy_write_ok=true;next_invocation();++epoch;
 CHECK(!postboot_completion_ready());CHECK(enter("manual",true));
 CHECK(state_record.network_windows==2&&state_record.work_remaining_ms==cut_record.work_remaining_ms-kPreflightMs);
 work.manifest=&sense_manifest;CHECK(complete_downgrade_policy_check("6.4.162","manual"));
 CHECK(done_ids.size()==1);
 // No pair/clock/proof or mismatched identity can close the accounting/credit.
 for(unsigned fault=0;fault<5;++fault){reset_recovery();CHECK(enter("manual",true));work.manifest=&sense_manifest;
  if(fault==0)proxy_ok=false;if(fault==1)clock_fresh=false;if(fault==2)local_valid=false;
  if(fault==3)strcpy(g_lcd_query_running_state,"INVALID");if(fault==4)strcpy(g_coord_pending,"other");
  CHECK(!complete_downgrade_policy_check("6.4.162","manual"));CHECK(done_ids.empty());
 }
 // Absent first normal campaign still gets normal accounting, not migration.
 reset_recovery();state_present=false;state_record={};store.bytes.clear();typed_absent=true;
 CHECK(enter("nightly",false));CHECK(!state_record.deferred_path&&state_record.day_attempts==0);
 // Actual RESOLVED->DISCOVERY comparison target remains eligible without
 // discarding completed identity or historical destructive counters.
 for(unsigned fault=0;fault<4;++fault){
  reset_recovery();Record previous{},initial,preflight,applied,begun;
  strcpy(previous.target.version,"6.4.178");strcpy(previous.target.peer_version,"6.4.178");
  strcpy(previous.target.url,sense_manifest.url);previous.target.bytes=sense_manifest.size;
  previous.target.peer_bytes=lcd_manifest.size;previous.target.sha256[0]=1;previous.target.peer_sha256[0]=2;
  uint8_t original_campaign[16]={3},campaign[16]={2};Record opened,closed;
  CHECK(start(previous.target,"earlier_update",original_campaign,{epoch-86420,true,true},false,initial));
  CHECK(reserve_preflight(initial,{epoch-86419,true,true},false,preflight)==Admission::ALLOWED);
  CHECK(reserve_apply(preflight,{epoch-86418,true,true},1000,120000,true,applied));
  CHECK(reserve_begin(applied,{epoch-86417,true,true},0,begun));
  if(fault==3){Record failed;CHECK(finish(begun,{epoch-86416,true,true},1000,true,Failure::TEMPORARY,0,0,epoch-60,nullptr,failed));begun=failed;}
  CHECK(resolve(begun,{epoch-86415,true,true},begun.target,true,true,true,previous));
  CHECK(reserve_discovery(previous,{epoch,true,true},false,false,opened,"completed_comparison",campaign,true)==Admission::ALLOWED);
  CHECK(close_discovery(opened,{epoch+1,true,false},1000,true,epoch+86400,closed));
  state_record=closed;uint8_t bytes[kRecordBytes];CHECK(encode(closed,bytes));store.bytes.assign(bytes,bytes+sizeof(bytes));
  epoch+=2;g_coord_credit.admitted_epoch=epoch;
  if(fault==1)flash_hash_matches=false;
  if(fault==2)strcpy(g_lcd_ota_query_resp_fw,"6.4.177");
  const auto before=store.bytes;
  if(fault){CHECK(!enter("manual",false));CHECK(store.bytes==before);
   if(fault==3){CHECK(last_admission==Admission::NOT_DUE);auto evidence=proof();evidence.completed_comparison=true;Record nextday;
    CHECK(reserve_recovery_discovery(closed,{epoch+86400,true,true},evidence,false,nextday)==Admission::ALLOWED);
    CHECK(nextday.deferred_path&&nextday.network_windows==1&&!memcmp(&nextday.target,&closed.target,sizeof(closed.target)));
    // Execute the retained-owner normal-day path through actual bind_pair.
    // A completed comparison must not enter the empty legacy migration bridge.
    epoch+=86400;++now_ms;normal_window=true;next_invocation();
    CHECK(enter("nightly",true));CHECK(work.legacy&&state_record.deferred_path);
    CHECK(state_record.fast_opportunities==closed.fast_opportunities);
    work.manifest=&sense_manifest;CHECK(complete_downgrade_policy_check("6.4.162","nightly"));
    CHECK(!memcmp(&state_record.target,&closed.target,sizeof(closed.target))&&done_ids.size()==1);
   }
  }
  else {CHECK(enter("manual",false));CHECK(state_record.network_windows==2);
   CHECK(!memcmp(&closed.target,&state_record.target,sizeof(closed.target)));
   CHECK(state_record.attempt_ordinal==closed.attempt_ordinal&&state_record.day_attempts==closed.day_attempts);
   work.manifest=&sense_manifest;CHECK(complete_downgrade_policy_check("6.4.162","manual"));
  }
 }
 // A genuinely newer manifest still binds immutable target and ordinary APPLY.
 reset_recovery();CHECK(enter("manual",true));OtaManifest newer=sense_manifest;
 strcpy(newer.version,"6.4.179");work.manifest=&newer;
 CHECK(bind_pair(lcd_manifest,"6.4.178"));CHECK(state_record.phase==Phase::APPLY);
 const auto bound=state_record.target;OtaManifest changed=newer;changed.sha256[0]='2';work.manifest=&changed;
 CHECK(!bind_pair(lcd_manifest,"6.4.178"));CHECK(!memcmp(&bound,&state_record.target,sizeof(bound)));
}
int main(){
 using namespace durable_ota;using namespace sense_policy;
 reset_recovery();const Record original=state_record;Record n{};
 CHECK(reserve_discovery(original,{epoch,true,false},true,false,n,"manual",nullptr,true)==Admission::LEGACY);
 puts("REPRODUCED177: actual discovery admission rejects retained later coordinator debt before GET");
 const auto credit_before=g_coord_credit;
 CHECK(enter("mqtt_cmd",true));CHECK(work.live);
 CHECK(state_record.generation==5&&!strcmp(state_record.origin,g_coord_pending));
 CHECK(!memcmp(state_record.campaign,original.campaign,16)&&state_record.created==original.created);
 CHECK(state_record.budget_day==epoch/86400&&state_record.network_windows==1);
 CHECK(state_record.work_remaining_ms==kDailyWorkMs-kPreflightMs&&state_record.reserved_work_ms==kPreflightMs);
 CHECK(state_record.day_attempts==0&&!state_record.begins[0]&&!state_record.begins[1]);
 CHECK(!memcmp(&credit_before,&g_coord_credit,sizeof(credit_before))&&debt_value&&g_coord_pending[0]);
 CHECK(at("policy_write")<at("policy_readback")&&at("policy_readback")<at("policy_published"));
 CHECK(legacy_commits==0);
 // Close/retry uses the adopted origin, preserving spent same-day windows.
 Record closed;CHECK(close_discovery(state_record,{epoch+1,true,false},1000,true,epoch+86400,closed));
 CHECK(reserve_recovery_discovery(closed,{epoch+2,true,false},proof(),true,n)==Admission::ALLOWED);
 CHECK(n.network_windows==2&&n.work_remaining_ms==closed.work_remaining_ms-kPreflightMs);
 CHECK(close_discovery(n,{epoch+3,true,false},1000,true,epoch+86400,closed));
 const Record spent=closed;CHECK(reserve_recovery_discovery(spent,{epoch+4,true,false},proof(),true,n)==Admission::BUDGET);
 CHECK(!memcmp(&spent,&closed,sizeof(spent)));
 // Real calendar versus explicit manual: no generic automatic day renewal.
 reset_recovery();CHECK(reserve_recovery_discovery(state_record,{epoch,true,false},proof(),false,n)==Admission::NOT_DUE);
 CHECK(reserve_recovery_discovery(state_record,{epoch,true,true},proof(),false,n)==Admission::ALLOWED);
 auto future=state_record;future.not_before=epoch+100;
 CHECK(reserve_recovery_discovery(future,{epoch,true,false},proof(),true,n)==Admission::NOT_DUE);
 // Admission cannot erase target-bearing, destructive, uncertain or other-owner debt.
 for(unsigned fault=0;fault<29;++fault){
  reset_recovery();const auto before=store.bytes;
  switch(fault){
   case 0: state_record.target.sha256[0]=1;break;
   case 1: state_record.deferred_path=true;break;
   case 2: state_record.fast_opportunities=1;break;
   case 3: state_record.attempt_ordinal=1;break;
   case 4: state_record.day_attempts=1;break;
   case 5: state_record.begins[0]=1;break;
   case 6: state_record.begins[1]=1;break;
   case 7: state_record.attempt_begins[0]=1;break;
   case 8: state_record.attempt_begins[1]=1;break;
   case 9: g_coord_credit.pending_resolved=true;break;
   case 10:g_coord_credit.pending_credit_admitted=false;g_coord_credit.admitted_epoch=0;break;
   case 11:strcpy(g_coord_pending,"other_pending");break;
   case 12:strcpy(g_coord_completion_target,"6.4.162");break;
   case 13:state_record.high_water=g_coord_credit.admitted_epoch+1;break;
   case 14:g_coord_credit_uncertain=true;break;
   case 15:g_ota_storage_uncertain=true;break;
   case 16:g_serial_install_uncertain=true;break;
   case 17:g_nvs_reclaim_uncertain=true;break;
   case 18:debt_read_ok=false;break;
   case 19:tz_ok=false;break;
   case 20:g_coord_credit_mutations=false;break;
   case 21:g_coord_credit_loaded=false;break;
   case 22:already_completed=true;break;
   case 23:user_busy=true;break;
   case 24:strcpy(g_lcd_query_coord_owner,"different");break;
   case 25:g_peer_gate.proof_ms=now_ms-2000;break;
   case 26:g_lcd_query_peer_boot_id=99;break;
   case 27:clock_fresh=false;break;
   case 28:local_valid=false;break;
  }
  CHECK(!enter("manual",true));CHECK(store.bytes==before&&legacy_commits==0);
 }
 // Failed write/readback may not publish an admission or permit a second try.
 for(unsigned fault=0;fault<2;++fault){reset_recovery();
  if(fault==0)fail_write=true;else fail_readback=true;
  CHECK(!enter("manual",true));CHECK(!work.live&&!state_allowed&&legacy_commits==0);
  const auto writes=std::count(events.begin(),events.end(),"policy_write");
  CHECK(!enter("manual",true));CHECK(std::count(events.begin(),events.end(),"policy_write")==writes);
 }
 // Reset after a committed grant retains its identity and charges unknown work.
 reset_recovery();CHECK(enter("manual",true));const auto granted=state_record;
 CHECK(reconcile_reset(granted,{epoch+1,true,false},epoch+86400,closed));
 CHECK(!strcmp(closed.origin,g_coord_pending)&&closed.network_windows==1);
 CHECK(closed.work_remaining_ms==granted.work_remaining_ms&&!active_phase(closed));
 CHECK(reserve_recovery_discovery(closed,{epoch+2,true,false},proof(),true,n)==Admission::ALLOWED);
 CHECK(n.network_windows==2);
 composition_cases();
 printf("PASS %u recovery admission/ownership/accounting/commit checks\n",checks);
}
'''


def run(root, out):
    out.mkdir(parents=True, exist_ok=False)
    src = out / 'test.cpp'
    src.write_text(harness(root))
    binary = out / 'test'
    command = [shutil.which('c++'), '-std=c++17', '-O1', '-Wall', '-Wextra',
               '-I' + str(root / 'halo_ota_demo/firmware/shared'), str(src), '-o', str(binary)]
    built = subprocess.run(command, capture_output=True, text=True, timeout=30)
    (out / 'compile.log').write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError('Compile failed: ' + str(out / 'compile.log'))
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    paths = ['halo_ota_demo/firmware/shared/DurableOtaDiscoveryRecovery.h',
             'halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h',
             'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino',
             'tools/test_postboot_policy_settlement.py',
             'tools/test_ota_discovery_recovery.py']
    receipt = {'passed': result.returncode == 0, 'source_root': str(root),
               'sources': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
               'hardware_actions': 0, 'network_actions': 0, 'runtime_nvs_writes': 0}
    (out / 'RESULT.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if result.returncode:
        raise RuntimeError('Test failed: ' + str(out / 'run.log'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.out:
        run(args.source_root, args.out)
    else:
        with tempfile.TemporaryDirectory(prefix='ota-recovery-host-') as directory:
            run(args.source_root, Path(directory) / 'run')
