"""Actual Sense request/service + LCD lock/touch handoff, with UART/NVS doubles.

No device, network or firmware build. Reuses the durable admission harness;
ArduinoJson is the local firmware library. A frozen source is a negative control.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import tempfile

import test_manual_ota_readiness_join as joined
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino'


def harness(root, negative=False):
    wrapper = (root / WRAPPER).read_text()
    lcd = (root / 'LCD_Minimal/LCD_Minimal.ino').read_text()
    rx = (root / 'LCD_Minimal/lcd_uart_rx.h').read_text()
    source = joined.harness(root).split('int main(){', 1)[0]
    source = '#include <atomic>\n#include <ArduinoJson.h>\nusing String=std::string;\n' + source
    source = source.replace('proof_ms=10000,peer_boot=12;', 'proof_ms=10000,peer_boot=12,next_query_ms=0;')
    source = source.replace(definition(source, 'static void ota_peer_service()'), 'static void ota_peer_service();')
    source = source.replace(definition(source, 'static void ota_peer_send_lock('), 'static void ota_peer_send_lock(bool);')
    # Production LCD functions, with only task primitives and unrelated UI
    # presentation doubled. Dispatch the full actual OTA_LOCK branch.
    source += r'''
#define portENTER_CRITICAL(...) do{}while(0)
#define portEXIT_CRITICAL(...) do{}while(0)
namespace lcd {
static std::atomic<uint32_t> g_lcd_coord_lease_until_ms{0},g_lcd_timer_receiver_wait_until_ms{0};
static std::atomic<bool> g_lcd_sleep_commit_gate{false},g_lcd_coord_notice_clear{false};
static uint32_t g_lcd_coord_boot_id=12,g_lcd_coord_sense_boot_id=0,g_lcd_coord_sequence=0;
static char g_lcd_coord_owner[40]{};
static bool g_lcd_ota_uart_receiving=false,ota_locked=false,ota_check_pending=false,ota_check_requested=false;
static bool g_ota_screen_active=false,g_lcd_ota_recovery_grace=false,g_lcd_maintenance_active=false;
static uint32_t ota_stay_awake_until_ms=0,g_ota_lock_window_until_ms=0,ota_lock_at_ms=0;
static uint32_t g_lcd_maintenance_boot_grace_until_ms=0,g_lcd_maintenance_deadline_ms=0;
static constexpr uint32_t OTA_LOCK_TIMEOUT_MS=120000,LCD_OTA_LOCK_STAY_AWAKE_MS=120000;
struct LcdMaintenanceStorageGuard{};struct LcdCoordCriticalGuard{};
static void lcd_media_retry_wait_release(const char*){}
static void lcd_allow_visible_ui(const char*){}
'''
    for sig in ('static const char* halo_lcd_coord_owner()', 'static uint32_t halo_lcd_coord_lease_ms()',
                'static void lcd_coord_cancel_preflight(bool only_if_expired)',
                'static void lcd_timer_receiver_wait_release(const char* reason)'):
        source += definition(lcd, sig) + '\n'
    lock_branch = definition(rx, 'if (strcmp(type, "OTA_LOCK") == 0)')
    source += 'static void receive(const char* json){JsonDocument doc;CHECK(!deserializeJson(doc,json));const char* type=doc["type"]|"";\n' + lock_branch + '\n}\n}\n'
    source += r'''
static std::vector<std::string> sent;
static uint32_t get_next_msg_id(){return (uint32_t)sent.size()+1;}
static constexpr int PROTOCOL_VERSION=1;
static void uart_send_json(const char*s){sent.emplace_back(s);JsonDocument d;CHECK(!deserializeJson(d,s));
 if(!strcmp(d["type"]|"","OTA_UNLOCK")){wire_result=d["result"]|"";events.push_back("terminal_unlock");lcd::lcd_coord_cancel_preflight(false);}
 else lcd::receive(s);
}
'''
    source += definition(wrapper, 'static void ota_peer_send_lock(bool release)') + '\n'
    query = (root / 'Sense_Minimal/sense_ota_lcd.h').read_text()
    source += definition(query, 'struct LcdOtaQuerySnapshot') + ';\n'
    source += r'''
enum LcdOtaQueryPoll{LCD_QUERY_WAITING,LCD_QUERY_READY,LCD_QUERY_TIMEOUT};
static LcdOtaQuerySnapshot reply{};static LcdOtaQueryPoll poll_result=LCD_QUERY_WAITING;
static unsigned queries=0;static uint32_t query_budget=0;
static bool sense_lcd_ota_query_start(const char*,uint32_t budget){++queries;query_budget=budget;return true;}
static LcdOtaQueryPoll sense_lcd_ota_query_poll(LcdOtaQuerySnapshot& s){s=reply;const auto r=poll_result;poll_result=LCD_QUERY_WAITING;
 if(r==LCD_QUERY_READY){strcpy(g_lcd_query_coord_id,g_peer_gate.challenge);strcpy(g_lcd_query_coord_owner,s.coord_owner);
  g_lcd_query_coord_lease_ms=s.coord_lease_ms;g_lcd_query_peer_boot_id=s.peer_boot_id;}
 return r;}
static bool action_busy=false,g_list_screen_active=false,setup_active=false;
static bool sense_action_inflight(){return action_busy;}
static struct{bool isSetupModeActive(){return setup_active;}}g_provisioning_manager;
'''
    service = definition(wrapper, 'static void ota_peer_service() {')
    # The preceding notice/NVS housekeeping is a separate boundary. Execute
    # the entire actual readiness tail, including all rejection/timeout paths.
    tail = service[service.index('  if (!g_peer_gate.active || g_peer_gate.entered) return;'):]
    source += 'static void ota_peer_service(){\n' + tail + '\n'
    source += r'''
static void receive_reply(bool waiting=false){
 reply={};reply.correlated=true;reply.boot_ready=true;reply.peer_boot_id=lcd::g_lcd_coord_boot_id;
 reply.coord_waiting=waiting;reply.recovery_idle=true;reply.coord_lease_ms=lcd::halo_lcd_coord_lease_ms();
 strcpy(reply.coord_owner,lcd::halo_lcd_coord_owner());poll_result=LCD_QUERY_READY;g_peer_gate.querying=true;
 ota_peer_service();
}
static void reset_touch(bool touch=true){
 reset_join();now_ms=1488;g_boot_ota_deadline_ms=g_peer_gate.deadline_ms=120935;
 g_peer_gate.active=g_peer_gate.locked=g_peer_gate.ready=true;g_peer_gate.querying=false;
 g_peer_gate.proof_ms=0;g_peer_gate.next_query_ms=0;g_peer_gate.peer_boot=12;
 g_peer_gate.sequence=g_coord_sequence=1;g_coord_sense_boot_id=123;strcpy(g_peer_gate.owner,"65c29efc03f4bcb0");
 lcd::g_lcd_coord_boot_id=12;lcd::g_lcd_coord_sense_boot_id=0;lcd::g_lcd_coord_sequence=0;
 lcd::g_lcd_coord_lease_until_ms=0;lcd::g_lcd_timer_receiver_wait_until_ms=120935;
 lcd::g_lcd_sleep_commit_gate=false;lcd::g_lcd_coord_notice_clear=false;lcd::g_lcd_coord_owner[0]=0;
 lcd::g_lcd_ota_uart_receiving=false;lcd::ota_locked=false;
 g_boot_ota_reason="coord_recovery";g_lcd_timer_seen_boot=12;action_busy=setup_active=g_list_screen_active=false;
 sent.clear();queries=0;poll_result=LCD_QUERY_WAITING;
 ota_peer_send_lock(false);CHECK(lcd::ota_locked&&lcd::g_lcd_coord_sequence==1);
 if(touch){now_ms=1703;lcd::lcd_timer_receiver_wait_release("user_input");
  CHECK(!lcd::ota_locked&&lcd::halo_lcd_coord_lease_ms()==0&&lcd::g_lcd_coord_sequence==1);}
 now_ms=3749;
}
struct Preserved{
 std::vector<uint8_t> bytes=sense_policy::store.bytes;
 durable_ota::Record policy=sense_policy::state_record;
 CoordinatorCreditState credit=g_coord_credit;
 Budget budget=g_lcd_work_budget;
 std::string pending=g_coord_pending,completion=g_coord_completion_target,owner=g_peer_gate.owner;
 uint32_t boot=g_boot_ota_deadline_ms,peer=g_peer_gate.deadline_ms,peer_boot=g_peer_gate.peer_boot;
 bool debt=debt_value;
 void check()const{
  CHECK(bytes==sense_policy::store.bytes&&!memcmp(&policy,&sense_policy::state_record,sizeof(policy)));
  CHECK(!memcmp(&credit,&g_coord_credit,sizeof(credit))&&!memcmp(&budget,&g_lcd_work_budget,sizeof(budget)));
  CHECK(pending==g_coord_pending&&completion==g_coord_completion_target&&debt==debt_value);
  CHECK(boot==g_boot_ota_deadline_ms&&peer==g_peer_gate.deadline_ms&&peer_boot==g_peer_gate.peer_boot&&owner==g_peer_gate.owner);
 }
};
'''
    if negative:
        return source + r'''
int main(){reset_touch();Preserved before;halo_prod_request_manual_ota("manual");
 CHECK(g_manual_ota_joined_readiness&&g_peer_gate.locked&&g_peer_gate.sequence==1);
 for(unsigned i=0;i<250;++i){now_ms+=310;receive_reply();CHECK(!g_peer_gate.ready&&!lcd::ota_locked);}
 CHECK(sent.size()==1&&queries==0);before.check();
 now_ms=g_peer_gate.deadline_ms;ota_peer_service();
 CHECK(wire_result=="peer_unavailable"&&!g_peer_gate.active&&!g_manual_ota_override);
 printf("REPRODUCED183 %u checks: touch cancels lease, manual retains stale lock, 250 valid empty-owner replies cannot progress, original deadline fails\n",checks);
}
'''
    return source + r'''
int main(){
 // Captured 1488 lock -> 1703 navigation touch -> 3749 explicit manual.
 reset_touch();Preserved before;g_peer_gate.querying=true;
 halo_prod_request_manual_ota("manual");
 CHECK(g_manual_ota_joined_readiness&&g_peer_gate.querying&&!g_peer_gate.ready&&!g_peer_gate.locked&&g_peer_gate.sequence==2);
 CHECK(g_manual_ota_override_until_ms==before.peer);before.check();
 receive_reply();CHECK(lcd::ota_locked&&lcd::g_lcd_coord_sequence==2&&!g_peer_gate.ready&&g_peer_gate.locked);
 CHECK(lcd::g_lcd_coord_lease_until_ms==before.peer&&sent.size()==2);before.check();
 // Reusing the old empty-owner snapshot is not proof of the replacement lock.
 poll_result=LCD_QUERY_READY;g_peer_gate.querying=true;now_ms+=310;ota_peer_service();CHECK(!g_peer_gate.ready);
 now_ms+=310;receive_reply();CHECK(g_peer_gate.ready&&g_peer_gate.proof_ms==now_ms);before.check();
 const auto sequence=g_coord_sequence;const auto lease=lcd::g_lcd_coord_lease_until_ms.load();
 for(unsigned i=0;i<10;++i){++now_ms;halo_prod_request_manual_ota("manual");}
 CHECK(g_coord_sequence==sequence&&sent.size()==2&&OtaIntent::updates==1&&lcd::g_lcd_coord_lease_until_ms==lease);before.check();
 // Actual production request->boot-ready->coordinator->enter charges once.
 CHECK(halo_policy_boot_ready());manual_pipeline();CHECK(entered_calls==1&&sense_policy::work.live);
 CHECK(sense_policy::state_record.network_windows==1&&sense_policy::state_record.day_attempts==0);
 const auto admitted=sense_policy::store.bytes;halo_prod_request_manual_ota("manual");
 CHECK(admitted==sense_policy::store.bytes&&entered_calls==1&&g_coord_sequence==sequence);
 // A live original same-owner lease cannot be extended by a new lock.
 reset_touch(false);const auto old_until=lcd::g_lcd_coord_lease_until_ms.load();
 CHECK(old_until==g_peer_gate.deadline_ms);halo_prod_request_manual_ota("manual");receive_reply();
 CHECK(lcd::g_lcd_coord_sequence==1&&lcd::g_lcd_coord_lease_until_ms==old_until&&!g_peer_gate.ready);
 receive_reply();CHECK(g_peer_gate.ready&&lcd::g_lcd_coord_lease_until_ms==old_until);
 // A foreign live lease is neither replaced nor admitted.
 reset_touch(false);strcpy(lcd::g_lcd_coord_owner,"foreign");
 halo_prod_request_manual_ota("manual");receive_reply();receive_reply();
 CHECK(!g_peer_gate.ready&&!strcmp(lcd::g_lcd_coord_owner,"foreign"));
 // Automatic timer origin still requires its notice; explicit manual can
 // reacquire after touch, but still requires the original correlated boot.
 reset_touch();g_boot_ota_reason="lcd_timer";g_peer_gate.ready=g_peer_gate.locked=false;
 receive_reply();CHECK(!lcd::ota_locked&&!g_peer_gate.ready&&sent.size()==1);
 halo_prod_request_manual_ota("manual");receive_reply();receive_reply();CHECK(g_peer_gate.ready);
 for(unsigned wrong=0;wrong<2;++wrong){reset_touch();g_boot_ota_reason="lcd_timer";halo_prod_request_manual_ota("manual");
  reply={};reply.correlated=wrong!=0;reply.peer_boot_id=wrong?13:12;reply.coord_waiting=false;
  poll_result=LCD_QUERY_READY;g_peer_gate.querying=true;ota_peer_service();CHECK(sent.size()==1&&!g_peer_gate.ready&&!lcd::ota_locked);
 }
 // Reboot invalidates old proof and changes owner/sequence before another
 // lock and another reply; it never extends the readiness deadline.
 reset_touch();halo_prod_request_manual_ota("manual");const auto deadline=g_peer_gate.deadline_ms;
 lcd::g_lcd_coord_boot_id=13;receive_reply();CHECK(!g_peer_gate.ready&&!g_peer_gate.locked&&g_peer_gate.peer_boot==0&&g_coord_sequence==3);
 receive_reply();CHECK(lcd::ota_locked&&!g_peer_gate.ready&&g_peer_gate.peer_boot==13);
 receive_reply();CHECK(g_peer_gate.ready&&g_peer_gate.deadline_ms==deadline&&lcd::g_lcd_coord_lease_until_ms==deadline);
 // User input remains authoritative after reacquisition; duplicate wire
 // requests do not create an automatic lease-stealing loop.
 reset_touch();halo_prod_request_manual_ota("manual");receive_reply();receive_reply();
 now_ms+=2001;lcd::lcd_timer_receiver_wait_release("user_input");halo_prod_request_manual_ota("manual");receive_reply();
 CHECK(!g_peer_gate.ready&&!lcd::ota_locked&&g_coord_sequence==2&&sent.size()==2);
 // Active ownership, completed/expired episodes, and local non-VALID state
 // cannot use this handoff. No sequence, deadline or durable field changes.
 for(unsigned i=0;i<13;++i){reset_touch();switch(i){
  case 0:g_peer_gate.entered=true;break;case 1:g_ota_check_in_progress=true;break;
  case 2:g_ota_apply_in_progress=true;break;case 3:g_lcd_ota_task_running=true;break;
  case 4:g_lcd_ota_proxy_owns_uart=true;break;case 5:g_lcd_work_budget_live=true;break;
  case 6:g_peer_continue_work=true;break;case 7:g_peer_episode_finished=true;break;
  case 8:g_ota_check_done=true;break;case 9:local_valid=false;break;
  case 10:now_ms=g_peer_gate.deadline_ms;break;case 11:g_boot_ota_deadline_ms=now_ms;break;
  case 12:g_boot_ota_pending=false;break;}
  Preserved p;halo_prod_request_manual_ota("manual");CHECK(!g_manual_ota_override&&OtaIntent::updates==0&&g_coord_sequence==1);p.check();
 }
 // Foreground work blocks replacement traffic; release still has the old
 // deadline, with truthful failure and no policy charge if it expires.
 reset_touch();halo_prod_request_manual_ota("manual");Preserved p;action_busy=true;receive_reply();
 CHECK(sent.size()==1&&!lcd::ota_locked&&!g_peer_gate.ready);p.check();action_busy=false;receive_reply();
 now_ms=g_peer_gate.deadline_ms;ota_peer_service();
 CHECK(!g_peer_gate.active&&!g_manual_ota_override&&wire_result=="peer_unavailable");p.check();
 // Sequence wrap skips zero, and millis wrap keeps the original deadline.
 reset_touch();g_coord_sequence=g_peer_gate.sequence=0xffffffffU;lcd::g_lcd_coord_sequence=0xffffffffU;
 now_ms=0xfffffff0U;g_boot_ota_deadline_ms=g_peer_gate.deadline_ms=32;
 halo_prod_request_manual_ota("manual");CHECK(g_peer_gate.sequence==1&&g_manual_ota_override_until_ms==32);
 receive_reply();CHECK(lcd::g_lcd_coord_sequence==1&&lcd::g_lcd_coord_lease_until_ms==32&&!g_peer_gate.ready);
 now_ms=0;receive_reply();CHECK(g_peer_gate.ready);now_ms=32;ota_peer_service();CHECK(!g_peer_gate.active);
 // Correlated owner echo limits remain enforced after replacement.
 for(unsigned bad=0;bad<4;++bad){reset_touch();halo_prod_request_manual_ota("manual");receive_reply();
  reply={};reply.correlated=true;reply.peer_boot_id=12;strcpy(reply.coord_owner,g_peer_gate.owner);reply.coord_lease_ms=100;
  if(bad==0)reply.coord_owner[0]=0;if(bad==1)reply.coord_lease_ms=0;
  if(bad==2)reply.coord_lease_ms=120001;if(bad==3)reply.peer_boot_id=0;
  poll_result=LCD_QUERY_READY;g_peer_gate.querying=true;ota_peer_service();CHECK(!g_peer_gate.ready);
 }
 printf("PASS %u actual Sense/LCD touch handoff, deadline, identity, ownership and durable-accounting checks\n",checks);
}
'''


def run(root, out, negative=False, sanitize=False, arduino_json=None):
    out.mkdir(parents=True, exist_ok=False)
    src = out / 'test.cpp'
    src.write_text(harness(root, negative))
    library = arduino_json or Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src'
    command = ['c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-I' + str(library),
               '-I' + str(root / 'halo_ota_demo/firmware/shared'), str(src), '-o', str(out / 'test')]
    if sanitize:
        command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    built = subprocess.run(command, capture_output=True, text=True, timeout=40)
    (out / 'compile.log').write_text(built.stdout + built.stderr)
    if built.returncode:
        raise RuntimeError('Compile failed: ' + str(out / 'compile.log'))
    result = subprocess.run([str(out / 'test')], capture_output=True, text=True, timeout=20)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end='')
    paths = [WRAPPER, 'LCD_Minimal/LCD_Minimal.ino', 'LCD_Minimal/lcd_uart_rx.h',
             'Sense_Minimal/sense_ota_lcd.h', 'halo_ota_demo/firmware/shared/SenseDurablePolicyRuntime.h']
    receipt = {'passed': result.returncode == 0, 'negative_control': negative,
               'sanitizers': sanitize, 'source_root': str(root), 'command': command,
               'sources': {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in paths},
               'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
               'hardware_actions': 0, 'network_actions': 0,
               'scope': 'Actual request, peer readiness tail, JSON lock encoder, LCD lock/touch cancellation and durable entry. UART delivery, task scheduling, clock and NVS transport are deterministic doubles.'}
    (out / 'RESULT.json').write_text(json.dumps(receipt, indent=2) + '\n')
    if result.returncode:
        raise RuntimeError('Test failed: ' + str(out / 'run.log'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--negative-control', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--arduino-json', type=Path)
    args = parser.parse_args()
    if args.out:
        run(args.source_root, args.out, args.negative_control, args.sanitize, args.arduino_json)
    else:
        with tempfile.TemporaryDirectory(prefix='manual-touch-') as temp:
            run(args.source_root, Path(temp) / 'run', args.negative_control, args.sanitize, args.arduino_json)
