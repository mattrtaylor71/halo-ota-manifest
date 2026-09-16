#!/usr/bin/env python3
"""Actual LCD sleep/typed-media admission and deadline controls; host-only.

Extracts production handshake, sleep-entry prefix through final admission, typed
link-idle, expiry/release and quarantine functions. Controlled RTOS/time/storage
boundaries model deterministic interleavings, not ESP electrical/flash behavior.
--source-root accepts frozen165 unchanged: the late receive case then fails by
actually returning fallback success while receive custody is active.
"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ('late_image', 'late_voice', 'ready_during_receive', 'recent_ready_race',
         'asleep_race', 'unsynced_race', 'fallback_edge_race',
         'pending_tx', 'active_tx', 'binary_only', 'initial_rx', 'force_rx',
         'before_guard_race', 'sleep_wins_admission', 'voice_idle_expiry',
         'image_idle_expiry', 'voice_total_expiry', 'image_total_expiry',
         'voice_quarantine_retirement', 'image_quarantine_retirement',
         'ordinary_timeout', 'ota_unchanged', 'commit_busy', 'completed_media')


def load_base():
    spec = importlib.util.spec_from_file_location('maintenance_sleep', ROOT / 'tools/test_lcd_maintenance_sleep.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def harness(root):
    base = load_base(); definition = base.definition
    sleep = (root / 'LCD_Minimal/lcd_sleep.h').read_text()
    text = base.harness(root)
    # The base provides declarations/rendering doubles only; replace its test
    # injection functions and main. Runtime functions remain actual source.
    old = definition(text, 'static int getTouch(uint16_t*,uint16_t*){')
    text = text.replace(old, 'static int getTouch(uint16_t*,uint16_t*){return 0;}')
    old = definition(text, 'static void vTaskDelay(unsigned long delay){')
    text = text.replace(old, '''static void vTaskDelay(unsigned long delay){
      now_ms+=delay;++delay_calls;
      if(!injected && now_ms>=73000 && (scenario=="late_image"||scenario=="late_voice"||scenario=="ready_during_receive")){
        injected=true;g_img_rx_active=true;g_img_rx_binary_mode=true;
        if(scenario=="ready_during_receive")sleep_ready_received=true;
      }
    }''')
    text = text[:text.index('\nint main(')]
    # Serial/guard hooks inject at final success and lock acquisition boundaries.
    text = text.replace('static struct {template<class... A>void printf(const char*,A...){} void println(const char*){}} Serial;', '''static void serial_hook(const char*);
static struct {template<class... A>void printf(const char* s,A...){serial_hook(s);} void println(const char* s){serial_hook(s);}} Serial;''')
    declarations = r'''
static bool g_suppress_uart_json_tx=false;
static std::atomic<bool> g_lcd_sleep_commit_gate{false};
static bool image_valid=true;
static bool lcd_nvs_image_valid(){return image_valid;}
static bool lcd_ota_in_progress_for_sd_guard(){return false;}
static int lock_depth=0,teardown=0,blocked_admissions=0,media_releases=0;
static bool race_at_guard=false,user_activity_since_sleep=false;
struct LcdMaintenanceStorageGuard {
 LcdMaintenanceStorageGuard(){if(race_at_guard){g_img_rx_active=true;g_img_rx_binary_mode=true;race_at_guard=false;}++lock_depth;}
 ~LcdMaintenanceStorageGuard(){--lock_depth;}
};
static unsigned sleep_deny_count=2;
static constexpr unsigned SLEEP_DENY_MAX_COUNT=4,SLEEP_LINK_RETRY_MS=2000;
static unsigned long last_sleep_retry_log_ms;
static bool notify_sense_sleep();
'''
    if 'static bool s_sleep_media_deferred' not in text:
        declarations += 'static bool s_sleep_media_deferred=false;\n'
    if 'static bool sleep_defer_for_media() {' in sleep and 'static bool sleep_defer_for_media() {' not in text:
        declarations += definition(sleep, 'static bool sleep_defer_for_media() {')+'\n'
    # Insert after the base custody globals, before any runtime helper.
    text = text.replace('static bool touch_pressed,touch_wake_only_pending,long_press_sent,ship_ai_touch_active;', declarations+'\nstatic bool touch_pressed,touch_wake_only_pending,long_press_sent,ship_ai_touch_active;')
    # Functions used here are already in scope at this point. Include typed
    # transport lifecycle source; only actual media storage abort is doubled.
    typed = ''
    for kind in ('voice','image'):
        src=(root/f'LCD_Minimal/lcd_{kind}_spool.h').read_text()
        typed += f'''
namespace halo_{kind} {{struct Meta {{uint32_t job_id=1,len=8;}};}}
static struct {{void abort(){{}} uint32_t received(){{return 0;}}}} g_{kind}_store;
static halo_{kind}::Meta g_{kind}_transfer_meta;
static bool g_{kind}_rx=false,g_{kind}_tx_pending=false,g_{kind}_waiting_abort=false;
static uint32_t g_{kind}_started_ms=0,g_{kind}_last_frame_ms=0,g_{kind}_sd_failed=0;
'''
        for token in (f'LCD_{kind.upper()}_XFER_MS',f'LCD_{kind.upper()}_IDLE_MS'):
            line=next(x for x in src.splitlines() if f' {token} =' in x);typed+=line+'\n'
        for anchor in (f'static bool lcd_{kind}_link_idle() {{',f'static void lcd_{kind}_release(',f'static bool lcd_{kind}_expired() {{',f'static void lcd_{kind}_quarantine_tick() {{'):
            typed += definition(src,anchor)+'\n'
        loop=definition(src,f'static bool lcd_{kind}_receive_loop() {{')
        # Exact opening expiry branch, before frame decode. Full frame/commit
        # handlers are independently exercised by the existing transport suite.
        early=loop[:loop.index('  uint8_t type=0;')]
        typed+=early+'  return true;\n}\n'
    text += '\nstatic void lcd_media_release(){++media_releases;}\nstatic void lcd_freeze_wdt_feed(){}\n'+typed
    # Final boundary uses actual entry prefix and actual SleepCommitGate. The
    # excluded UI/timer/GPIO teardown is a sentinel, not simulated hardware.
    enter=definition(sleep,'static void enterLightSleep() {')
    end=enter.index('  sleep_handshake_fail_count = 0;\n  sleep_deny_count = 0;')
    prefix=enter[:end]
    gate=definition(enter,'  struct SleepCommitGate {')+' sleep_commit_guard;\n'
    text+='\n'+prefix+gate+'''  ++teardown;
  if(lock_depth && g_lcd_sleep_commit_gate.load() && !lcd_voice_link_idle() && !lcd_image_link_idle())++blocked_admissions;
}\n'''
    # Structural closure: admission guard precedes all UI teardown and remains
    # in the function scope through commit; both typed admission call sites use
    # that same guard before reading link-idle and publishing active custody.
    if 'sleep_defer_for_media' in sleep:
        assert enter.count('LcdMaintenanceStorageGuard sleep_arm_guard;')==1
        assert enter.index('LcdMaintenanceStorageGuard sleep_arm_guard;')<enter.index('sense_awake_confirmed = false;')<enter.index('struct SleepCommitGate')
        for kind in ('voice','image'):
            src=(root/f'LCD_Minimal/lcd_{kind}_spool.h').read_text()
            fn=definition(src,f'static bool lcd_{kind}_uart(')
            assert fn.index('LcdMaintenanceStorageGuard admission_guard;')<fn.index(f'if(!lcd_{kind}_link_idle()')<fn.index('g_img_rx_active=true;')
    return text+r'''
static void serial_hook(const char* s){
 if(injected)return;
 bool fire=(scenario=="recent_ready_race"&&strstr(s,"sense_ready_recent ->"))||
  (scenario=="asleep_race"&&strstr(s,"sense_asleep ->"))||
  (scenario=="unsynced_race"&&strstr(s,"link_unsynced_stale"))||
  (scenario=="fallback_edge_race"&&strstr(s,"no_response_to_INPUT_SLEEP"));
 if(fire){injected=true;g_img_rx_active=true;g_img_rx_binary_mode=true;}
}
int main(int argc,char**argv){
 assert(argc==2);scenario=argv[1];sleep_handshake_fail_count=2;
 if(scenario=="pending_tx")g_spool_tx_pending=true;
 if(scenario=="active_tx")g_spool_tx_active=true;
 if(scenario=="binary_only")g_img_rx_binary_mode=true;
 if(scenario=="initial_rx"||scenario=="force_rx")g_img_rx_active=true;
 if(scenario=="force_rx")sleep_deny_count=SLEEP_DENY_MAX_COUNT;
 if(scenario=="recent_ready_race")last_sense_sleep_ready_ms=now_ms-1;
 if(scenario=="asleep_race"||scenario=="before_guard_race"||scenario=="sleep_wins_admission"||scenario=="commit_busy"||scenario=="completed_media")sense_state=SENSE_ASLEEP;
 if(scenario=="unsynced_race"){link_synced=false;now_ms=80000;last_sense_rx_ms=1;}
 if(scenario=="before_guard_race")race_at_guard=true;
 if(scenario=="ota_unchanged")g_lcd_validation_pending=true;
 if(scenario=="commit_busy"){
  g_lcd_sleep_commit_gate=true;assert(!lcd_voice_link_idle()&&!lcd_image_link_idle());
  g_lcd_sleep_commit_gate=false;assert(lcd_voice_link_idle()&&lcd_image_link_idle());return 0;
 }
 bool expiry=scenario.find("_expiry")!=std::string::npos;
 bool quarantine=scenario.find("_quarantine_retirement")!=std::string::npos;
 if(expiry||quarantine){
  const bool voice=scenario.find("voice_")==0;
  sense_state=SENSE_ASLEEP;now_ms=1000;
  if(quarantine){
   g_spool_tx_active=true;g_suppress_uart_json_tx=true;
   if(voice){g_voice_waiting_abort=true;g_voice_started_ms=1000;}else{g_image_waiting_abort=true;g_image_started_ms=1000;}
  }else{
   g_img_rx_active=g_img_rx_binary_mode=true;g_suppress_uart_json_tx=true;
   if(voice){g_voice_rx=true;g_voice_started_ms=g_voice_last_frame_ms=1000;}
   else{g_image_rx=true;g_image_started_ms=g_image_last_frame_ms=1000;}
  }
  enterLightSleep();assert(teardown==0&&sleep_handshake_fail_count==2&&sleep_deny_count==2);
  bool total=scenario.find("total")!=std::string::npos;
  uint32_t bound=quarantine||total?90000:4000;
  now_ms=1000+bound-1;
  if(total){if(voice)g_voice_last_frame_ms=now_ms;else g_image_last_frame_ms=now_ms;}
  if(quarantine){if(voice)lcd_voice_quarantine_tick();else lcd_image_quarantine_tick();assert(g_spool_tx_active);}
  else {bool live=voice?lcd_voice_receive_loop():lcd_image_receive_loop();assert(live);}
  ++now_ms;
  if(quarantine){if(voice)lcd_voice_quarantine_tick();else lcd_image_quarantine_tick();assert(!g_spool_tx_active&&g_suppress_uart_json_tx);}
  else {bool live=voice?lcd_voice_receive_loop():lcd_image_receive_loop();assert(!live&&!g_img_rx_active&&!g_img_rx_binary_mode);}
  enterLightSleep();assert(teardown==1&&lock_depth==0&&blocked_admissions==1);
 }else{
  unsigned before_deny=sleep_deny_count;
  enterLightSleep();
  const bool pass=scenario=="ordinary_timeout"||scenario=="sleep_wins_admission"||scenario=="completed_media";
  printf("OBS %s teardown=%d active_rx=%d time=%lu msgs=%u\n",scenario.c_str(),teardown,g_img_rx_active,now_ms,sleep_messages);
  if(pass){assert(teardown==1);assert(blocked_admissions==1);}
  else {assert(teardown==0);assert(sleep_handshake_fail_count==2&&sleep_deny_count==before_deny);}
  if(scenario=="ordinary_timeout")assert(sleep_messages==3&&now_ms==86600&&sleep_fallback_timer_sec==15);
  if(scenario=="late_image"||scenario=="late_voice"||scenario=="ready_during_receive")assert(now_ms<=73040);
 }
 assert(lock_depth==0&&!g_lcd_sleep_handshake_active&&!g_lcd_sleep_commit_gate);
 printf("PASS %s\n",scenario.c_str());
}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root',type=Path,default=ROOT)
    p.add_argument('--case',choices=CASES,action='append')
    a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-sleep-media-') as tmp:
        tmp=Path(tmp);source=tmp/'check.cpp';binary=tmp/'check';source.write_text(harness(a.source_root))
        subprocess.run([shutil.which('c++'),'-std=c++17',str(source),'-o',str(binary)],check=True,timeout=30)
        failures=0
        for case in a.case or CASES:
            r=subprocess.run([str(binary),case],timeout=5);failures+=r.returncode!=0
        print(f'{len(a.case or CASES)} cases, {failures} failures; controlled host boundaries only')
        return int(bool(failures))
if __name__=='__main__':raise SystemExit(main())
