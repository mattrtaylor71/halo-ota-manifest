"""Execute the production completed-comparison hint repair and checked NVS setter.

The existing postboot harness supplies SDK/flash boundaries and real durable
policy transitions/codecs. This suite additionally extracts the actual repair,
coordinator projection and typed debt read/write functions. Faults are injected
only at hardware/storage boundaries; no duplicate repair predicate is used.
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


def harness(root, runtime_override=None):
    postboot.ROOT = root
    postboot.SHARED = root / 'halo_ota_demo/firmware/shared'
    postboot.WRAPPER = root / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino'
    postboot.RUNTIME = runtime_override or postboot.SHARED / 'SenseDurablePolicyRuntime.h'
    postboot.STATE = postboot.SHARED / 'SenseDurablePolicyState.h'
    runtime, wrapper, state = (p.read_text() for p in
                               (postboot.RUNTIME, postboot.WRAPPER, postboot.STATE))
    source = postboot.harness().split('int main(){', 1)[0]
    source = source.replace('#include <cassert>', '''#include <cassert>
#include <cstdlib>
#include <functional>
#include <atomic>
#include "CoordinatorCreditState.h"''')
    source = source.replace('char owner[32]="owner";', 'char owner[32]="owner",challenge[40]="nonce";')
    source = source.replace('static bool halo_ota_manual_override_active(){return false;}',
                            'static bool manual_request=true;static bool halo_ota_manual_override_active(){return manual_request;}')
    source = source.replace('static bool halo_primary_user_work_busy(){return false;}',
                            'static bool user_busy=false;static bool halo_primary_user_work_busy(){return user_busy;}')
    source = source.replace('static bool get_lcd_ota_due_nvs(){return false;}',
                            'static bool get_lcd_ota_due_nvs();')
    source = source.replace('static bool normal_entry(){return false;}',
                            'static bool normal_window=false;static bool normal_entry(){return normal_window;}')
    source = source.replace('static bool unresolved_legacy(){return false;}',
                            definition(state, 'inline bool unresolved_legacy()'))
    source = source.replace('static void remember_retry_baseline(){}',
                            'static bool peer_valid(const char* expected=nullptr);\n' +
                            definition(runtime, 'static void remember_retry_baseline()'))
    source = source.replace('static bool peer_valid(const char* expected=nullptr) {',
                            'static bool peer_valid(const char* expected) {')
    source = source.replace('static bool reconcile_resolved_lcd_hint(bool& reconciled){reconciled=false;return false;}', '')
    source = source.replace('static bool resolved_lcd_hint_ready(){return false;}', '')
    source = source.replace('++hash_reads;assert(start==g_lcd_work_budget.started_ms',
                            '++hash_reads;inject("hash");assert(start==g_lcd_work_budget.started_ms')

    boundaries = r'''
static CoordinatorCreditState g_coord_credit;
static bool g_coord_credit_persisted=true,g_coord_credit_uncertain=false,g_ota_storage_uncertain=false;
static bool g_serial_install_uncertain=false,g_nvs_reclaim_uncertain=false,g_lcd_due_ram_obligation=false;
static bool g_peer_continue_work=false,g_self_retry_boot=false,g_self_retry_execution=false;
static bool g_ota_apply_in_progress=false,g_ota_check_in_progress=false;
static bool g_lcd_ota_proxy_owns_uart=false,g_lcd_ota_task_running=false;
static bool g_spool_owns_uart=false,g_img_spool_tx_active=false;
static bool retry_safe=true,s_lcd_mode_marker_pending=false,storage_uncertain=false,image_binary_pending=false;
static std::atomic<bool> g_img_spool_request_active{false};
static bool sense_lcd_ota_retry_safe(){return retry_safe;}
static bool sense_img_spool_binary_pending(){return image_binary_pending;}
static bool g_lcd_query_recovery_idle=true,g_lcd_query_coord_reported=true;
static char g_lcd_query_coord_id[40]="nonce",g_lcd_query_coord_owner[32]="owner";
static uint32_t g_lcd_query_coord_lease_ms=120000,g_lcd_ota_query_resp_part_size=2621440;
static std::vector<std::string> done_ids;
static bool ota_peer_schedule_completed(const char* id){
 return std::find(done_ids.begin(),done_ids.end(),id)!=done_ids.end();
}
static bool coord_credit_budget_open(){return g_lcd_work_budget_live&&g_lcd_work_budget.remaining_ms();}
static bool coord_credit_format_write_ready(){return !g_serial_install_uncertain&&local_valid;}
using nvs_handle_t=int;using esp_err_t=int;
static constexpr int ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;
static bool due_present=true,lease_ok=true,capacity_ok=true,namespace_absent=false;
static uint32_t due_scalar=1;
static bool nvs_open_read_fail=false,nvs_open_write_fail=false,nvs_read_fail=false;
static bool nvs_type_fail=false,nvs_erase_fail=false,nvs_commit_fail=false,nvs_readback_fail=false;
static bool erased_pending=false,nvs_committed=false;
static unsigned erases=0,commits=0,sets=0,debt_reads=0,opens=0,closes=0;
static std::function<void(const char*)> inject=[](const char*){};
static esp_err_t nvs_open(const char* ns,int mode,nvs_handle_t* h){
 assert(!strcmp(ns,"halo"));inject(mode==NVS_READONLY?"open_read":"open_write");
 if((mode==NVS_READONLY&&nvs_open_read_fail)||(mode==NVS_READWRITE&&nvs_open_write_fail))return 2;
 if(namespace_absent&&mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;
 *h=1;++opens;return ESP_OK;
}
static void nvs_close(nvs_handle_t){++closes;}
static esp_err_t nvs_get_u32(nvs_handle_t,const char* key,uint32_t* value){
 assert(!strcmp(key,"lcd_ota_due"));++debt_reads;inject(nvs_committed?"readback":"read");
 if(nvs_read_fail||nvs_type_fail||(nvs_committed&&nvs_readback_fail))return 2;
 if(!due_present)return ESP_ERR_NVS_NOT_FOUND;*value=due_scalar;return ESP_OK;
}
static esp_err_t nvs_set_u32(nvs_handle_t,const char*,uint32_t){++sets;return 2;}
static esp_err_t nvs_erase_key(nvs_handle_t,const char* key){
 assert(!strcmp(key,"lcd_ota_due"));++erases;inject("erase");
 if(nvs_erase_fail)return 2;erased_pending=true;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t){
 ++commits;inject("commit");if(nvs_commit_fail)return 2;
 if(erased_pending)due_present=false;nvs_committed=true;return ESP_OK;
}
struct NvsCapacityLease{NvsCapacityLease(){inject("lease");}explicit operator bool()const{return lease_ok;}};
static bool wakelog_nvs_prepare_essential(unsigned needed,bool(*ready)()){
 assert(needed==2);inject("capacity");return capacity_ok&&ready();
}
static bool set_lcd_ota_due_nvs(bool value,bool(*admission)()=nullptr);
'''
    boundaries += definition(wrapper, 'static CoordinatorCreditState coord_credit_base()') + '\n'
    for sig in ('static bool ota_storage_read_debt(', 'static bool get_lcd_ota_due_nvs()',
                'static bool ota_storage_mutation_open()', 'static bool set_lcd_ota_due_nvs('):
        boundaries += definition(wrapper, sig) + '\n'
    source = source.replace('namespace sense_policy {', boundaries + '\nnamespace sense_policy {', 1)
    entry = definition(runtime, 'static bool enter(')
    repair = '\n'.join(definition(runtime, sig) for sig in
                       ('static bool retry_transport_ready()', 'static bool resolved_lcd_hint_ready()',
                        'static bool reconcile_resolved_lcd_hint('))
    source = source.replace(entry, repair + '\n' + entry)
    return source + CASES


CASES = r'''
static unsigned checks=0,cases=0;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL case %u line %u: %s\n",cases,unsigned(__LINE__),#x);exit(1);}}while(0)
static void reset_hint(){
 reset();using namespace sense_policy;using namespace durable_ota;
 epoch=1790035200;now_ms=10000;manual_request=true;user_busy=normal_window=false;
 kFirmwareVersion="6.4.199";strcpy(g_lcd_ota_query_resp_fw,"6.4.199");
 g_coord_pending[0]=g_coord_completion_target[0]=0;g_coord_credit={};
 strcpy(g_coord_credit.schedule.id,"nightly_20260922");g_coord_credit.schedule.bound=true;
 strcpy(g_coord_credit.schedule.timezone,"PST8PDT,M3.2.0,M11.1.0");g_coord_credit.schedule.target_epoch=1790067600;
 g_coord_credit_loaded=g_coord_credit_mutations=g_coord_credit_persisted=true;
 g_coord_credit_uncertain=g_ota_storage_uncertain=g_serial_install_uncertain=g_nvs_reclaim_uncertain=false;
 g_lcd_due_ram_obligation=false;g_peer_continue_work=g_self_retry_boot=g_self_retry_execution=false;
 g_ota_apply_in_progress=g_ota_check_in_progress=g_lcd_ota_proxy_owns_uart=g_lcd_ota_task_running=false;
 g_spool_owns_uart=g_img_spool_tx_active=s_lcd_mode_marker_pending=storage_uncertain=image_binary_pending=false;
 g_img_spool_request_active=false;retry_safe=true;
 g_lcd_query_recovery_idle=g_lcd_query_coord_reported=true;
 strcpy(g_lcd_query_coord_id,"nonce");strcpy(g_lcd_query_coord_owner,"owner");
 g_lcd_query_coord_lease_ms=120000;g_lcd_ota_query_resp_part_size=2621440;
 g_peer_gate.entered=true;strcpy(g_peer_gate.challenge,"nonce");
 due_present=lease_ok=capacity_ok=true;due_scalar=1;namespace_absent=false;
 nvs_open_read_fail=nvs_open_write_fail=nvs_read_fail=nvs_type_fail=false;
 nvs_erase_fail=nvs_commit_fail=nvs_readback_fail=erased_pending=nvs_committed=false;
 erases=commits=sets=debt_reads=opens=closes=0;inject=[](const char*){};
 Target target{};strcpy(target.version,"6.4.198");strcpy(target.peer_version,"6.4.198");
 strcpy(target.url,"https://example.com/sense198.bin");target.bytes=1868224;target.peer_bytes=2030064;
 target.sha256[0]=1;target.peer_sha256[0]=2;uint8_t campaign[16]={43};Record r,n;
 CHECK(start(target,"nightly_20260920",campaign,{epoch-20,true,true},false,r));
 CHECK(reserve_preflight(r,{epoch-19,true,true},false,n)==Admission::ALLOWED);r=n;
 CHECK(reserve_apply(r,{epoch-18,true,true},1000,120000,true,n));r=n;
 CHECK(resolve(r,{epoch-17,true,true},target,true,true,true,n));r=n;
 r.generation=43;r.work_remaining_ms=17;r.network_windows=2;r.day_attempts=2;r.begins[0]=r.begins[1]=4;
 CHECK(shape(r));CHECK(credit_state_shape(g_coord_credit));
 state_record=r;state_loaded=state_present=state_allowed=true;
 uint8_t bytes[kRecordBytes];CHECK(encode(r,bytes));store.bytes.assign(bytes,bytes+sizeof(bytes));store.wrote=false;
 done_ids={"nightly_20260920"};events.clear();work={};boot_reconciled=false;
 work.original_start=g_lcd_work_budget.started_ms;work.original_budget=g_lcd_work_budget.limit_ms;
 remember_retry_baseline();CHECK(work.retry_baseline.boot==12);
}
struct Retained{
 std::vector<uint8_t> policy;std::vector<std::string> history;uint8_t credit[COORDINATOR_CREDIT_BYTES]{};
 Retained():policy(sense_policy::store.bytes),history(done_ids){CHECK(credit_encode(g_coord_credit,credit));}
 void unchanged(){uint8_t actual[COORDINATOR_CREDIT_BYTES]{};
  CHECK(sense_policy::store.bytes==policy);CHECK(credit_encode(g_coord_credit,actual));
  CHECK(!memcmp(actual,credit,sizeof(credit)));CHECK(done_ids==history);CHECK(!legacy_commits&&sets==0);
 }
};
// Exact OTA-only blobs from the byte-preserved service207 NVS backup. No
// provisioning data. Policy SHA0ef76d018a7faa7ae6b0b09e666df427a18e081283bded74058de42b4904bee9;
// credit SHA686e878184f4e80178a609b911eb8e2a780a0fea3051b81eb1f38429a76f62c8.
static std::vector<uint8_t> unhex(const char* hex){
 std::vector<uint8_t> bytes;for(size_t i=0;hex[i];i+=2){unsigned v=0;assert(sscanf(hex+i,"%2x",&v)==1);bytes.push_back(uint8_t(v));}return bytes;
}
static void reset_discovery(){
 reset_hint();using namespace sense_policy;using namespace durable_ota;
 const auto policy=unhex(
  "444f523101090000340000003102bbf5cd2bf1c95e0d941f01720d526d7174745f636d64000000000000000000000000"
  "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000362e342e"
  "3230310000000000000000000000000000000000000000000000000068747470733a2f2f68616c6f2d6f74612d70726f"
  "642e73332e75732d656173742d312e616d617a6f6e6177732e636f6d2f68616c6f2f6f74612f70726f642f6172746966"
  "616374732f73656e73655f362e342e3230315f333765303165623834366139333432652e62696e000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000037e01eb8"
  "46a9342e30cfb06c54c30fdc47c66c763c8f2e2969fa5442b048ae9780881c00362e342e323031000000000000000000"
  "000000000000000000000000000000005cbdeccecba68671116a567d0cd9b2a8b12c88656e1b9b994d46fa5d4c93e24c"
  "e0041f00a136a86a33f6b16aee5000005aeab16a0000000000000000000000009043b26a060000000000000000000000"
  "40ca22000000000000000000000000000000000000000000000100000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000099740962"
 );
 const auto credit=unhex(
  "4e434432020100006e696768746c795f3230323630393232000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "0000000000000000505354385044542c4d332e322e302c4d31312e312e30000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000009043b26a00000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
  "00000000000000000000000000000000000000000000000000000000000000000000000000000000"
 );
 CHECK(decode(policy.data(),policy.size(),state_record));
 CHECK(credit_decode(credit.data(),credit.size(),g_coord_credit));
 store.bytes=policy;store.wrote=false;
 CHECK(state_record.phase==Phase::DISCOVERY&&!active_phase(state_record)&&!state_record.deferred_path);
 CHECK(state_record.reserved_work_ms==0&&state_record.active_deadline==0&&!strcmp(state_record.target.version,"6.4.201"));
 CHECK(!g_coord_credit.pending.id[0]&&!g_coord_credit.deferred.id[0]&&!g_coord_credit.pending_credit_admitted);
 epoch=1790066829;kFirmwareVersion="6.4.207";strcpy(g_lcd_ota_query_resp_fw,"6.4.206");
 strcpy(g_lcd_query_running_part,"app0");strcpy(g_lcd_query_boot_part,"app0");
 remember_retry_baseline();
 done_ids={"nightly_20260920","nightly_20260919","nightly_20260918","nightly_20260917", "nightly_20260916","nightly_20260915","nightly_20260914","nightly_20260912"};
}
static void denied(const char* name,const std::function<void()>& change,bool discovery=false){
 ++cases;if(discovery)reset_discovery();else reset_hint();change();const auto policy=sense_policy::store.bytes;
 const CoordinatorCreditState credit=g_coord_credit;const auto history=done_ids;
 bool reconciled=true;CHECK(!sense_policy::reconcile_resolved_lcd_hint(reconciled));CHECK(!reconciled);
 CHECK(erases==0&&commits==0&&sets==0&&due_present);CHECK(sense_policy::store.bytes==policy);
 CHECK(!memcmp(&g_coord_credit,&credit,sizeof(credit))&&done_ids==history&&!legacy_commits);
 CHECK(opens==closes);(void)name;
}
int main(){
 using namespace sense_policy;using namespace durable_ota;
 ++cases;reset_hint();Retained retained;bool reconciled=false;
 CHECK(reconcile_resolved_lcd_hint(reconciled)&&reconciled);CHECK(!due_present&&erases==1&&commits==1);
 CHECK(!get_lcd_ota_due_nvs()&&!g_ota_storage_uncertain&&!g_lcd_due_ram_obligation);
 CHECK(!hash_reads);retained.unchanged();CHECK(opens==closes);
 ++cases;reconciled=true;CHECK(!reconcile_resolved_lcd_hint(reconciled)&&!reconciled);
 CHECK(erases==1&&commits==1);retained.unchanged();
 // Same-version proof must use the actual full-image matching adapter.
 ++cases;reset_hint();kFirmwareVersion="6.4.198";strcpy(g_lcd_ota_query_resp_fw,"6.4.198");
 remember_retry_baseline();reconciled=false;CHECK(reconcile_resolved_lcd_hint(reconciled)&&reconciled);
 CHECK(hash_reads==1&&erases==1);CHECK(opens==closes);
 denied("same-version wrong image",[]{kFirmwareVersion="6.4.198";flash_hash_matches=false;});
 denied("older local image",[]{kFirmwareVersion="6.4.197";});
 denied("older LCD image",[]{strcpy(g_lcd_ota_query_resp_fw,"6.4.197");remember_retry_baseline();});
 for(unsigned phase=unsigned(Phase::FAST);phase<=unsigned(Phase::BENCH_READY);++phase){
  if(phase==unsigned(Phase::RESOLVED))continue;
  denied("unresolved phase",[phase]{state_record.phase=Phase(phase);});
 }
 denied("malformed policy",[]{state_record.generation=0;});
 denied("invalid target",[]{state_record.target.bytes=0;});
 denied("absent policy",[]{state_present=false;});
 denied("unloaded policy",[]{state_loaded=false;});
 denied("uncertain policy",[]{state_allowed=false;});
 denied("bench policy",[]{state_record.bench.state=BenchState::ACTIVE;});
 denied("one-shot policy",[]{state_record.one_shot.phase=OneShotPhase::CLOSED;});
 denied("credit unloaded",[]{g_coord_credit_loaded=false;});
 denied("credit mutation barred",[]{g_coord_credit_mutations=false;});
 denied("credit not persisted",[]{g_coord_credit_persisted=false;});
 denied("credit malformed",[]{g_coord_credit.schedule.target_epoch=0;});
 denied("raw pending",[]{strcpy(g_coord_credit.pending.id,"nightly_20260920");});
 denied("raw deferred",[]{g_coord_credit.deferred=g_coord_credit.schedule;});
 denied("runtime pending",[]{strcpy(g_coord_pending,"nightly_20260922");});
 denied("completion target",[]{strcpy(g_coord_completion_target,"6.4.200");});
 denied("OTA storage uncertainty",[]{g_ota_storage_uncertain=true;});
 denied("credit uncertainty",[]{g_coord_credit_uncertain=true;});
 denied("serial install uncertainty",[]{g_serial_install_uncertain=true;});
 denied("reclaim uncertainty",[]{g_nvs_reclaim_uncertain=true;});
 denied("active policy invocation",[]{work.live=true;});
 denied("coordinator continuation",[]{g_peer_continue_work=true;});
 denied("self retry executing",[]{g_self_retry_execution=true;});
 denied("apply owner",[]{g_ota_apply_in_progress=true;});
 denied("check owner",[]{g_ota_check_in_progress=true;});
 denied("proxy UART owner",[]{g_lcd_ota_proxy_owns_uart=true;});
 denied("LCD OTA task",[]{g_lcd_ota_task_running=true;});
 denied("voice spool UART owner",[]{g_spool_owns_uart=true;});
 denied("image UART owner",[]{g_img_spool_tx_active=true;});
 denied("foreground work",[]{user_busy=true;});
 denied("local SDK invalid",[]{local_valid=false;});
 denied("clock not fresh",[]{clock_fresh=false;});
 denied("clock before policy high water",[]{epoch=state_record.high_water-1;});
 denied("budget inactive",[]{g_lcd_work_budget_live=false;});
 denied("budget exhausted",[]{now_ms+=g_lcd_work_budget.limit_ms;});
 denied("original budget absent",[]{work.original_budget=0;});
 denied("original budget changed",[]{++work.original_budget;});
 denied("original deadline expired",[]{work.original_start=now_ms-120000;});
 denied("peer gate inactive",[]{g_peer_gate.active=false;});
 denied("peer not entered",[]{g_peer_gate.entered=false;});
 denied("peer not ready",[]{g_peer_gate.ready=false;});
 denied("peer not locked",[]{g_peer_gate.locked=false;});
 denied("legacy peer",[]{g_peer_gate.legacy=true;});
 denied("owner absent",[]{g_peer_gate.owner[0]=0;});
 denied("nonce absent",[]{g_peer_gate.challenge[0]=0;});
 denied("peer proof stale",[]{g_peer_gate.proof_ms=now_ms-2000;});
 denied("peer deadline expired",[]{g_peer_gate.deadline_ms=now_ms;});
 denied("peer boot changed",[]{g_lcd_query_peer_boot_id=99;});
 denied("gate boot mismatch",[]{g_peer_gate.peer_boot=99;});
 denied("peer nonce mismatch",[]{strcpy(g_lcd_query_coord_id,"other");});
 denied("peer owner mismatch",[]{strcpy(g_lcd_query_coord_owner,"other");});
 denied("peer lease absent",[]{g_lcd_query_coord_lease_ms=0;});
 denied("peer lease excessive",[]{g_lcd_query_coord_lease_ms=120001;});
 denied("peer not boot-ready",[]{g_lcd_query_boot_ready=false;});
 denied("peer image invalid",[]{strcpy(g_lcd_query_running_state,"PENDING_VERIFY");});
 denied("peer partition missing",[]{g_lcd_query_running_part[0]=0;});
 denied("peer partition unknown",[]{strcpy(g_lcd_query_running_part,"?");strcpy(g_lcd_query_boot_part,"?");});
 denied("peer selected partition mismatch",[]{strcpy(g_lcd_query_boot_part,"app0");});
 denied("baseline boot missing",[]{work.retry_baseline.boot=0;});
 denied("baseline firmware drift",[]{strcpy(work.retry_baseline.fw,"6.4.200");});
 denied("baseline partition drift",[]{strcpy(work.retry_baseline.part,"app0");});
 denied("baseline size drift",[]{--work.retry_baseline.part_size;});
 denied("receiver not idle",[]{g_lcd_query_recovery_idle=false;});
 denied("coord report missing",[]{g_lcd_query_coord_reported=false;});
 denied("transport quarantine",[]{retry_safe=false;});
 denied("unsafe transfer marker",[]{s_lcd_mode_marker_pending=true;});
 denied("policy storage uncertainty",[]{storage_uncertain=true;});
 denied("pending image request",[]{g_img_spool_request_active=true;});
 denied("pending image binary",[]{image_binary_pending=true;});
 denied("typed debt read failure",[]{nvs_read_fail=true;});
 denied("typed debt type mismatch",[]{nvs_type_fail=true;});
 denied("typed debt invalid value",[]{due_scalar=2;});
 denied("read namespace open failure",[]{nvs_open_read_fail=true;});
 denied("capacity lease unavailable",[]{lease_ok=false;});
 denied("essential capacity unavailable",[]{capacity_ok=false;});
 denied("write namespace open failure",[]{nvs_open_write_fail=true;});
 // Recheck proof immediately after each potentially blocking prewrite boundary.
 for(const char* step:{"lease","capacity","open_write"}){
  denied("user arrived while awaiting storage",[step]{inject=[step](const char* event){if(!strcmp(step,event))user_busy=true;};});
  denied("proof expired awaiting storage",[step]{inject=[step](const char* event){if(!strcmp(step,event))now_ms+=2000;};});
 }
 // Equal-version hashing must also retain the original proof and deadline.
 denied("unsafe before equal-version hash",[]{kFirmwareVersion="6.4.198";g_peer_gate.proof_ms=now_ms-2000;});
 denied("proof expires during hash",[]{kFirmwareVersion="6.4.198";inject=[](const char* event){if(!strcmp(event,"hash"))now_ms+=2000;};});
 denied("foreground arrives during hash",[]{kFirmwareVersion="6.4.198";inject=[](const char* event){if(!strcmp(event,"hash"))user_busy=true;};});
 for(unsigned failure=0;failure<3;++failure){
  ++cases;reset_hint();Retained before;
  if(failure==0)nvs_erase_fail=true;else if(failure==1)nvs_commit_fail=true;else nvs_readback_fail=true;
  reconciled=true;CHECK(!reconcile_resolved_lcd_hint(reconciled)&&!reconciled);
  CHECK(erases==1&&g_ota_storage_uncertain&&g_lcd_due_ram_obligation);
  CHECK(get_lcd_ota_due_nvs());before.unchanged();const auto writes=erases;
  CHECK(!reconcile_resolved_lcd_hint(reconciled)&&erases==writes);CHECK(opens==closes);
 }
 // Committed retirement may outlive admission. Preserve that distinction and
 // never grant discovery if a user or stale proof appears during commit.
 for(unsigned late=0;late<2;++late){
  ++cases;reset_hint();Retained before;
  inject=[late](const char* event){if(!strcmp(event,"commit")){if(late)now_ms+=2000;else user_busy=true;}};
  reconciled=false;CHECK(!reconcile_resolved_lcd_hint(reconciled)&&reconciled);
  CHECK(!due_present&&erases==1&&commits==1);before.unchanged();CHECK(opens==closes);
 }
 // Actual enter controls whether ordinary discovery is possible. The caller
 // issues GET only after enter succeeds; these tests execute its whole source.
 ++cases;reset_hint();Retained manual_before;CHECK(enter("manual",true));
 CHECK(work.live&&!work.legacy&&state_record.phase==Phase::DISCOVERY);
 CHECK(erases==1&&commits==1&&!due_present&&state_record.network_windows==1);
 CHECK(state_record.generation==44&&state_record.reserved_work_ms==kPreflightMs);
 CHECK(!strcmp(state_record.origin,"manual"));CHECK(done_ids==manual_before.history&&!legacy_commits);
 uint8_t actual_credit[COORDINATOR_CREDIT_BYTES];CHECK(credit_encode(g_coord_credit,actual_credit));
 CHECK(!memcmp(actual_credit,manual_before.credit,sizeof(actual_credit)));
 // Application-only bootstrap may first run newer Sense with prior VALID LCD.
 // Each current image independently satisfies the completed target's floor.
 ++cases;reset_hint();kFirmwareVersion="6.4.201";CHECK(enter("manual",true));
 CHECK(work.live&&state_record.phase==Phase::DISCOVERY&&state_record.generation==44);
 CHECK(!strcmp(g_lcd_ota_query_resp_fw,"6.4.199")&&erases==1&&commits==1&&!due_present&&!hash_reads);
 for(const char* reason:{"lcd_due","policy_recovery","manual"}){
  ++cases;reset_hint();manual_request=false;Retained before;
  CHECK(!enter(reason,true));CHECK(!work.live&&erases==1&&!due_present);before.unchanged();
 }
 ++cases;reset_hint();inject=[](const char* event){if(!strcmp(event,"commit"))user_busy=true;};
 Retained late;CHECK(!enter("manual",true)&&!work.live);CHECK(erases==1&&!due_present);late.unchanged();
 // Both post-erase debt reads and enter's final recomputation may block. The
 // original proof must still be fresh after every such read, before admission.
 for(unsigned read_number=2;read_number<=4;++read_number){
  ++cases;reset_hint();Retained before;unsigned reads_after_commit=0;
  inject=[&](const char* event){if(!strcmp(event,"readback")&&++reads_after_commit==read_number)now_ms+=2000;};
  CHECK(!enter("manual",true)&&!work.live);CHECK(erases==1&&!due_present);before.unchanged();
 }
 // No new hint write when a later boot observes the committed absence.
 ++cases;reset_hint();due_present=false;reconciled=true;
 CHECK(!reconcile_resolved_lcd_hint(reconciled)&&!reconciled&&erases==0);
 ++cases;reset_hint();namespace_absent=true;reconciled=true;
 CHECK(!reconcile_resolved_lcd_hint(reconciled)&&!reconciled&&erases==0);
 // The actual207 refusal fixture: keep comparison/origin/accounting bytes
 // untouched while retiring the hint, then use the ordinary manual grant.
 ++cases;reset_discovery();const Record captured=state_record;Retained captured_before;
 Record expected{};CHECK(reserve_discovery(captured,fresh_clock(),false,false,expected,"manual",nullptr,true)==Admission::ALLOWED);
 CHECK(enter("manual",true));
 uint8_t expected_bytes[kRecordBytes];CHECK(encode(expected,expected_bytes));
 CHECK(store.bytes==std::vector<uint8_t>(expected_bytes,expected_bytes+sizeof(expected_bytes)));
 CHECK(!strcmp(state_record.origin,captured.origin)&&!memcmp(&state_record.target,&captured.target,sizeof(captured.target)));
 CHECK(work.live&&!work.legacy&&erases==1&&commits==1&&!due_present&&hash_reads==0);
 CHECK(done_ids==captured_before.history&&!legacy_commits);
 CHECK(credit_encode(g_coord_credit,actual_credit)&&!memcmp(actual_credit,captured_before.credit,sizeof(actual_credit)));
 ++cases;reset_discovery();Retained discovery_before;reconciled=false;
 CHECK(reconcile_resolved_lcd_hint(reconciled)&&reconciled);discovery_before.unchanged();
 CHECK(!due_present&&erases==1&&commits==1&&opens==closes);
 // Automatic calendar recovery owns its separate gate; no manual authority
 // may be inferred from a reason string, even during a normal time window.
 for(const char* reason:{"manual","lcd_due","nightly"}){
  ++cases;reset_discovery();manual_request=false;normal_window=true;Retained before;
  CHECK(!enter(reason,true)&&!work.live&&due_present&&erases==0);before.unchanged();
 }
 denied("closed discovery empty target",[]{state_record.target={};},true);
 denied("legacy deferred discovery",[]{state_record.deferred_path=true;},true);
 denied("active discovery",[]{Record next;CHECK(reserve_discovery(state_record,fresh_clock(),false,false,next,nullptr,nullptr,true)==Admission::ALLOWED);state_record=next;},true);
 denied("discovery one-shot",[]{state_record.one_shot.phase=OneShotPhase::CLOSED;},true);
 denied("discovery bench",[]{state_record.bench.state=BenchState::ACTIVE;},true);
 denied("discovery pending owner",[]{g_coord_credit.pending=g_coord_credit.schedule;},true);
 denied("discovery deferred owner",[]{g_coord_credit.deferred=g_coord_credit.schedule;},true);
 denied("discovery completion owner",[]{strcpy(g_coord_completion_target,"6.4.201");},true);
 denied("discovery stale peer",[]{g_peer_gate.proof_ms=now_ms-2000;},true);
 denied("discovery receiver not idle",[]{g_lcd_query_recovery_idle=false;},true);
 denied("discovery unsafe transport",[]{retry_safe=false;},true);
 denied("discovery invalid local",[]{local_valid=false;},true);
 denied("discovery below peer floor",[]{strcpy(g_lcd_ota_query_resp_fw,"6.4.200");remember_retry_baseline();},true);
 denied("discovery uncertain storage",[]{g_ota_storage_uncertain=true;},true);
 denied("discovery intent expires while storing",[]{inject=[](const char* e){if(!strcmp(e,"open_write"))manual_request=false;};},true);
 denied("discovery user arrives while storing",[]{inject=[](const char* e){if(!strcmp(e,"capacity"))user_busy=true;};},true);
 for(unsigned failure=0;failure<3;++failure){
  ++cases;reset_discovery();Retained before;
  if(failure==0)nvs_erase_fail=true;else if(failure==1)nvs_commit_fail=true;else nvs_readback_fail=true;
  CHECK(!enter("manual",true)&&!work.live&&g_ota_storage_uncertain&&get_lcd_ota_due_nvs());before.unchanged();
 }
 ++cases;reset_discovery();Retained expired;unsigned reads_after=0;
 inject=[&](const char* e){if(!strcmp(e,"readback")&&++reads_after==4)now_ms+=2000;};
 CHECK(!enter("manual",true)&&!work.live&&erases==1&&!due_present);expired.unchanged();
 printf("PASS %u cases / %u checks: production orphan repair, typed checked setter, refusals, idempotence, manual grant and automatic no-GET\n",cases,checks);
}
'''


def run(root, out):
    out.mkdir(parents=True, exist_ok=False)
    src, binary = out / 'test.cpp', out / 'test'
    src.write_text(harness(root))
    command = [shutil.which('c++'), '-std=c++17', '-O1', '-Wall', '-Wextra',
               '-I' + str(root / 'halo_ota_demo/firmware/shared'), str(src), '-o', str(binary)]
    built = subprocess.run(command, capture_output=True, text=True, timeout=45)
    (out / 'compile.log').write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError('Compile failed: ' + str(out / 'compile.log'))
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    paths = ['halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h',
             'halo_ota_demo/firmware/shared/SenseDurablePolicyState.h',
             'halo_ota_demo/firmware/shared/DurableOtaPolicy.h',
             'halo_ota_demo/firmware/shared/DurableOtaDiscovery.h',
             'halo_ota_demo/firmware/shared/CoordinatorCreditState.h',
             'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino',
             'tools/test_postboot_policy_settlement.py', 'tools/test_ota_resolved_hint.py']
    receipt = {'passed': result.returncode == 0, 'source_root': str(root),
               'sources': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
               'hardware_actions': 0, 'network_actions': 0, 'device_nvs_writes': 0}
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
        with tempfile.TemporaryDirectory(prefix='ota-resolved-hint-host-') as directory:
            run(args.source_root, Path(directory) / 'run')
