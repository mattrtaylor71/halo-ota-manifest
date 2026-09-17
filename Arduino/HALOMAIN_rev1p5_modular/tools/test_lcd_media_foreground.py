#!/usr/bin/env python3
"""Compose real shipping touch/queue code with real typed SD handlers/store.

GPIO/LVGL visuals, RTOS queues, serial, clock and SD mount boundaries are doubles.
Production touch branches, queue admission/drain, metadata and FILE operations run.
An optional old-source control must compile and fail the actual busy-gesture
assertions; missing APIs or compilation failure never count as reproduction.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_manual_ota_clock import definition
import test_lcd_voice_transport as voice
import test_lcd_image_transport as image

ROOT = Path(__file__).resolve().parents[1]

QUEUE = r'''
typedef struct {char type[24];int delta;char id[40];bool has_delta;bool has_id;} tx_msg_t;
using QueueHandle_t=std::deque<tx_msg_t>*;
static std::deque<tx_msg_t> queue_items;
static QueueHandle_t uart_tx_queue=&queue_items;
static uint32_t uart_tx_dropped_count=0;
static unsigned long deferred_awake_tx_last_ping_ms=0;
static bool queue_full=false,proof_ready=true;
static void(*enqueue_hook)()=nullptr;
static constexpr int pdTRUE=1;
static int xQueueSend(QueueHandle_t q,const tx_msg_t* msg,unsigned){
#ifdef HAS_MEDIA_FOREGROUND
 check(!media_critical_depth,"queue I/O outside media lock");
#endif
 if(enqueue_hook){auto h=enqueue_hook;enqueue_hook=nullptr;h();}
 if(queue_full)return 0;q->push_back(*msg);return pdTRUE;
}
static int xQueueReceive(QueueHandle_t q,tx_msg_t* msg,unsigned){if(q->empty())return 0;*msg=q->front();q->pop_front();return pdTRUE;}
#include "LCD_Minimal/lcd_deferred_ring.h"
static std::vector<std::string> delivered;
static std::vector<uint32_t> delivered_at;
static bool g_lcd_ota_binary_mode=false,sense_awake_confirmed=true,link_synced=true;
static constexpr unsigned SENSE_CONTROL_READY_WINDOW_MS=2000;
static bool sense_recently_heard(unsigned){return true;}
static bool tx_msg_requires_awake_proof(const tx_msg_t*){return true;}
static bool sense_ready_for_control_tx(){return proof_ready;}
static void post_list_delete_result(const char*,bool){}
static void deferred_awake_tx_service(){}
static void tx_msg_send_now(const tx_msg_t* m){check(!g_suppress_uart_json_tx,"ordinary action dispatch only when JSON safe");delivered.push_back(m->type);delivered_at.push_back(tick_ms);}
static const int UART_TX_MAX_PER_LOOP=8;
'''
UI = r'''
static unsigned listening=0,on_it=0,toasts=0,scans=0,wakes=0;
static bool provisioned=true,ship_ai_touch_active=false,ship_ai_touch_blocked=false,long_press_sent=false;
static bool g_voice_fire_and_forget_ignore_ui=false,waiting_for_voice_response=false,g_ship_voice_json_pending=false;
static char g_ship_ui_op[16]="",g_ship_ui_phase[16]="";
static uint32_t g_ship_ui_job_id=0;
static bool g_ship_ui_finalized=false,g_ship_ui_dirty=false;
static unsigned long voice_response_deadline_ms=0,ship_voice_end_resend_due_ms=0,touch_press_time=0,ship_ai_listening_countdown_start_ms=0;
static uint8_t ship_voice_end_resends_remaining=0;
static char g_ship_voice_json_text[32]="";
static constexpr unsigned LONG_PRESS_THRESHOLD_MS=300;
static constexpr int SCREEN_AI_LISTENING=7;
static int ui_screen_state=0;
static bool provisioning_input_locked(){return !provisioned;}
static void io_boundary(){
#ifdef HAS_MEDIA_FOREGROUND
 check(!media_critical_depth,"UI/wake I/O outside media lock");
#endif
}
static void request_sense_wake(const char*){io_boundary();++wakes;}
static void show_auto_hiding_status_message(const char*,unsigned){io_boundary();++toasts;}
static void resetActivityTimer(){}
static void stop_glowing_animation(){}
static void ship_show_ai_listening_screen(){io_boundary();++listening;ui_screen_state=SCREEN_AI_LISTENING;}
static void show_ship_main_menu(){ui_screen_state=0;}
static void ship_update_ai_listening_countdown(){}
static void ship_set_processing_text(const char*){}
static void ship_show_voice_ack(){io_boundary();++on_it;}
static void ui_lvgl_tick(){}
static void example_lvgl_unlock(){}
static bool ship_main_menu_is_ai_action(const void*){return true;}
static void ship_menu_begin_local_scan_request(const char*){io_boundary();++scans;}
'''


def harness(root, media, negative=False):
    uart=(root/'LCD_Minimal/lcd_uart.h').read_text()
    task=(root/'LCD_Minimal/lcd_uart_task.h').read_text()
    ino=(root/'LCD_Minimal/LCD_Minimal.ino').read_text()
    screens=(root/'LCD_Minimal/lcd_ship_screens.h').read_text()
    action=(root/'LCD_Minimal/lcd_ship_action.h').read_text()
    source=(voice if media=='voice' else image).HARNESS.split('int main(){')[0]
    # Use the full real SD header; add a deterministic interleaving at send_frame.
    source='#include <map>\n'+source
    source=source.replace('class UartOtaProtocol {','static void(*chunk_hook)()=nullptr;\nstatic void(*frame_hook)(uint8_t)=nullptr;\nclass UartOtaProtocol {')
    source=source.replace('struct HardwareSerial {','static void(*json_hook)()=nullptr;\nstruct HardwareSerial {')
    source=source.replace("if(v=='{')json_pins.push_back(pin_count);","if(v=='{'){json_pins.push_back(pin_count);if(json_hook)json_hook();}")
    source=source.replace('output_frames.push_back(f);','output_frames.push_back(f);if(frame_hook)frame_hook(type);')
    source=source.replace('    if(type==MSG_IMG_CHUNK&&peer!=Peer::Passive){','    if(type==MSG_IMG_CHUNK&&chunk_hook){auto h=chunk_hook;chunk_hook=nullptr;h();}\n    if(type==MSG_IMG_CHUNK&&peer!=Peer::Passive){')
    source=source.replace('static bool lcd_media_deferred_intent_pending(){return false;}','')
    marker='#define LCD_'+media.upper()+'_SPOOL_DIR'
    queue=QUEUE
    if 'static bool lcd_media_deferred_intent_pending(' in uart:
        queue+=definition(uart,'static bool lcd_media_deferred_intent_pending(')+'\n'
    queue+=definition(uart,'static inline bool uart_tx_enqueue(')+'\n'
    source=source.replace(marker,queue+'\n'+marker)
    funcs=UI
    if 'static bool ship_voice_gesture_begin(' in screens:
        funcs+=definition(screens,'static bool ship_voice_gesture_begin(')+'\n'
    if 'static void ship_voice_status_begin(' in screens:
        funcs+=definition(screens,'static void ship_voice_status_begin(')+'\n'
    sig='static bool ship_queue_voice_input(' if 'static bool ship_queue_voice_input(' in screens else 'static void ship_queue_voice_input('
    funcs+=definition(screens,sig)+'\n'+definition(screens,'static void ship_service_voice_end_resend(')+'\n'
    funcs+=definition(action,'static void ship_menu_send_menu_select(')+'\n'
    press=definition(ino,'if (ship_main_menu_is_ai_action(hb))')
    held=definition(ino,'if (ship_ai_touch_active && !long_press_sent && !provisioning_input_locked())')
    release=definition(ino,'if (ship_ai_touch_active) {')
    blocked=definition(ino,'if(ship_ai_touch_blocked)') if 'if(ship_ai_touch_blocked)' in ino else ''
    funcs+='static void press(){touch_press_time=tick_ms;const void* hb=nullptr;'+press+'}\n'
    funcs+='static void held(){unsigned long now=tick_ms;'+held+'}\n'
    funcs+='static void release_touch(){unsigned long now=tick_ms;unsigned long press_duration=now-touch_press_time;bool was_long_press=long_press_sent;'+blocked+release+'}\n'
    if not negative:
        assert task.index('lcd_'+media+'_quarantine_tick();') < task.index('const bool binary_xfer_active =')
    drain_sig='if (!binary_xfer_active) {\n      while (uart_tx_queue'
    drain=definition(task,drain_sig)
    start=task.index('const bool binary_xfer_active =')
    binary=task[start:task.index(';',start)+1]
    funcs+='static void drain_queue(){tx_msg_t tx_msg{};unsigned tx_count=0;bool yielded_early=false;'+binary+drain+'}\n'
    funcs+=r'''
static void reset_ui(){
 auto_abort=false;queue_items.clear();deferred_ring_clear();delivered.clear();delivered_at.clear();queue_full=false;proof_ready=true;enqueue_hook=nullptr;chunk_hook=nullptr;frame_hook=nullptr;json_hook=nullptr;
 listening=on_it=toasts=scans=wakes=0;ship_ai_touch_active=ship_ai_touch_blocked=long_press_sent=false;
 ship_voice_end_resends_remaining=0;ship_voice_end_resend_due_ms=0;ui_screen_state=0;g_lcd_ota_binary_mode=false;
#ifdef HAS_MEDIA_FOREGROUND
 g_lcd_media_queued_intents=0;g_lcd_media_user_session=false;lcd_media_voice_end();lcd_media_release();
#endif
}
static void gesture(){press();tick_ms+=500;held();tick_ms+=2500;release_touch();}
static void camera(){ship_menu_send_menu_select("discard",2,"Discard");}
// Independent host-file audit: runtime lookup correctly expires its own SD
// scan budget after90s, so compare actual on-disk bytes without extending it.
static std::map<std::string,std::vector<uint8_t>> disk_snapshot(){
 std::map<std::string,std::vector<uint8_t>> out;
 for(const auto& e:std::filesystem::recursive_directory_iterator("."))if(e.is_regular_file()){
  const auto path=e.path().string();if(path=="./main.cpp"||path=="./test")continue;
  FILE* f=std::fopen(path.c_str(),"rb");check(f!=nullptr,"host audit opens retained file");
  std::vector<uint8_t> bytes;int c;while((c=std::fgetc(f))!=EOF)bytes.push_back((uint8_t)c);std::fclose(f);out[path]=bytes;
 }
 return out;
}
'''
    main=NEGATIVE if negative else MAIN
    # Bind one common test body to the corresponding actual voice/image APIs.
    main=main.replace('MEDIA',media).replace('NAMESPACE','halo_'+media).replace('LCD_'+media+'_IDLE','LCD_MEDIA_IDLE').replace('LCD_'+media+'_REPLAY','LCD_MEDIA_REPLAY').replace('LCD_'+media+'_XFER_MS','LCD_'+media.upper()+'_XFER_MS')
    return source+funcs+main

NEGATIVE=r'''
int main(){
 for(bool replay:{false,true}){
  reset_case();reset_ui();auto m=fixture();if(replay){commit(m);fetch(m);}else begin(m);
  gesture();
  check(listening==0&&on_it==0,"busy gesture rejected before Listening and On it");
  check(queue_items.empty(),"busy voice edges are not queued for delayed collapse");
  tick_ms+=30001;drain_queue();check(delivered.empty(),"binary path initially suppresses queued edges");
  lcd_MEDIA_release("negative_control_transport_end",false);drain_queue();
  if(delivered.size()==2&&delivered_at[0]==delivered_at[1])std::puts("REPRODUCED old START/END delivered in same drain after 30 seconds");
 }
 std::printf("%s %u old-source gesture controls (%u failures)\n",failures?"FAIL":"PASS",checks,failures);
 return failures?1:0;
}
'''
MAIN=r'''
int main(){
 {
  reset_case();reset_ui();auto m=fixture();commit(m);fetch(m);peer=Peer::DropChunk;
  chunk_hook=[](){lcd_media_note_user_input();};output_frames.clear();
  const auto started=tick_ms;lcd_MEDIA_send_file();
  check(tick_ms-started<3000&&pin_count==0&&g_MEDIA_waiting_abort,"wake-only tap/encoder cancels replay promptly without needing an action");
  check(retained(m),"wake-only yield preserves original SD media");
  JsonDocument a;deserializeJson(a,abort_json(m));lcd_MEDIA_uart(a);
  check(released()&&!lcd_media_try_claim(true,false),"replay stays paused for rest of user awake session after cleanup");
  check(lcd_media_try_claim(false,false),"fresh SD save remains admitted in user session");lcd_media_release();
 }
 {
  reset_case();reset_ui();auto m=fixture();begin(m);lcd_media_note_user_input();
  check(g_MEDIA_rx&&!lcd_media_replay_cancelled(),"user wake never cancels a SAVE holding the only current capture");
  JsonDocument a;deserializeJson(a,abort_json(m));lcd_MEDIA_uart(a);
 }
 for(bool replay:{false,true}){
  reset_case();reset_ui();auto m=fixture();if(replay){commit(m);fetch(m);}else begin(m);
  gesture();check(listening==0&&on_it==0&&toasts==1,"busy gesture rejected before Listening and On it");
  check(queue_items.empty()&&!ship_ai_touch_active&&!ship_ai_touch_blocked,"busy gesture is consumed through release without deferred voice edges");
  tick_ms+=31000;ship_service_voice_end_resend(tick_ms);drain_queue();
  check(delivered.empty()&&queue_items.empty(),"voice START/END cannot collapse after a 30 second transport");
  if(replay)check(lcd_media_replay_cancelled(),"voice press requests replay cancellation");
  else check(!lcd_media_replay_cancelled()&&g_MEDIA_rx,"SAVE retains live payload custody");
  JsonDocument abort;deserializeJson(abort,abort_json(m));lcd_MEDIA_uart(abort);
 }
 {
  reset_case();reset_ui();gesture();check(listening==1&&on_it==1,"idle voice gesture retains Listening and On it");
  check(queue_items.size()==2,"admitted idle gesture queues both edges");drain_queue();
  check(delivered.size()==2&&delivered[0]=="INPUT_LONG_PRESS_START"&&delivered[1]=="INPUT_LONG_PRESS_END","normal START and END order is preserved");
  check(!g_lcd_media_voice_gesture&&g_lcd_media_queued_intents==0,"normal gesture and queue release admission ownership");
 }
 {
  reset_case();reset_ui();press();check(listening==1,"idle press admits listening before held threshold");
  check(!lcd_media_try_claim(true,false),"live admitted gesture prevents replay starting before START edge");
  tick_ms+=100;release_touch();check(on_it==0&&!g_lcd_media_voice_gesture,"short press releases gesture without false On it");
 }
 {
  reset_case();reset_ui();press();queue_full=true;tick_ms+=500;held();tick_ms+=100;release_touch();
  check(on_it==0&&queue_items.empty()&&!g_lcd_media_voice_gesture&&g_lcd_media_queued_intents==0,"queue-full START does not acknowledge voice or leak intent");
 }
 {
  reset_case();reset_ui();auto m=fixture();begin(m);camera();
  check(scans==0&&toasts==1&&queue_items.empty()&&g_MEDIA_rx,"SAVE refuses camera before local capture UI and preserves current bytes");
  JsonDocument abort;deserializeJson(abort,abort_json(m));lcd_MEDIA_uart(abort);
 }
 {
  reset_case();reset_ui();auto m=fixture();commit(m);
  enqueue_hook=[](){check(!lcd_media_try_claim(true,false),"queue intent publication precedes xQueueSend interleaving");};camera();
  check(scans==1&&queue_items.size()==1,"camera enters real queue");
  JsonDocument f;f["type"]="MEDIA_SPOOL_FETCH";f["MEDIA_schema"]=1;f["owner_id"]=m.owner_id;f["device_id"]=m.device_id;f["request_id"]=m.request_id;
  // Wire operation strings are uppercase; schema remains lowercase.
  f["type"]="FETCH_TYPE";lcd_MEDIA_uart(f);auto r=last_reply();
  check(r["ok"]==0&&r["reason"]=="busy"&&!g_MEDIA_tx_pending,"queued camera refuses replay admission");
  proof_ready=false;drain_queue();check(deferred_ring_count==1,"actual UART drain preserves camera awaiting fresh awake proof");
  lcd_MEDIA_uart(f);r=last_reply();check(r["reason"]=="busy"&&!g_MEDIA_tx_pending,"deferred genuine camera also refuses replay");
 }
 {
  reset_case();reset_ui();auto m=fixture();commit(m);fetch(m);camera();
  check(queue_items.size()==1&&scans==1&&lcd_media_replay_cancelled(),"camera retained in actual queue requests active replay yield");
  drain_queue();check(delivered.empty()&&queue_items.size()==1,"camera remains queued while replay owns binary mode");
  output_frames.clear();senseSerial.output.clear();json_pins.clear();peer=Peer::Accept;lcd_MEDIA_send_file();
  check(pin_count==0&&g_suppress_uart_json_tx&&g_MEDIA_waiting_abort,"foreground cancellation closes file and preserves JSON quarantine");
  check(!output_frames.empty()&&output_frames.back().type==MSG_IMG_NACK,"foreground replay cancellation emits NACK");
  check(retained(m),"foreground yield retains original committed SD object");drain_queue();check(delivered.empty(),"no ordinary camera before peer mode proof");
  auto wrong=fixture(2);JsonDocument a;deserializeJson(a,abort_json(wrong));lcd_MEDIA_uart(a);
  check(g_MEDIA_waiting_abort&&g_suppress_uart_json_tx,"wrong request cannot clear foreground quarantine");
  deserializeJson(a,abort_json(m));lcd_MEDIA_uart(a);auto r=last_reply();
  check(r["type"]=="ABORT_TYPE"&&r["json_ready"]==true&&released(),"bound ABORT proves JSON mode and releases ownership");
  check(!json_pins.empty()&&json_pins.back()==0,"bound ABORT ACK serialized only after file close");
  drain_queue();check(delivered.size()==1&&delivered[0]=="INPUT_MENU_SELECT"&&queue_items.empty(),"retained camera delivered exactly once after mode proof");
 }
 {
  reset_case();reset_ui();auto m=fixture();commit(m);fetch(m);peer=Peer::DropChunk;
  chunk_hook=[](){camera();};output_frames.clear();senseSerial.output.clear();json_pins.clear();
  const auto started=tick_ms;lcd_MEDIA_send_file();
  check(tick_ms-started<3000&&pin_count==0&&g_MEDIA_waiting_abort,"mid-chunk foreground cancellation is prompt and closes FILE");
  check(output_frames.size()>=2&&output_frames.front().type==MSG_IMG_CHUNK&&output_frames.back().type==MSG_IMG_NACK,"mid-chunk cancellation sends NACK after real data frame");
  check(retained(m)&&delivered.empty(),"mid-chunk cancel retains SD and ordinary queue");
  JsonDocument a;deserializeJson(a,abort_json(m));lcd_MEDIA_uart(a);drain_queue();check(delivered.size()==1,"mid-chunk camera runs only after bound abort");
 }
 {
  reset_case();reset_ui();auto m=fixture();commit(m);fetch(m);peer=Peer::Accept;
  frame_hook=[](uint8_t type){if(type==MSG_IMG_END)camera();};
  output_frames.clear();lcd_MEDIA_send_file();
  check(g_MEDIA_waiting_abort&&g_suppress_uart_json_tx&&pin_count==0&&retained(m),"final END ACK proves payload only; missing ABORT keeps quarantine and SD");
  check(queue_items.size()==1&&output_frames.back().type==MSG_IMG_END,"camera arriving at END stays queued while successful payload waits mode proof");
  drain_queue();check(delivered.empty(),"no queued input escapes final-ACK to raw-RX cleanup race");
  frame_hook=nullptr;json_hook=[](){check(pin_count==0&&g_suppress_uart_json_tx&&g_MEDIA_waiting_abort,"ABORT ACK is serialized after close but before ordinary JSON release");};
  JsonDocument a;deserializeJson(a,abort_json(m));lcd_MEDIA_uart(a);json_hook=nullptr;
  drain_queue();check(released()&&delivered.size()==1,"late exact ABORT resumes final-ACK queued camera once");
 }
 {
  reset_case();reset_ui();auto m=fixture();commit(m);fetch(m);camera();peer=Peer::Accept;auto_abort=true;
  const auto start=tick_ms;lcd_MEDIA_send_file();
  check(tick_ms-start<1000&&released()&&retained(m),"prompt matching ABORT closes foreground yield without waiting full cleanup deadline");
  drain_queue();check(delivered.size()==1,"prompt cleanup still preserves camera once");
 }
 for(bool wrap:{false,true}) {
  reset_case();reset_ui();auto m=fixture();commit(m);fetch(m);peer=Peer::Accept;
  if(wrap){tick_ms=0xfffff000u;g_MEDIA_started_ms=tick_ms;g_MEDIA_last_frame_ms=tick_ms;}
  const uint32_t original_started=g_MEDIA_started_ms;const auto original_disk=disk_snapshot();lcd_MEDIA_send_file();camera();
  tick_ms=original_started+LCD_MEDIA_XFER_MS-1;lcd_MEDIA_quarantine_tick();
  check(g_spool_tx_active&&g_MEDIA_waiting_abort,"missing ABORT retains sleep custody until original transfer deadline");
  tick_ms=original_started+LCD_MEDIA_XFER_MS;lcd_MEDIA_quarantine_tick();
  check(!g_spool_tx_active&&g_MEDIA_waiting_abort&&g_suppress_uart_json_tx&&g_lcd_media_mode==LCD_MEDIA_REPLAY,"original90s retirement clears only active sleep custody, keeps mode and JSON quarantine");
  check(g_MEDIA_started_ms==original_started&&disk_snapshot()==original_disk&&pin_count==0,"retirement never renews deadline or deletes retained media");
  drain_queue();check(delivered.empty()&&queue_items.size()==1,"retired sleep ownership still cannot dispatch ordinary input");
  gesture();check(listening==0&&on_it==0,"quarantine retirement does not admit false voice feedback");
  JsonDocument a;auto wrong=fixture(2);deserializeJson(a,abort_json(wrong));lcd_MEDIA_uart(a);
  check(g_MEDIA_waiting_abort&&g_suppress_uart_json_tx,"late wrong ABORT does not release retired quarantine");
  deserializeJson(a,abort_json(m));lcd_MEDIA_uart(a);drain_queue();
  check(released()&&delivered.size()==1,"late exact ABORT safely releases quarantine after deadline and timer wrap");
 }
 {
  reset_case();reset_ui();auto m=fixture();ota_busy=true;begin(m);
  check(!g_MEDIA_rx&&g_lcd_media_mode==LCD_MEDIA_IDLE,"existing OTA guard refuses storage before foreground claim");
  ota_busy=false;camera();g_lcd_ota_binary_mode=true;drain_queue();check(delivered.empty()&&queue_items.size()==1,"existing OTA binary guard retains queued ordinary camera");
 }
 std::printf("%s %u actual-source MEDIA foreground/transport checks (%u failures)\n",failures?"FAIL":"PASS",checks,failures);
 return failures?1:0;
}
'''


def run(root, media, negative, compiler):
    source=harness(root,media,negative)
    source=source.replace('FETCH_TYPE',media.upper()+'_SPOOL_FETCH').replace('ABORT_TYPE',media.upper()+'_XFER_ABORT_ACK')
    aj=root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    with tempfile.TemporaryDirectory(prefix='halo-media-foreground-') as td:
        work=Path(td);(work/'main.cpp').write_text(source)
        built=subprocess.run([compiler,'-std=c++17','-Wno-deprecated-declarations','-I',str(root),'-I',str(aj),str(work/'main.cpp'),'-o',str(work/'test')],capture_output=True,text=True)
        if built.returncode: raise RuntimeError('compile failed (never an accepted negative control):\n'+built.stderr)
        result=subprocess.run([str(work/'test')],cwd=work,capture_output=True,text=True,timeout=30)
    print(result.stdout+result.stderr,end='')
    if negative:
        okay=result.returncode==1 and 'FAIL busy gesture rejected before Listening and On it' in result.stderr and 'REPRODUCED old START/END delivered in same drain after 30 seconds' in result.stdout
    else: okay=result.returncode==0
    return {'media':media,'negative_control':negative,'source_root':str(root),'passed':okay,'returncode':result.returncode,'stdout':result.stdout,'stderr':result.stderr}


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-root',type=Path,default=ROOT);p.add_argument('--negative-source-root',type=Path);p.add_argument('--out',type=Path)
    args=p.parse_args();compiler=shutil.which('clang++') or shutil.which('g++')
    results=[run(args.source_root.resolve(),m,False,compiler) for m in ('voice','image')]
    if args.negative_source_root:results.extend(run(args.negative_source_root.resolve(),m,True,compiler) for m in ('voice','image'))
    paths=['LCD_Minimal/lcd_media_foreground.h','LCD_Minimal/LCD_Minimal.ino','LCD_Minimal/lcd_ship_action.h','LCD_Minimal/lcd_ship_screens.h','LCD_Minimal/lcd_uart.h','LCD_Minimal/lcd_uart_task.h','LCD_Minimal/lcd_voice_spool.h','LCD_Minimal/lcd_image_spool.h']
    receipt={'status':'PASS' if all(r['passed'] for r in results) else 'FAIL','scope':__doc__,'results':results,'source_sha256':{s:hashlib.sha256((args.source_root/s).read_bytes()).hexdigest() for s in paths},'test_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    if args.out:args.out.parent.mkdir(parents=True,exist_ok=True);args.out.write_text(json.dumps(receipt,indent=2)+'\n')
    raise SystemExit(0 if receipt['status']=='PASS' else 1)
if __name__=='__main__':main()
