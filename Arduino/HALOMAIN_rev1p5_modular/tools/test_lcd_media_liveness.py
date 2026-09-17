#!/usr/bin/env python3
"""Compose actual typed Store/transport, peer-liveness and sleep handshake.

Real temporary files/production JSON/CRC, with UART/clock/RTOS boundary doubles.
Exercises 320000-byte binary-only transfers, subsequent admission, and both
terminal/expired-PONG lock orderings. No hardware or cloud success is simulated.
Frozen166 is an executable negative control through --source-root.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
CASES=('long_save_next_begin','expiry_then_terminal','terminal_then_expiry',
       'long_replay','wrong_chunk','wrong_ack','wrong_abort','receive_timeout',
       'late_chunk_ack','late_end_ack','stale_then_terminal','terminal_then_stale')
def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/(name+'.py'))
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def harness(root,kind):
    define=module('test_lcd_maintenance_sleep').definition
    source=(root/'LCD_Minimal/LCD_Minimal.ino').read_text()
    sleep=(root/'LCD_Minimal/lcd_sleep.h').read_text()
    text=module('test_lcd_'+kind+'_transport').HARNESS.split('int main(){',1)[0]
    # Replace only the old suite's explicit liveness stub with actual code.
    stub=define(text,'static void note_sense_binary_media_rx(')
    text=text.replace(stub,'')
    text=text.replace('static uint32_t tick_ms=1000;','static unsigned media_critical_depth=0;\nstatic uint32_t tick_ms=1000,response_delay=1,terminal_response_delay=0;\nstatic void(*before_lock)()=nullptr;\nstatic void(*after_printf)()=nullptr;\nstatic void(*after_unlock)()=nullptr;\nstatic void(*delay_hook)()=nullptr;\nstatic void(*frame_wait_hook)()=nullptr;')
    text=text.replace('static unsigned media_critical_depth=0;\nstatic void portENTER','static void portENTER')
    text=text.replace('static uint32_t millis(){return tick_ms;}','static uint32_t millis(){check(!media_critical_depth,"no clock read under state mux");return tick_ms;}')
    text=text.replace('static void vTaskDelay(uint32_t n){tick_ms+=n;}','static void vTaskDelay(uint32_t n){tick_ms+=n;if(delay_hook){auto h=delay_hook;delay_hook=nullptr;h();}}')
    text=text.replace('template<class... Args>void printf(const char*,Args...){}','template<class... Args>void printf(const char*,Args...){check(!media_critical_depth,"no logging under state mux");if(after_printf){auto h=after_printf;after_printf=nullptr;h();}}')
    text=text.replace('input_events.pop_front();++tick_ms;','input_events.pop_front();tick_ms+=(terminal_response_delay&&!output_frames.empty()&&output_frames.back().type==MSG_IMG_END)?terminal_response_delay:response_delay;if(frame_wait_hook)frame_wait_hook();')
    text=text.replace('check(media_critical_depth==0,', 'if(before_lock){auto h=before_lock;before_lock=nullptr;h();}check(media_critical_depth==0,',1)
    text=text.replace('--media_critical_depth;}','--media_critical_depth;if(after_unlock){auto h=after_unlock;after_unlock=nullptr;h();}}',1)
    pre=r'''
static portMUX_TYPE s_sense_pong_mux=portMUX_INITIALIZER_UNLOCKED;
enum SenseState{SENSE_UNKNOWN,SENSE_AWAKE,SENSE_ASLEEP};
static SenseState sense_state=SENSE_AWAKE;
static uint8_t sense_missed_pongs=0;
static bool sense_pong_pending=false,sense_awake_confirmed=false,sense_awake_estimate=true,sense_rx_stale_logged=false;
static unsigned long sense_pong_deadline_ms=0,last_sense_rx_ms=1000,last_sense_any_rx_ms=1000,last_sense_msg_ms=1000,last_proof_of_life_ms=1000,last_sense_sleep_ready_ms=0;
static bool sleep_ready_received=false;
static void lcd_errlog_store_with_context(const char*,const char*,const char*,int,const char*){check(!media_critical_depth,"no error storage under state mux");}
enum {REFRESH_IDLE,REFRESH_WAKE_PENDING,REFRESH_INFLIGHT};
static int refresh_state=REFRESH_IDLE;
static uint32_t safe_age_ms(uint32_t n,uint32_t t){return n>=t?n-t:0;}
'''
    for name in ['SENSE_PONG_TIMEOUT_MS','SENSE_MISSED_PONGS_FOR_ASLEEP','SENSE_UNKNOWN_STALE_MS','SENSE_UNKNOWN_STALE_EXTENDED_MS']:
        pre+=re.search(r'^static const [^;\n]+\b'+name+r'\s*=\s*[^;]+;',source,re.M).group()+'\n'
    for sig in ['static const char* sense_state_name(','static void sense_pong_reset(','static void sense_pong_arm(','static bool sense_pong_take_expired(','static void sense_state_set(','static void refresh_sense_awake_estimate(']:pre+=define(source,sig)+'\n'
    if 'static void note_sense_binary_media_rx(' in source:pre+=define(source,'static void note_sense_binary_media_rx(')+'\n'
    block=define(source,'if (sense_pong_take_expired(now_ms))')
    pre+='static void timeout_tick(){const auto now_ms=millis();'+block+'}\n'
    pre+='static void peer_age_tick(){refresh_sense_awake_estimate(millis());timeout_tick();}\n'
    header='#include "LCD_Minimal/lcd_'+kind+'_spool.h"'
    text=text.replace(header,pre+header)
    # Add the other real namespace/handler for a following IMAGE_BEGIN.
    if kind=='voice':text=text.replace(header,header+'\n#define LCD_IMAGE_SPOOL_DIR "image-next"\n#include "LCD_Minimal/lcd_image_spool.h"')
    sleep_stubs=r'''
static std::atomic<bool> g_lcd_sleep_handshake_active{false};
static bool provisioning_active=false,link_synced=true,link_sync_pending=false;
static unsigned long sense_awake_grace_until_ms=0;
static constexpr unsigned SENSE_RX_STALE_MS=8000,SENSE_SLEEP_READY_GRACE_MS=15000,SENSE_RECENT_RX_FOR_SLEEP_MS=10000;
static bool sleep_deny_received=false,sleep_deny_active=false,sleep_handshake_fail_link=false,sleep_wait_for_sense_idle=false,sleep_retry_requires_user=false;
static unsigned long sleep_deny_retry_ms=0,sleep_deny_received_ms=0,sleep_retry_allowed_ms=0;
static unsigned sleep_fallback_timer_sec=0,sleep_handshake_fail_count=0;
static constexpr unsigned SLEEP_FALLBACK_TIMER_SEC=15,SLEEP_DENY_RETRY_DEFAULT_MS=5000,SLEEP_HANDSHAKE_MAX_ATTEMPTS=3,SLEEP_HANDSHAKE_RETRY_DELAY_MS=800;
static char sleep_deny_reason[64]={};
static unsigned long last_user_activity_ms=900,last_scroll_activity_ms=900,ota_stay_awake_until_ms=0,touch_ignore_until=0,scroll_ignore_until=0,touch_press_time=0;
static bool touch_pressed=false,touch_wake_only_pending=false,long_press_sent=false,ship_ai_touch_active=false;
static uint16_t touch_press_x=0,touch_press_y=0;
static unsigned sleep_messages=0;
static bool sleep_blocked_for_ota(){return ota_busy;}
static void lcd_send_diag_pre_sleep(){}
static void lcd_sleep_ts(const char*){}
static void send_sense_ping(){sense_pong_arm(millis());}
static bool sleep_prepare_wake_line_for_request(){return true;}
static void send_input_sleep_message(){++sleep_messages;}
static int getTouch(uint16_t*,uint16_t*){return 0;}
static void cancel_pending_sleep_for_user_input(const char*){}
static void abort_sleep_transition(const char*,bool){}
static void sleep_enter_wait_low_power(const char*){}
static bool s_sleep_media_deferred=false;
'''
    provision=('#include "'+str(root/'LCD_Minimal/lcd_provision_flow.h')+'"\nstatic LcdProvisionFlow provision_flow;\nstatic bool s_sleep_provision_deferred=false;\n'+define(sleep,'static bool sleep_defer_for_provisioning() {')+'\n') if 'static bool sleep_defer_for_provisioning() {' in sleep else ''
    sleep_actual=define(sleep,'static bool sleep_defer_for_media() {')+'\n'+provision+define(sleep,'struct LcdSleepHandshakeScope {')+';\n'+define(sleep,'static bool notify_sense_sleep() {')
    main=r'''
static void poison_stale_sleep(){
 sense_state=SENSE_ASLEEP;sense_awake_estimate=false;sense_missed_pongs=2;
 sleep_ready_received=true;last_sense_sleep_ready_ms=millis()-1;
 last_sense_rx_ms=last_proof_of_life_ms=1;
 sense_pong_pending=true;sense_pong_deadline_ms=millis();
}
static void terminal_hook(){after_unlock=after_printf=before_lock=nullptr;frame(MSG_IMG_END,1);check(!lcd_KIND_receive_loop(),"actual terminal receive completes in interleave");}
static void following_image(){
 halo_image::Meta m;m.len=8;const uint8_t p[8]={1,2,3,4,5,6,7,8};m.crc32=halo_image::crc32(p,8);m.job_id=63;m.epoch=1789500000;
 strcpy(m.owner_id,"test-owner");strcpy(m.device_id,"test-device");strcpy(m.mode,"discard");std::snprintf(m.request_id,sizeof(m.request_id),"%032x",63);
 strcpy(m.checksum_sha256,"66840dda154e8a113c31dd0ad32f7f3a366a80e8136979d8f5a101d3d29d6f72");
 check(halo_image::valid(m),"following image metadata valid");
 JsonDocument d;lcd_image_put_meta(d.to<JsonObject>(),m);d["type"]="IMAGE_XFER_BEGIN";lcd_image_uart(d);
 check(g_image_rx&&g_img_rx_active,"next actual image BEGIN admitted while handshake waits");
}
static void assert_fresh(){
 check(sense_state==SENSE_AWAKE&&sense_awake_estimate,"accepted media overrides stale peer ASLEEP");
 check(last_sense_rx_ms>1&&last_proof_of_life_ms==last_sense_rx_ms&&last_sense_any_rx_ms==last_sense_rx_ms&&last_sense_msg_ms==last_sense_rx_ms,"all peer RX clocks refreshed");
 check(!sleep_ready_received&&!last_sense_sleep_ready_ms&&!sense_pong_pending&&!sense_missed_pongs,"old sleep/PONG episode invalidated");
 check(last_user_activity_ms==900&&last_scroll_activity_ms==900&&link_synced,"user/scroll/sync untouched");
}
static void long_save(){
 std::vector<uint8_t> body(320000);for(size_t i=0;i<body.size();++i)body[i]=(uint8_t)i;
 auto m=fixture(7);m.len=body.size();m.crc32=halo_KIND::crc32(body.data(),body.size());begin(m);
 const uint32_t started=g_KIND_started_ms;uint16_t seq=0;uint32_t next_probe=millis()+11000;
 for(size_t pos=0;pos<body.size();pos+=512){
  tick_ms+=58;peer_age_tick();
  if(millis()>=next_probe){sense_pong_arm(millis());next_probe+=11000;}
  std::vector<uint8_t> chunk(body.begin()+pos,body.begin()+std::min(body.size(),pos+512));
  frame(MSG_IMG_CHUNK,seq++,chunk);check(lcd_KIND_receive_loop(),"real long binary chunk accepted");
 }
 check(millis()-started>30000&&millis()-started<90000,"actual transfer crosses JSON stale threshold within original budget");
 frame(MSG_IMG_END,seq);check(!lcd_KIND_receive_loop()&&released()&&retained(m),"actual full payload committed and terminal custody released");
 check(g_KIND_started_ms==started,"media deadline never restarted");assert_fresh();
 // There is no JSON heartbeat in the gap. Main must send/wait INPUT_SLEEP,
 // then observe the real next BEGIN instead of returning local sleep.
 delay_hook=following_image;const bool sleep=notify_sense_sleep();
 check(!sleep&&sleep_messages==1&&g_image_rx,"terminal gap waits and next media defers sleep");
 if(g_image_rx)lcd_image_release("host_cleanup",true);
}
int main(int argc,char**argv){
 check(argc==2,"one case required");std::string name=argv[1];reset_case();
 if(name=="long_save_next_begin"){long_save();}
 else if(name=="expiry_then_terminal"||name=="terminal_then_expiry"){
  auto m=fixture();begin(m);frame(MSG_IMG_CHUNK,0,BYTES);lcd_KIND_receive_loop();poison_stale_sleep();
  if(name=="expiry_then_terminal"){after_unlock=terminal_hook;timeout_tick();}
  else{const auto stale_now=millis();terminal_hook();sense_pong_take_expired(stale_now);}
  assert_fresh();check(retained(m)&&released(),"terminal race preserves real committed item");
 }else if(name=="stale_then_terminal"||name=="terminal_then_stale"){
  auto m=fixture();begin(m);frame(MSG_IMG_CHUNK,0,BYTES);lcd_KIND_receive_loop();
  tick_ms=35000;g_KIND_last_frame_ms=tick_ms;last_proof_of_life_ms=1;
  sense_state=SENSE_AWAKE;sense_awake_estimate=true;
  if(name=="stale_then_terminal"){after_unlock=terminal_hook;after_printf=terminal_hook;}
  else before_lock=terminal_hook;
  refresh_sense_awake_estimate(millis());
  assert_fresh();check(released(),"stale ordering consumed actual terminal receive");
 }else if(name=="late_chunk_ack"||name=="late_end_ack"){
  auto m=fixture();commit(m);fetch(m);poison_stale_sleep();peer=Peer::Accept;auto_abort=false;
  if(name=="late_chunk_ack")response_delay=4001;else terminal_response_delay=4001;
  lcd_KIND_send_file();
  check(last_sense_rx_ms < g_KIND_last_frame_ms+4000,"late ACK never advances last valid RX past original idle deadline");
  if(name=="late_chunk_ack")check(last_sense_rx_ms==1&&sense_state==SENSE_ASLEEP,"late first ACK has no liveness proof");
  check(g_KIND_waiting_abort&&g_suppress_uart_json_tx,"expired ACK keeps original bound cleanup quarantine");
 }else if(name=="long_replay"){
  // Persist a genuine320k payload through the actual store, then exercise the
  // full actual FETCH/ACK/ABORT path with 60ms peer frame service.
  std::vector<uint8_t> body(320000,17);auto m=fixture();m.len=body.size();m.crc32=halo_KIND::crc32(body.data(),body.size());
  check(g_KIND_store.begin(m)==halo_KIND::Result::Ok,"seed actual replay store");uint16_t seq=0;
  for(size_t p=0;p<body.size();p+=512)check(g_KIND_store.append(seq++,body.data()+p,std::min<size_t>(512,body.size()-p))==halo_KIND::Result::Ok,"append seeded replay bytes");
  check(g_KIND_store.finish(seq)==halo_KIND::Result::Ok,"commit seeded replay bytes");
  fetch(m);poison_stale_sleep();peer=Peer::Accept;response_delay=60;frame_wait_hook=peer_age_tick;
  const auto started=g_KIND_started_ms;lcd_KIND_send_file();frame_wait_hook=nullptr;
  check(millis()-started>30000&&millis()-started<90000,"actual replay crosses JSON stale threshold");assert_fresh();
  check(released(),"bound cleanup releases transfer ownership");
  g_KIND_sd_work_started_ms=millis();g_KIND_sd_work_budget_ms=12000;
  check(retained(m),"fresh bounded inventory retains SD; no deletion or cloud receipt fabricated");
 }else if(name=="wrong_chunk"||name=="receive_timeout"){
  auto m=fixture();begin(m);poison_stale_sleep();
  if(name=="wrong_chunk")frame(MSG_IMG_CHUNK,9,BYTES);else tick_ms=g_KIND_started_ms+90000;
  lcd_KIND_receive_loop();check(last_sense_rx_ms==1&&sense_state==SENSE_ASLEEP,"invalid or timed-out receive cannot refresh proof");
 }else if(name=="wrong_ack"){
  auto m=fixture();commit(m);fetch(m);poison_stale_sleep();peer=Peer::WrongChunk;auto_abort=false;
  lcd_KIND_send_file();check(last_sense_rx_ms==1&&sense_state==SENSE_ASLEEP,"wrong ACK does not refresh proof");
 }else if(name=="wrong_abort"){
  auto m=fixture();begin(m);poison_stale_sleep();JsonDocument d;deserializeJson(d,abort_json(m));d["job_id"]=m.job_id+1;
  check(!lcd_KIND_abort(d.as<JsonObjectConst>()),"wrong bound abort refused");check(last_sense_rx_ms==1,"wrong bound abort no proof");
 }
 printf("%s KIND %s (%u checks,%u failures)\n",failures?"FAIL":"PASS",name.c_str(),checks,failures);return failures?1:0;
}
'''.replace('KIND',kind).replace('BYTES','pcm' if kind=='voice' else 'jpeg')
    return text+sleep_stubs+sleep_actual+'\n'+main

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,default=ROOT);p.add_argument('--case',choices=CASES,action='append');p.add_argument('--media',choices=['voice','image'],action='append');a=p.parse_args()
    failures=0
    for kind in a.media or ('voice','image'):
      with tempfile.TemporaryDirectory(prefix='halo-media-life-') as tmp:
        tmp=Path(tmp);cpp=tmp/'test.cpp';exe=tmp/'test';cpp.write_text(harness(a.source_root,kind))
        aj=a.source_root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
        subprocess.run([shutil.which('c++'),'-std=c++17','-Wno-deprecated-declarations','-I',str(a.source_root),'-I',str(aj),str(cpp),'-o',str(exe)],check=True,timeout=30)
        for case in a.case or CASES:
          directory=tmp/case;directory.mkdir();r=subprocess.run([str(exe),case],cwd=directory,timeout=15);failures+=r.returncode!=0
    print(f'{failures} failed cases; actual source with controlled host boundaries');return int(bool(failures))
if __name__=='__main__':raise SystemExit(main())
