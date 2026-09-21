"""Execute actual postboot gate, enter, running-match and policy commit code.

The wrapper's early-completion block is composed with the real enter() adapter.
Clock, nonce replies, flash hash result and storage transport are modeled; the
canonical policy transitions, codec and exact readback-before-publication are
production code. No device, network, firmware build or persistent test output.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
import unittest
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]
SHARED = ROOT / 'halo_ota_demo/firmware/shared'
WRAPPER = ROOT / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino'
RUNTIME = SHARED / 'SenseDurablePolicyRuntime.h'
STATE = SHARED / 'SenseDurablePolicyState.h'


def early_block(source):
    start = source.index('  if (g_coord_pending[0] && g_coord_completion_target[0] &&',
                         source.index('  g_ota_pending_verify_active = g_health_gate.getPendingVerify()'))
    end = source.index('  nightly_maintenance_tick();', start)
    return source[start:end]


def harness(wrapper=None, baseline=False):
    runtime, state = RUNTIME.read_text(), STATE.read_text()
    wrapper = wrapper or WRAPPER.read_text()
    pre = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "DurableOtaDiscovery.h"
#include "DurableOtaOneShot.h"
#include "DurableOtaBench.h"
#define HALO_DURABLE_OTA_POLICY 1
struct OtaManifest {};
static uint32_t s_lcd_reboot_cleanup_boot=0;
static bool g_coord_credit_loaded=true,g_coord_credit_mutations=true;
static bool halo_primary_user_work_busy(){return false;}
static uint32_t now_ms=10000,epoch=1789372600;
static bool clock_fresh=true,local_valid=true,flash_hash_matches=true,legacy_write_ok=true;
static bool g_boot_ota_pending=true,g_ota_check_done=false,g_peer_episode_finished=false;
static bool g_lcd_work_budget_live=true,typed_absent=false,read_error=false;
static bool fail_write=false,fail_readback=false;
static unsigned entries=0,legacy_commits=0,hash_reads=0;
static std::vector<std::string> events;
static uint32_t millis(){return now_ms;}
static uint32_t esp_random(){return 123;}
static const char* kFirmwareVersion="6.4.145";
static char g_coord_pending[64]="nightly_20260915",g_coord_completion_target[32]="6.4.145";
static uint32_t g_coord_sense_boot_id=123,g_coord_sequence=0,g_boot_ota_deadline_ms=120000;
struct Budget{uint32_t started_ms=10000,limit_ms=120000;uint32_t remaining_ms()const{
 return uint32_t(now_ms-started_ms)<limit_ms?limit_ms-uint32_t(now_ms-started_ms):0;}}g_lcd_work_budget;
struct Peer{bool active=true,ready=true,entered=false,locked=true,legacy=false;
 uint32_t sequence=0,deadline_ms=120000,proof_ms=10000,peer_boot=12;char owner[32]="owner";}g_peer_gate;
static bool g_lcd_query_boot_ready=true;
static uint32_t g_lcd_query_peer_boot_id=12;
static char g_lcd_ota_query_resp_fw[32]="6.4.145",g_lcd_query_running_state[16]="VALID";
static char g_lcd_query_running_part[16]="app1",g_lcd_query_boot_part[16]="app1";
static int compareSemver(const char*a,const char*b){return strcmp(a,b);}
struct esp_partition_t{uint32_t address=0x1f0000;};
static esp_partition_t running;
using esp_ota_img_states_t=int;
static constexpr int ESP_OK=0,ESP_OTA_IMG_VALID=2;
static const esp_partition_t* esp_ota_get_running_partition(){return &running;}
static int esp_ota_get_state_partition(const esp_partition_t*,int*s){*s=local_valid?ESP_OTA_IMG_VALID:1;return ESP_OK;}
static bool nvs_capacity_image_valid(){return local_valid;}
static bool get_lcd_ota_due_nvs(){return false;}
static void uart_send_sense_diag_persist(const char*,const char*,const char*,int,const char*){}
static void ota_peer_cancel(const char*){events.push_back("completion_cancel");}
static void boot_ota_finish(const char*){g_boot_ota_pending=false;events.push_back("boot_finish");}
static bool halo_ota_manual_override_active(){return false;}
static struct{template<class...T>void printf(const char*,T...){}void println(const char*s){events.push_back(s);}}Serial;
enum class CoordCompletion{Deferred,NoPending,Credited,ResolvedUncredited};
static CoordCompletion ota_peer_schedule_complete(){
 if(!g_coord_pending[0])return CoordCompletion::NoPending;
 if(!legacy_write_ok)return CoordCompletion::Deferred;
 events.push_back("done_ids");++legacy_commits;g_coord_pending[0]=0;
 return CoordCompletion::Credited;
}
// Modeled nonce transport supplies no new reply. Keep the production service's
// 2s freshness rejection so stale proof cannot reach enter through ready().
static void ota_peer_service(){
 if(g_peer_gate.ready&&uint32_t(now_ms-g_peer_gate.proof_ms)<2000)return;
 g_peer_gate.ready=false;
}
''' + definition(wrapper, 'static bool ota_peer_ready()') + r'''
namespace sense_policy {
static durable_ota::Record state_record{};
static bool state_loaded=false,state_present=false,state_allowed=false;
static uint8_t state_scratch[durable_ota::kRecordBytes]{};
static unsigned state_status=0;
struct MemoryStore{
 std::vector<uint8_t> bytes;bool wrote=false;
 size_t write(const uint8_t*b,size_t n){
  events.push_back("policy_write");wrote=true;if(fail_write)return 0;bytes.assign(b,b+n);return n;
 }
 durable_ota::ReadResult read(uint8_t*b,size_t cap,size_t&n){
  if(read_error)return durable_ota::ReadResult::ERROR;
  if(typed_absent){n=0;return durable_ota::ReadResult::ABSENT;}
  n=bytes.size();if(n>cap)return durable_ota::ReadResult::ERROR;
  memcpy(b,bytes.data(),n);if(fail_readback&&wrote&&n)b[0]^=1;
  events.push_back(wrote?"policy_readback":"policy_compare_read");return durable_ota::ReadResult::PRESENT;
 }
}store;
''' + definition(state, 'inline const durable_ota::Record* current()') + '\n' + definition(state, 'inline bool absent()') + r'''
static bool load_state(uint32_t,uint32_t){
 if(state_loaded)return !state_present||state_allowed;
 auto result=durable_ota::load(store,state_record,state_allowed,state_scratch);
 if(result==durable_ota::ReadResult::ERROR)return false;
 state_loaded=true;state_present=result==durable_ota::ReadResult::PRESENT;
 return !state_present||state_allowed;
}
static bool commit_candidate(const durable_ota::Record&candidate,uint32_t start,uint32_t budget,bool first=false){
 assert(start==g_lcd_work_budget.started_ms&&budget==g_lcd_work_budget.limit_ms);
 if(uint32_t(now_ms-start)>=budget||!local_valid)return false;
 bool ok=durable_ota::commit(store,candidate,state_record,state_allowed,state_scratch,first);
 if(ok){state_present=true;events.push_back("policy_published");}return ok;
}
static bool normal_entry(){return false;}
static durable_ota::Clock fresh_clock(bool normal=false){return {epoch,clock_fresh,normal};}
static uint32_t next_normal_epoch(){return epoch+86400;}
static bool unresolved_legacy(){return false;}
// Empty-discovery recovery has its own actual-function regression; these cases
// exercise only the pre-existing exact-target postboot settlement contract.
static durable_ota::Admission reserve_proved_discovery_recovery(const durable_ota::Record&,
 durable_ota::Clock,bool,durable_ota::Record&){return durable_ota::Admission::LEGACY;}
// Orphan-hint retirement has its own actual-function/storage regression. These
// postboot cases retain pending completion debt and must not retire that hint.
static bool reconcile_resolved_lcd_hint(bool& reconciled){reconciled=false;return false;}
static bool resolved_lcd_hint_ready(){return false;}
static bool legacy_expectation(uint32_t,uint32_t,bool&pending){pending=false;return !read_error;}
static bool rollback_matches(const durable_ota::Record&,uint32_t,uint32_t){return false;}
static bool image_matches(const esp_partition_t*,const durable_ota::Target&,uint32_t start,uint32_t budget){
 ++hash_reads;assert(start==g_lcd_work_budget.started_ms&&budget==g_lcd_work_budget.limit_ms);
 return flash_hash_matches&&uint32_t(now_ms-start)<budget;
}
''' + runtime[runtime.index('struct RetryBaseline {'):runtime.index('// A refusal describes this invocation')] + r'''
static durable_ota::Admission last_admission=durable_ota::Admission::NOT_DUE;
static bool admission_allowed(durable_ota::Admission a){last_admission=a;return a==durable_ota::Admission::ALLOWED;}
static void remember_retry_baseline(){}
static void diagnostic_observed_pair(const durable_ota::Record&){events.push_back("observed_pair");}
static void diagnostic_terminal(const durable_ota::Record&){events.push_back("terminal");}
static uint32_t remaining(){return g_lcd_work_budget.remaining_ms();}
static void clamp_pair(){}
''' + definition(runtime, 'static bool peer_valid(') + '\n' + definition(runtime, 'static bool running_matches(') + '\n' + definition(runtime, 'static bool enter(') + r'''
}
// Model the existing caller boundaries only: a fresh readiness reply must
// precede actual enter(); false consumes this invocation, not durable credit.
static void nightly_maintenance_tick(){
 if(!g_boot_ota_pending||g_ota_check_done||!ota_peer_ready()||!clock_fresh)return;
 g_peer_gate.entered=true;++entries;
 bool admitted=sense_policy::enter("policy_recovery",true);
 assert(!admitted); // completion/refusals must not run another manifest GET
 g_ota_check_done=true;g_peer_episode_finished=true;events.push_back("transaction_close");
 if(g_boot_ota_pending)boot_ota_finish("check_started");
}
static void loop_tick(){
''' + early_block(wrapper) + r'''
 nightly_maintenance_tick();
}
static void reset(){
 using namespace durable_ota;using namespace sense_policy;
 now_ms=10000;epoch=1789372600;clock_fresh=local_valid=flash_hash_matches=legacy_write_ok=true;
 typed_absent=read_error=fail_write=fail_readback=false;
 g_boot_ota_pending=true;g_ota_check_done=g_peer_episode_finished=false;g_lcd_work_budget_live=true;
 g_lcd_work_budget={};g_peer_gate={};g_boot_ota_deadline_ms=120000;
 g_lcd_query_boot_ready=true;g_lcd_query_peer_boot_id=12;
 strcpy(g_lcd_ota_query_resp_fw,"6.4.145");strcpy(g_lcd_query_running_state,"VALID");
 strcpy(g_lcd_query_running_part,"app1");strcpy(g_lcd_query_boot_part,"app1");
 strcpy(g_coord_pending,"nightly_20260915");strcpy(g_coord_completion_target,"6.4.145");
 kFirmwareVersion="6.4.145";entries=legacy_commits=hash_reads=0;events.clear();work={};boot_reconciled=false;
 Target target{};strcpy(target.version,"6.4.145");strcpy(target.peer_version,"6.4.145");
 strcpy(target.url,"https://example.com/sense145.bin");target.bytes=1812080;target.peer_bytes=1891888;
 target.sha256[0]=1;target.peer_sha256[0]=2;uint8_t campaign[16]={1};Record r,n;
 assert(start(target,"nightly_20260915",campaign,{epoch-20,true,true},false,r));
 assert(reserve_preflight(r,{epoch-19,true,true},false,n)==Admission::ALLOWED);r=n;
 assert(reserve_apply(r,{epoch-18,true,true},1000,1498000,true,n));r=n;
 assert(reserve_begin(r,{epoch-17,true,true},0,n));r=n;
 assert(reserve_begin(r,{epoch-16,true,true},1,n));r=n;
 // Exhausted day counters cannot prevent observing the exact completed target.
 r.network_windows=2;r.day_attempts=2;r.begins[1]=4;r.work_remaining_ms=889352;
 assert(shape(r));uint8_t bytes[kRecordBytes];assert(encode(r,bytes));store.bytes.assign(bytes,bytes+sizeof(bytes));
 store.wrote=false;state_record=r;state_loaded=state_present=state_allowed=true;
}
static size_t at(const char*s){auto i=std::find(events.begin(),events.end(),s);assert(i!=events.end());return i-events.begin();}
'''
    if baseline:
        return pre + r'''
int main(){reset();loop_tick();
 assert(legacy_commits==1&&entries==0&&sense_policy::state_record.phase==durable_ota::Phase::APPLY);
 puts("REPRODUCED old loop: done_ids/cancel precede durable entry; APPLY stranded");}
'''
    return pre + r'''
int main(){
 using namespace sense_policy;using namespace durable_ota;
 reset();Record original=state_record;loop_tick();
 assert(state_record.phase==Phase::RESOLVED&&state_record.generation==original.generation+1);
 assert(state_record.work_remaining_ms==original.work_remaining_ms&&state_record.network_windows==2&&state_record.day_attempts==2&&state_record.begins[1]==4);
 assert(!state_record.reserved_work_ms&&legacy_commits==1&&entries==1&&hash_reads==2);
 assert(at("policy_write")<at("policy_readback")&&at("policy_readback")<at("policy_published")&&at("policy_published")<at("done_ids")&&at("done_ids")<at("transaction_close"));
 loop_tick();assert(legacy_commits==1&&entries==1);
 // Durable commit succeeds but separate legacy write fails. A later loop,
 // even after invocation cleanup, must complete it from committed RESOLVED.
 reset();legacy_write_ok=false;loop_tick();assert(state_record.phase==Phase::RESOLVED&&legacy_commits==0);
 legacy_write_ok=true;loop_tick();assert(legacy_commits==1&&entries==1&&at("policy_readback")<at("done_ids"));
 // Reset between durable resolution and legacy completion also preserves it.
 reset();legacy_write_ok=false;loop_tick();state_loaded=false;assert(load_state(now_ms,120000));
 boot_reconciled=false;g_boot_ota_pending=true;g_ota_check_done=g_peer_episode_finished=false;
 legacy_write_ok=true;loop_tick();assert(legacy_commits==1&&entries==1&&at("done_ids")<at("completion_cancel"));
 // Failed or uncertain write/readback never authorizes done_ids/cancel.
 for(unsigned failure=0;failure<2;++failure){reset();original=state_record;
  fail_write=failure==0;fail_readback=failure==1;loop_tick();loop_tick();
  assert(legacy_commits==0&&g_coord_pending[0]&&state_record.phase==Phase::APPLY);
  assert(!state_allowed&&std::find(events.begin(),events.end(),"completion_cancel")==events.end());}
 // Caller cannot pass stale/no peer proof or an expired original deadline.
 for(unsigned failure=0;failure<3;++failure){reset();
  if(failure==0)g_peer_gate.proof_ms=now_ms-2000;
  if(failure==1)g_peer_gate.ready=false;
  if(failure==2)g_peer_gate.deadline_ms=now_ms;
  loop_tick();assert(!entries&&!legacy_commits&&g_coord_pending[0]&&state_record.phase==Phase::APPLY);}
 // Neither non-VALID/mismatched peer nor local version/hash/selector evidence
 // can resolve the target. Any native DEFERRED accounting is kept as debt.
 for(unsigned failure=0;failure<7;++failure){reset();
  if(failure==0)strcpy(g_lcd_query_running_state,"PENDING_VERIFY");
  if(failure==1)strcpy(g_lcd_query_boot_part,"app0");
  if(failure==2)strcpy(g_lcd_ota_query_resp_fw,"6.4.144");
  if(failure==3)kFirmwareVersion="6.4.147";
  if(failure==4)flash_hash_matches=false;
  if(failure==5)local_valid=false;
  if(failure==6)clock_fresh=false;
  loop_tick();loop_tick();assert(!legacy_commits&&g_coord_pending[0]&&state_record.phase!=Phase::RESOLVED);}
 // Only typed absence is compatibility authority. Unknown/corrupt never is.
 reset();state_loaded=true;state_present=false;state_allowed=false;typed_absent=true;
 loop_tick();assert(legacy_commits==1&&entries==0);
 reset();state_loaded=false;state_present=false;read_error=true;
 loop_tick();assert(!legacy_commits&&g_coord_pending[0]);
 reset();state_loaded=true;state_present=true;state_allowed=false;
 loop_tick();assert(!legacy_commits&&g_coord_pending[0]);
 reset();state_loaded=false;store.bytes[0]^=1;
 loop_tick();assert(!legacy_commits&&g_coord_pending[0]&&!state_loaded);
 // A different committed campaign/target cannot satisfy this pending origin.
 reset();Record resolved;assert(resolve(state_record,{epoch,true,false},state_record.target,true,true,true,resolved));
 state_record=resolved;strcpy(state_record.origin,"other");assert(!postboot_completion_ready());
 strcpy(state_record.origin,g_coord_pending);strcpy(state_record.target.version,"6.4.144");assert(!postboot_completion_ready());
 puts("PASS actual ordered settlement: canonical readback before legacy completion; deferred legacy retry; restart; failed writes; proof/clock/deadline rejection; typed absence; no refill or extra GET");
}
'''



def reachability_harness():
    wrapper, runtime, state = WRAPPER.read_text(), RUNTIME.read_text(), STATE.read_text()
    text=harness().split('int main(){')[0]
    # Keep the actual early block/enter composition, replace only its modeled
    # nightly caller with the full production readiness/coordinator functions.
    text=text.replace('static void nightly_maintenance_tick(){','static void run_enter_model(){')
    text=text.replace('static void loop_tick(){','static void nightly_maintenance_tick();\nstatic void loop_tick(){')
    extra=r'''
#include <mutex>
#include "CoordinatorCreditState.h"
static const char*g_boot_ota_reason="coord_recovery";
static bool g_ota_pending_verify_active=false,g_boot_ota_begin_reported=true;
static bool g_boot_ota_diag_context=false,g_boot_ota_time_sync_started=true;
static bool foreground_active=false,voice_recording_active=false,g_list_screen_active=false;
static bool g_ota_check_in_progress=false,g_ota_apply_in_progress=false;
static bool g_lcd_ota_task_running=false,g_lcd_ota_proxy_owns_uart=false,g_ota_check_requested=false;
static bool g_coord_credit_uncertain=false,g_nvs_reclaim_uncertain=false;
static bool g_serial_install_uncertain=false,g_ota_storage_uncertain=false,g_peer_continue_work=false;
static uint32_t g_boot_ota_next_try_ms=0,g_lcd_timer_seen_boot=12;
static void*op_queue=nullptr;
static unsigned uxQueueMessagesWaiting(void*){return 0;}
static bool sense_action_inflight(){return false;}
static bool sense_lcd_ota_retry_safe(){return true;}
struct Provisioner{bool isSetupModeActive(){return false;}}g_provisioning_manager;
struct ProvisioningState{static bool isProvisioned(){return true;}static int getState(){return 1;}
 static constexpr int STATE_CONNECTED=1;};
static bool wifiReadyForHttps(){return true;}
static bool is_time_valid(){return clock_fresh;}
static bool sense_ntp_attempt_pending(){return false;}
static void halo_prod_kick_time_sync(const char*){}
static uint32_t ota_scheduled_clock_retry_deadline(){return 0;}
static bool ota_clock_ready_before_work(){return clock_fresh;}
static void set_lcd_ota_due_nvs(bool){}
static void uart_send_sense_diag(const char*,const char*,const char*,int,const char*){}
struct String:std::string{size_t length()const{return size();}};
struct Preferences{bool begin(const char*,bool){return false;}String getString(const char*,const char*){return {};}
 void end(){}};
static struct{const char*event="begin";const char*reason="coord_recovery";int code=0;char detail[1]={};}g_boot_ota_begin_record;
static struct{char schedule[64]="nightly_20260915";int wake=2,reset=3;uint32_t epoch=1789372600,boot_id=12;bool resumed=false;}g_lcd_timer_origin;
static struct{bool pending=false;}g_lcd_timer_notice;
static NightlyCreditOrigin g_calendar_timer_origin;
static CoordinatorCreditState g_coord_credit;
static std::recursive_mutex g_time_mutex;
static CoordinatorCreditState coord_credit_base(){return g_coord_credit;}
static bool ota_peer_schedule_completed(const char*){return false;}
static bool coord_credit_reserve_timer(){return true;}
static bool coord_credit_retire_configured_timezone(){return true;}
static void ensure_timezone_pt(const char*){}
static void coord_credit_clock(uint64_t&now,bool&fresh,char*tz,bool&finished){now=epoch;fresh=clock_fresh;strcpy(tz,"UTC0");finished=true;}
static bool coord_credit_save(const CoordinatorCreditState&r){g_coord_credit=r;return true;}
static bool halo_policy_short_due(){return false;}
static bool s_desired_force=false,s_attempt_recorded=false;
static uint32_t s_last_attempt_ms=0;
''' + definition((SHARED/'OtaIntent.cpp').read_text(),'static bool cooldown_allows()') + r'''
struct OtaIntent{static bool cooldownAllows(){return cooldown_allows();}};
static bool ota_peer_continuation(){return false;}
static bool coord_credit_notice_future_without_repair(const char*, uint64_t*);
'''
    # Execute the actual future-notice function: a live pending ID must reject
    # cancellation, including a different stored future calendar arm.
    future=definition(wrapper,'static bool coord_credit_notice_future_without_repair(')
    # Its timezone read section is unreachable with pending repair, but still
    # compile the full function against explicit timezone boundary functions.
    extra += r'''
static bool coord_credit_timezone_matches_configuration(char*out){strcpy(out,"UTC0");return true;}
static bool g_tz_initialized=true;static const char*g_tz_current="UTC0";
static bool sense_time_has_fresh_sync(){return clock_fresh;}
'''
    # Only the first decisive source guard is needed; later calendar/TZ logic
    # has no authority while g_coord_pending is present. Preserve guard bytes.
    cut=future.index('  {\n    std::lock_guard<std::recursive_mutex> config_lock')
    extra+=future[:cut]+'  return true; // modeled future calendar only after the real debt guard\n}\n'
    extra+='\n'.join(definition(wrapper,sig) for sig in (
        'static bool coord_credit_future_notice_wait(',
        'static bool coord_credit_cancel_future_notice()',
        'static bool coord_credit_calendar_entry(',
        'static bool coord_credit_prepare_work(',
    ))
    extra+=r'''
namespace sense_policy {
static uint32_t normal_calendar_due(){return 0;}
'''+ '\n'.join(definition(state,sig) for sig in (
        'inline bool bench_retry_phase(', 'inline bool bench_deferred_request(',
    ))+r'''
}
static void halo_policy_note_readiness(const char*,durable_ota::Clock,uint32_t=0,bool=false){}
static bool manual_ota_joined_readiness_active(){return false;}
static bool halo_policy_accepted_lcd_origin(const char*){return false;}
'''+definition(runtime,'static bool halo_policy_boot_ready()')+'\n'+definition(wrapper,'static bool self_retry_boot_admit()')+r'''
static void maybeRunOtaCheck(const char*reason,bool){
 assert(coord_credit_prepare_work(reason));run_enter_model();
}
'''+definition(wrapper,'static void nightly_maintenance_tick()')+r'''
int main(){
 using namespace sense_policy;using namespace durable_ota;
 for(const char* reason:{"coord_recovery","policy_recovery","nightly","lcd_timer"}){
  reset();g_boot_ota_reason=reason;g_boot_ota_next_try_ms=0;g_boot_ota_begin_reported=true;
  g_coord_credit={};strcpy(g_coord_credit.pending.id,g_coord_pending);
  // A pending admitted campaign is independent of the next future calendar.
  strcpy(g_coord_credit.schedule.id,"nightly_20260916");g_coord_credit.schedule.bound=true;
  g_coord_credit.schedule.target_epoch=epoch+86400;g_coord_credit.schedule_observed=false;
  assert(!coord_credit_cancel_future_notice());assert(coord_credit_calendar_entry(reason));
  assert(halo_policy_boot_ready());assert(coord_credit_prepare_work(reason));
  loop_tick();assert(entries==1&&legacy_commits==1&&state_record.phase==Phase::RESOLVED);
  assert(at("policy_readback")<at("done_ids"));
 }
 // Existing guards remain real: pending-verify, consumed request and deadline
 // cannot enter; no early legacy completion is allowed while APPLY remains.
 for(unsigned guard=0;guard<3;++guard){
  reset();g_boot_ota_reason="coord_recovery";g_boot_ota_next_try_ms=0;
  g_ota_pending_verify_active=guard==0;g_ota_check_done=guard==1;
  if(guard==2)g_boot_ota_deadline_ms=now_ms;
  loop_tick();assert(!entries&&!legacy_commits&&state_record.phase==Phase::APPLY);
 }
 puts("PASS actual nightly/clock readiness and coordinator pending-origin entry reaches durable settlement before completion");
}
'''
    return text+extra


class PostbootSettlementTests(unittest.TestCase):
    def compile_run(self, text):
        with tempfile.TemporaryDirectory(prefix='halo-postboot-settlement-') as folder:
            cpp, binary = Path(folder)/'check.cpp', Path(folder)/'check'
            cpp.write_text(text)
            subprocess.run([shutil.which('c++'), '-std=c++17', '-I', str(SHARED), str(cpp), '-o', str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_actual_composed_loop_and_settlement(self):
        self.compile_run(harness())

    def test_actual_pending_recovery_reaches_enter(self):
        self.compile_run(reachability_harness())

    def test_feature_off_legacy_completion(self):
        text=harness(baseline=True).replace("#define HALO_DURABLE_OTA_POLICY 1", "#define HALO_DURABLE_OTA_POLICY 0")
        text=text.replace("REPRODUCED old loop: done_ids/cancel precede durable entry; APPLY stranded", "PASS feature-off legacy completion remains compatible")
        self.compile_run(text)

    def test_real_caller_and_proof_boundaries(self):
        text=WRAPPER.read_text()
        call=text.index('  if(!sense_policy::enter(reason,retained_legacy))')
        preceding=text[text.rfind('  if (!ota_clock_ready_before_work())',0,call):call]
        self.assertIn('if (!ota_peer_ready()) return;',preceding)
        self.assertIn('g_peer_gate.entered = true;',preceding)
        self.assertIn('OtaPeerTransactionCleanup peer_cleanup;',preceding)
        runtime=RUNTIME.read_text()
        match=definition(runtime,'static bool running_matches(')
        self.assertIn('!strcmp(t.version,kFirmwareVersion)',match)
        self.assertEqual(match.count('nvs_capacity_image_valid()'),2)
        self.assertIn('image_matches(esp_ota_get_running_partition(),t,started,budget)',match)
        image=definition(runtime,'static bool image_matches(')
        self.assertIn('!memcmp(digest,t.sha256,32)',image)
        self.assertIn('esp_partition_read(part,offset,state_scratch,n)',image)
        self.assertIn('uint32_t(millis()-started)>=budget',image)


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--baseline-ref');args,rest=parser.parse_known_args()
    if args.baseline_ref:
        rel=WRAPPER.relative_to(ROOT).as_posix()
        source=subprocess.check_output(['git','show',args.baseline_ref+':Arduino/HALOMAIN_rev1p5_modular/'+rel],cwd=ROOT,text=True)
        PostbootSettlementTests().compile_run(harness(source,baseline=True))
    else:
        unittest.main(argv=[__file__]+rest)
