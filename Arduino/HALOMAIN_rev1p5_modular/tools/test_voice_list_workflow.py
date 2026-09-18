#!/usr/bin/env python3
"""Exercise actual fresh-voice/list scheduling and immutable upload custody.

Compiles production admission, cancellation, voice transport and worker branches
against the existing deterministic SDK/storage boundaries. No device or network.
The optional mutation must reach its named behavior assertion; an unrelated
crash, compilation failure or sanitizer report never counts as a negative pass.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import signal
import subprocess

import test_fresh_upload_user_priority as fresh

ROOT = Path(__file__).resolve().parents[1]


def harness(root, negative=False):
    code = fresh.harness(root).split('int main(){', 1)[0]
    replace = fresh.corners.replace_once
    definition = fresh.corners.network.definition
    main = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    ops = (root / 'Sense_Minimal/sense_op_queue.h').read_text()
    activity = (root / 'Sense_Minimal/sense_user_activity.h').read_text()
    # Inline the exact header so the regression mutation can remove only the
    # newly observed wire event, without modifying repository/source inputs.
    code = replace(code, '#include "Sense_Minimal/sense_user_activity.h"', activity)
    code = replace(code, 'static struct {bool active=false;uint32_t job_id=0;} current_job;', '')
    code = replace(code,
        'static bool foreground_priority_active(uint32_t,const char** reason){if(reason)*reason=foreground_reason;return foreground_active;}',
        r'''
static OpJob current_job={};
static bool scan_ui_inflight=false,g_list_screen_active=false;
static bool provisioning=false,ota_pending=false,ota_transfer=false;
static std::atomic<bool> g_camera_radio_off_owned{false};
static uint32_t last_input_wake_ms=0,last_user_activity_ms=0,last_lcd_communication=0;
static std::vector<UploadJob> media_queue;
static std::vector<OpJob> operation_queue;
static void* upload_queue=&media_queue;
static void* op_queue=&operation_queue;
static unsigned uxQueueMessagesWaiting(void* q){return q==upload_queue?media_queue.size():operation_queue.size();}
static int xQueuePeek(void* q,UploadJob* job,int wait){assert(q==upload_queue&&wait==0);
 if(media_queue.empty())return 0;*job=media_queue.front();return pdTRUE;}
static int xQueuePeek(void* q,OpJob* job,int wait){assert(q==op_queue&&wait==0);
 if(operation_queue.empty())return 0;*job=operation_queue.front();return pdTRUE;}
''' + definition(ops, 'static bool foreground_priority_active('))
    code = replace(code, 'static uint32_t upload_queue_count(){return 0;}',
                   'static uint32_t upload_queue_count(){return media_queue.size();}')
    code = replace(code, 'static void camera_dma_reserve_release(const char*){++dma_releases;}',
                   'static void camera_dma_reserve_release(const char*){assert(semaphore_held);++dma_releases;}')
    code = replace(code, 'static void camera_dma_reserve_acquire(const char*){++dma_acquires;}',
                   'static void camera_dma_reserve_acquire(const char*){assert(semaphore_held);++dma_acquires;}')
    code = replace(code, 'struct HTTPClient {', 'static uint32_t http_read_timeout=0;\nstruct HTTPClient {')
    code = replace(code, 'void setTimeout(uint32_t){}', 'void setTimeout(uint32_t ms){http_read_timeout=ms;}')
    code = replace(code, 'assert(!strcmp(reason,"voice_post_fail")||',
                   'assert(!strcmp(reason,"network_or_clock_pending")||!strcmp(reason,"voice_post_fail")||')
    code += r'''
#define HALO_SENSE_PROD_WRAPPER 1
#define HALO_DEFER_UPLOADS_TO_SLEEP 1
static std::atomic<uint32_t> g_voice_list_attempted_job{0};
static std::atomic<bool> g_voice_list_refresh_pending{false},list_transport_refresh_deferred{false};
static bool halo_provisioning_active(){return provisioning;}
static bool halo_prod_boot_ota_pending(){return ota_pending;}
static bool lcd_ota_in_progress(){return ota_transfer;}
static unsigned long g_upload_hold_since_ms=0;
static const unsigned long UPLOAD_HOLD_MAX_MS=600000;
static const uint32_t UPLOAD_HOLD_HIGHWATER=8;
'''
    for signature in ['static bool voice_list_job_eligible(', 'static bool voice_list_pending() {',
                      'static bool uploads_held_for_session(']:
        code += definition(main, signature) + '\n'
    code += r'''
static char g_last_refresh_reason[64]={};
static bool list_refresh_inflight=false;
static unsigned long list_refresh_start_ms=0,list_refresh_cooldown_until_ms=0,list_last_fetch_ok_ms=0;
static constexpr uint32_t LIST_REFRESH_TIMEOUT_MS=15000,LIST_REFRESH_COOLDOWN_MS=3000;
static unsigned refresh_enqueues=0,status_sends=0,cached_sends=0;
static uint32_t get_next_msg_id(){return 100+refresh_enqueues;}
static bool enqueue_op_job(const OpJob& job,bool front,const char*){
 assert(!front&&job.type==OP_LIST_REFRESH&&job.pri==PRI_BG);
 operation_queue.push_back(job);++refresh_enqueues;return true;
}
static void uart_send_ui_status(const char*){++status_sends;}
static void uart_send_ui_list(){++cached_sends;}
'''
    code += definition(main, 'static void request_list_refresh(const char* reason, bool send_status) {') + '\n'
    followup = definition(main, 'if (g_voice_list_followup.exchange(false)')
    pending = definition(main, 'if ((g_voice_list_refresh_pending.load() || list_transport_refresh_deferred.load())')
    code += 'static void actual_followup_tick(){\n' + followup + '\n' + pending + '\n}\n'
    worker = definition(main, 'static void upload_worker_task(')
    offline = definition(worker, '      if (!wifi_is_connected() || !sense_time_has_fresh_sync()) {')
    code += '#define free worker_free\nstatic bool actual_worker_offline(const UploadJob& job){do{\n'
    code += offline + '\nreturn true;\n}while(false);return false;}\n#undef free\n'
    anim = (root / 'LCD_Minimal/lcd_anim.h').read_text()
    ui = (root / 'LCD_Minimal/lcd_ui_task.h').read_text()
    menu = (root / 'LCD_Minimal/lcd_menu.h').read_text()
    for callback in ('static void knob_left_cb(', 'static void knob_right_cb('):
        producer = definition(menu, callback)
        assert 'EVT_SCROLL_DELTA' in producer and 'xQueueSendFromISR(app_event_queue, &evt,' in producer
    scroll = definition(ui, 'if (evt.type == EVT_SCROLL_DELTA) {')
    scroll_prefix = scroll[:scroll.index('        if (scroll_wake_context ||')]
    assert 'ensure_awake_for_ui("scroll_evt");' in scroll_prefix
    admission_line = next(line.strip() for line in main.splitlines()
                          if 'sense_note_admitted_user_action(sense_voice_list_input_compatible(type,' in line)
    code += 'static void actual_sense_input(const char* type){' + admission_line + '}\n'
    code += r'''
namespace lcd_scroll_producer {
static unsigned wake_notices=0,local_activity=0;
static std::vector<std::string> wire_types;
struct tx_msg_t{char type[24];};
static bool uart_tx_enqueue(const tx_msg_t* msg,const char*){
 ++wake_notices;wire_types.emplace_back(msg->type);actual_sense_input(msg->type);return true;
}
static void lcd_media_note_user_input(){++local_activity;}
static void lcd_timer_receiver_wait_release(const char*){}
static void lcd_rearm_sense_wake_for_user(uint32_t){}
static void lcd_allow_visible_ui(const char*){}
static void sleep_fallback_reset(const char*){}
static void cancel_pending_sleep_for_user_input(const char*){}
static bool g_ota_mode_active=false,g_lcd_maintenance_headless=false;
static bool g_in_light_sleep=false,g_sleep_transition=false,g_idle_screen_dark=false;
static bool g_panel_enabled=true,g_lvgl_running=true;
static int g_backlight_duty=255;
static void lcd_clear_maintenance_state(const char*,bool){}
static void lcd_exit_ota_mode(const char*){}
static void lcd_set_backlight_binary(bool,const char*){}
static void lcd_panel_set_power(bool){}
static bool lv_is_initialized(){return true;}
static void* lv_scr_act(){return nullptr;}
static void lv_obj_invalidate(void*){}
static void* app_event_queue=nullptr;
static struct{int count=0;}g_active;
enum{EVT_SCROLL_DELTA,EVT_RENDER_ACTIVE_LIST,EVT_RESET_UI};
struct app_event_t{int type=EVT_SCROLL_DELTA;struct{int scroll_delta=1,new_count=0;}data;};
static int xQueueSend(void*,app_event_t*,unsigned){return 1;}
static bool provisioning_input_locked(){return false;}
static void provision_ui_handle_scroll(int){}
static void resetActivityTimer(){}
static void lv_timer_handler(){}
static void example_lvgl_unlock(){}
static void user_activity_bump(const char*){}
'''
    code += definition(anim, 'static void lcd_media_user_wake(') + '\n'
    code += definition(anim, 'static void ensure_awake_for_ui(') + '\n'
    # Execute the actual UI task's scroll path through its wake call. Rendering
    # beyond that call and RTOS delivery are explicitly outside this test.
    code += 'static void actual_scroll_event(){app_event_t evt{};do{\n' + scroll_prefix
    code += '\n}\n}while(false);}\n}\n'
    if negative:
        # Generated host code only: restore the old all-input cancellation
        # decision for the list-compatible scope without changing firmware.
        if negative == 'missing_activity':
            compatible = definition(code, 'static bool sense_voice_list_input_compatible(')
            old = replace(compatible, 'return list_active &&',
                          'return (!type || strcmp(type,"INPUT_USER_ACTIVE")) && list_active &&')
            code = replace(code, compatible, old)
        else:
            # Keep the real accessor declaration intact; mutate only the
            # three scope/cancellation decisions back to all-input semantics.
            declaration = definition(code, 'static uint32_t sense_user_interrupt_generation(')
            code = replace(code, declaration, 'USER_INTERRUPT_ACCESSOR')
            assert code.count('sense_user_interrupt_generation()') == 3
            code = code.replace('sense_user_interrupt_generation()', 'sense_user_action_generation()')
            code = replace(code, 'USER_INTERRUPT_ACCESSOR', declaration)
    return code + r'''
static void workflow_reset(std::vector<int> codes={202}){
 fresh_reset(codes);g_sense_user_action_generation=17;
 media_queue.clear();operation_queue.clear();current_job={};scan_ui_inflight=false;
 g_list_screen_active=true;provisioning=ota_pending=ota_transfer=false;
 g_camera_radio_off_owned=false;last_input_wake_ms=last_user_activity_ms=last_lcd_communication=0;
 g_voice_list_attempted_job=0;g_voice_list_refresh_pending=false;g_voice_list_followup=false;
 list_transport_refresh_deferred=false;g_upload_hold_since_ms=0;
 list_refresh_inflight=false;list_refresh_start_ms=list_refresh_cooldown_until_ms=list_last_fetch_ok_ms=0;
 refresh_enqueues=status_sends=cached_sends=0;
 http_read_timeout=0;
 // Do not assume interrupt generation starts at zero: scope snapshots must
 // work after earlier foreground gestures, including counter rollover.
}
static void same_job(const UploadJob& expected,const UploadJob& actual){
 assert(expected.job_id==actual.job_id&&expected.image_buf==actual.image_buf);
 assert(expected.image_len==actual.image_len&&expected.is_voice==actual.is_voice);
 assert(!memcmp(&expected.voice,&actual.voice,sizeof(expected.voice)));
}
int main(){
 unsigned scenarios=0;auto job=fixture();
 // Physical encoder events and the debug-scroll event reach this actual UI
 // path. Its wake helper emits INPUT_USER_ACTIVE once even when an independent
 // INPUT_SCROLL is absent/suppressed; use the produced type, never assume it.
 workflow_reset();bool lcd_activity_preserved_voice=false;
 boundary_hook=[&](const char* at){if(!strcmp(at,"post_enter")){
  lcd_scroll_producer::actual_scroll_event();
  assert(lcd_scroll_producer::wire_types.size()==1&&lcd_scroll_producer::wire_types[0]=="INPUT_USER_ACTIVE");
  lcd_activity_preserved_voice=!media_retry_network_cancelled();assert(lcd_activity_preserved_voice);
  for(unsigned i=0;i<50;++i)lcd_scroll_producer::actual_scroll_event();
  assert(lcd_scroll_producer::wake_notices==1&&lcd_scroll_producer::local_activity==51);
 }};
 {MediaRetryNetworkScope scope(job,true);assert(voice_upload_and_parse(job));actual_worker_voice_dispatch(job,true);}
 assert(lcd_activity_preserved_voice&&delivered==1&&!upload_worker_parked_pending());released();++scenarios;
 workflow_reset();bool capture_requested=false;
 boundary_hook=[&](const char* at){if(!strcmp(at,"post_enter")){
  actual_sense_input(lcd_scroll_producer::wire_types[0].c_str());assert(!media_retry_network_cancelled());
 }else if(!strcmp(at,"sdk_write_done")){
  actual_sense_input("INPUT_MENU_SELECT");capture_requested=true;
 }};
 {MediaRetryNetworkScope scope(job,true);assert(!voice_upload_and_parse(job));actual_worker_voice_dispatch(job,false);}
 assert(capture_requested&&upload_worker_parked_pending()&&persisted==0&&worker_freed==0);
 same_job(job,upload_worker_parked_job);released();++scenarios;
 puts("PASS actual LCD scroll wake emits compatible INPUT_USER_ACTIVE; a following capture still cancels and retains voice custody");
 for(const char* type:{"INPUT_SCROLL","INPUT_WAKE","INPUT_TOUCH","INPUT_MENU_SELECT",
                      "INPUT_LONG_PRESS_START","INPUT_DELETE","INPUT_OTA_CHECK","INPUT_RESET_WIFI",
                      "INPUT_WIFI_SCAN","INPUT_USER_ACTIVE"}){
  workflow_reset();bool compatible=!strcmp(type,"INPUT_SCROLL")||!strcmp(type,"INPUT_WAKE")||!strcmp(type,"INPUT_TOUCH")||!strcmp(type,"INPUT_USER_ACTIVE");
  assert(sense_voice_list_input_compatible(type,true)==compatible);
  assert(!sense_voice_list_input_compatible(type,false));
  const auto before=sense_user_interrupt_generation();
  sense_note_admitted_user_action(sense_voice_list_input_compatible(type,g_list_screen_active));
  assert(sense_user_action_generation()==18);
  assert(sense_user_interrupt_generation()==before+(compatible?0:1));++scenarios;
 }
 assert(!sense_voice_list_input_compatible(nullptr,true));
 for(unsigned blocked=0;blocked<13;++blocked){
  workflow_reset();media_queue={job};
  switch(blocked){case 0:g_list_screen_active=false;break;case 1:connected=false;break;
   case 2:age_ok=false;break;case 3:foreground_active=true;break;case 4:scan_ui_inflight=true;break;
   case 5:dish_scan_inflight=true;break;case 6:voice_recording_active=true;break;
   case 7:g_camera_radio_off_owned=true;break;case 8:current_job.active=true;current_job.pri=PRI_USER;break;
   case 9:operation_queue.push_back({OP_VOICE,PRI_USER});break;
   case 10:provisioning=true;break;case 11:ota_pending=true;break;case 12:ota_transfer=true;break;}
  assert(!voice_list_job_eligible(job)&&!voice_list_pending());
  assert(uploads_held_for_session(nullptr)&&media_queue.size()==1&&media_queue.front().image_buf==job.image_buf);++scenarios;
 }
 workflow_reset();media_queue={job};
 assert(voice_list_job_eligible(job)&&voice_list_pending()&&!uploads_held_for_session(nullptr));
 http_inflight=true;assert(uploads_held_for_session(nullptr));http_inflight=false;
 auto image_job=job;image_job.is_voice=false;image_job.job_id=8;
 media_queue={image_job,job};assert(!voice_list_pending()&&uploads_held_for_session(nullptr));
 assert(media_queue.size()==2&&media_queue.front().job_id==8&&media_queue.back().job_id==job.job_id);
 media_queue={job};assert(upload_worker_park_job(image_job,"old_image","fixture"));
 assert(!voice_list_pending()&&uploads_held_for_session(nullptr));
 UploadJob oldest{};assert(upload_worker_take_parked_job(oldest));same_job(image_job,oldest);
 assert(upload_worker_park_job(job,"voice","fixture")&&voice_list_pending()&&!uploads_held_for_session(nullptr));
 media_queue.clear();http_inflight=true;assert(uploads_held_for_session(nullptr));http_inflight=false;
 g_voice_list_attempted_job=job.job_id;
 assert(!voice_list_pending()&&uploads_held_for_session(nullptr));
 g_upload_flush_requested=true;assert(!uploads_held_for_session(nullptr));++scenarios;
 puts("PASS actual list-only admission preserves all urgent owners, parked precedence and FIFO image backlog");
 workflow_reset();last_input_wake_ms=last_user_activity_ms=last_lcd_communication=millis();
 assert(foreground_priority_active(millis(),nullptr));
 {MediaRetryNetworkScope scope(job,true);assert(!foreground_priority_active(millis(),nullptr));
  voice_recording_active=true;assert(foreground_priority_active(millis(),nullptr));voice_recording_active=false;
  scan_ui_inflight=true;assert(foreground_priority_active(millis(),nullptr));}
 released();++scenarios;
 for(const char* event:{"lock_wait","start","http_begin","post_enter","sdk_connect",
                       "sdk_write_enter","sdk_write_done","post_status","reply_size",
                       "reply_body","ack_hash_enter","ack_hash_done"}){
  workflow_reset();const auto initial_interrupt=sense_user_interrupt_generation();
  unsigned injected=0;
  boundary_hook=[event,&injected](const char* at){if(!strcmp(event,at))
   for(unsigned i=0;i<100;++i){sense_note_admitted_user_action(true);++injected;}};
  {MediaRetryNetworkScope scope(job,true);
   assert(media_voice_list_active()&&media_voice_list_remaining_ms()==12000);
   assert(voice_upload_and_parse(job));
   actual_worker_voice_dispatch(job,true);
   assert(!media_retry_network_cancelled());
  }
  assert(injected>=100&&sense_user_action_generation()==17+injected);
  assert(sense_user_interrupt_generation()==initial_interrupt);
  assert(posts==1&&delivered==1&&persisted==0&&worker_freed==1&&resets==0);
  assert(!upload_worker_parked_pending());released();++scenarios;
 }
 puts("PASS repeated compatible list input at twelve actual transport boundaries cannot starve fresh voice");
 for(const char* event:{"lock_wait","http_begin","sdk_connect","sdk_write_done",
                       "post_status","reply_body","ack_hash_done"}){
  workflow_reset();input_at(event);
  {MediaRetryNetworkScope scope(job,true);
   assert(!voice_upload_and_parse(job));actual_worker_voice_dispatch(job,false);
  }
  assert(upload_worker_parked_pending()&&persisted==0&&worker_freed==0&&deleted==0);
  UploadJob resumed{};assert(upload_worker_take_parked_job(resumed));same_job(job,resumed);
  released();boundary_hook={};statuses.push_back(202);
  {MediaRetryNetworkScope next(resumed,true);
   assert(!media_retry_network_cancelled());assert(voice_upload_and_parse(resumed));
   actual_worker_voice_dispatch(resumed,true);
  }
  assert(delivered==1&&persisted==0&&!upload_worker_parked_pending());released();++scenarios;
 }
 puts("PASS urgent input cancels actual I/O, parks one exact descriptor and later resumes the same frozen request");
 for(unsigned origin=0;origin<5;++origin){
  workflow_reset();auto ordinary=job;
  ordinary.is_voice=origin!=1;ordinary.from_voice_sd=origin==2;
  ordinary.from_image_sd=origin==3;ordinary.from_persisted=origin==4;
  {MediaRetryNetworkScope scope(ordinary,origin!=0);
   assert(!media_voice_list_active());sense_note_admitted_user_action(true);
   assert(media_retry_network_cancelled());
  }
  released();++scenarios;
 }
 puts("PASS ordinary fresh mode, images and every saved origin retain existing all-input cancellation");
 for(uint32_t start:{0U,1000U,UINT32_MAX-6000U}){
  workflow_reset();clock_ms=start;
  {MediaRetryNetworkScope scope(job,true);
   assert(media_voice_list_remaining_ms()==12000);
   clock_ms=start+11999;sense_note_admitted_user_action(true);
   assert(media_voice_list_remaining_ms()==1&&!media_retry_network_cancelled());
   clock_ms=start+12000;
   assert(media_voice_list_remaining_ms()==0&&media_retry_network_cancelled());
  }
  released();++scenarios;
 }
 puts("PASS original 12s scope deadline cannot renew on scrolling and expires across millis rollover");
 for(const char* event:{"upload_ok","http_end","done"}){
  workflow_reset();input_at(event);
  {MediaRetryNetworkScope scope(job,true);bool accepted=voice_upload_and_parse(job);
   assert(accepted);actual_worker_voice_dispatch(job,accepted);
  }
  assert(delivered==1&&worker_freed==1&&!upload_worker_parked_pending()&&persisted==0);
  released();++scenarios;
 }
 puts("PASS interruption after the validated acceptance decision cannot repark or duplicate completed custody");
 workflow_reset();media_queue={job};
 for(unsigned i=0;i<100;++i)request_list_refresh("user_refresh",true);
 assert(g_voice_list_refresh_pending&&refresh_enqueues==0&&operation_queue.empty());
 g_voice_list_attempted_job=job.job_id;media_queue.clear();
 {MediaRetryNetworkScope scope(job,true);
  for(unsigned i=0;i<100;++i){request_list_refresh("scroll_refresh",true);sense_note_admitted_user_action(true);}
  assert(refresh_enqueues==0&&!media_retry_network_cancelled());
  assert(voice_upload_and_parse(job));actual_worker_voice_dispatch(job,true);
  actual_followup_tick();assert(refresh_enqueues==0);
 }
 released();actual_followup_tick();assert(refresh_enqueues==1&&list_refresh_inflight&&operation_queue.size()==1);
 for(unsigned i=0;i<100;++i){request_list_refresh("repeated_refresh",true);actual_followup_tick();}
 assert(refresh_enqueues==1&&operation_queue.size()==1&&!g_voice_list_refresh_pending);++scenarios;
 puts("PASS two hundred refreshes coalesce into one real request after voice cleanup, with no concurrent TLS dispatch");
 workflow_reset();g_voice_list_followup=true;g_list_screen_active=false;
 actual_followup_tick();assert(refresh_enqueues==0&&!g_voice_list_refresh_pending);++scenarios;
 for(bool no_clock:{false,true}){
  workflow_reset();connected=no_clock;age_ok=!no_clock;
  {MediaRetryNetworkScope scope(job,true);assert(!actual_worker_offline(job));}
  assert(persisted==1&&worker_freed==1&&posts==0&&!upload_inflight);same_job(job,retained);released();++scenarios;
 }
 for(uint32_t start:{1000U,UINT32_MAX-6000U}) for(bool simultaneous_input:{false,true}){
  workflow_reset();clock_ms=start;
  boundary_hook=[start,simultaneous_input](const char* at){if(!strcmp(at,"sdk_write_done")){
   clock_ms=start+12000;if(simultaneous_input)sense_note_admitted_user_action();}};
  {MediaRetryNetworkScope scope(job,true);assert(!voice_upload_and_parse(job));
   assert(media_voice_list_timed_out());actual_worker_voice_dispatch(job,false);}
  assert(resets==0);
  if(simultaneous_input){assert(persisted==0&&worker_freed==0&&upload_worker_parked_pending());
   same_job(job,upload_worker_parked_job);}
  else {assert(persisted==1&&worker_freed==1&&!upload_worker_parked_pending());same_job(job,retained);}
  released();++scenarios;
 }
 for(int status:{-1,503,400}){
  workflow_reset({status});
  {MediaRetryNetworkScope scope(job,true);assert(!voice_upload_and_parse(job));actual_worker_voice_dispatch(job,false);}
  assert(posts==1&&resets==0&&delay_ms==0&&persisted==1&&!upload_worker_parked_pending());released();++scenarios;
 }
 puts("PASS offline, fresh-clock failure and deadline exhaustion retain exact durable custody; list mode never radio-resets or retries");
 for(uint32_t start:{1000U,UINT32_MAX-6000U}) for(bool user_interrupt:{false,true}){
  workflow_reset();clock_ms=start;semaphore_held=true;http_inflight=true;
  if(user_interrupt)cancel_delay=20;
  {MediaRetryNetworkScope scope(job,true);assert(!voice_upload_and_parse(job));
   assert(media_retry_network_cancelled());actual_worker_voice_dispatch(job,false);}
  // This owner never acquired/released the other transport's mutex or DMA.
  assert(semaphore_held&&http_inflight&&semaphore_acquires==0&&semaphore_releases==0);
  assert(posts==0&&dma_releases==0&&dma_acquires==0&&resets==0);
  if(user_interrupt){assert(delay_ms==60&&upload_worker_parked_pending()&&persisted==0);}
  else {assert(delay_ms==12000&&persisted==1&&!upload_worker_parked_pending());}
  semaphore_held=false;http_inflight=false;released();++scenarios;
 }
 workflow_reset();semaphore_held=true;http_inflight=true;const uint32_t lock_start=clock_ms;
 boundary_hook=[lock_start](const char* at){if(!strcmp(at,"lock_wait")&&uint32_t(clock_ms-lock_start)>=5000){
  semaphore_held=false;http_inflight=false;}};
 {MediaRetryNetworkScope scope(job,true);assert(voice_upload_and_parse(job));actual_worker_voice_dispatch(job,true);}
 assert(delay_ms==5000&&posts==1&&last_connect==7000&&last_handshake==7&&http_read_timeout==1500);
 assert(semaphore_acquires==1&&semaphore_releases==1&&delivered==1);released();++scenarios;
 puts("PASS actual HTTP acquisition is cancellable/deadline-bound without disturbing another owner; remaining budget reaches TLS");
 printf("PASS %u voice/list workflow assertions across %u scenarios\n",checks,scenarios);
}
'''


def execute(root, out, negative=False):
    out.mkdir(parents=True, exist_ok=False)
    cpp, exe = out / 'test.cpp', out / 'test'
    cpp.write_text(harness(root, negative))
    command = [shutil.which('clang++') or 'c++', '-std=c++17', '-pthread',
               '-Wno-deprecated-declarations', '-Wno-c++11-narrowing', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I', str(root), '-I',
               str(root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
               str(cpp), '-o', str(exe)]
    build = subprocess.run(command, capture_output=True, timeout=30,
                           preexec_fn=fresh.corners.network.no_core)
    (out / 'compile.log').write_bytes(build.stdout + build.stderr)
    if build.returncode:
        return {'compiled': False, 'exit_code': build.returncode}
    run = subprocess.run([str(exe)], capture_output=True, timeout=20,
                         preexec_fn=fresh.corners.network.no_core)
    (out / 'run.log').write_bytes(run.stdout + run.stderr)
    return {'compiled': True, 'exit_code': run.returncode,
            'stderr': run.stderr.decode(),
            'output': (run.stdout + run.stderr).decode()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--negative-control', action='store_true')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    result = {'current': execute(a.source_root, a.out / 'current'), 'hardware': False,
              'boundaries': 'SDK, network and durable storage are deterministic doubles'}
    result['pass'] = result['current']['compiled'] and result['current']['exit_code'] == 0
    if a.negative_control:
        negative = execute(a.source_root, a.out / 'negative', True)
        negative['expected_assertion'] = bool(negative['compiled'] and
            negative['exit_code'] == -signal.SIGABRT and re.fullmatch(
                r'FAIL line \d+: lcd_activity_preserved_voice\n', negative.get('stderr', '')))
        result['negative_all_input_cancellation'] = negative
        result['pass'] &= negative['expected_assertion']
        missing = execute(a.source_root, a.out / 'negative_missing_activity', 'missing_activity')
        missing['expected_assertion'] = bool(missing['compiled'] and
            missing['exit_code'] == -signal.SIGABRT and re.fullmatch(
                r'FAIL line \d+: lcd_activity_preserved_voice\n', missing.get('stderr', '')))
        result['negative_missing_lcd_wire_activity'] = missing
        result['pass'] &= missing['expected_assertion']
    paths = ['Sense_Minimal/Sense_Minimal.ino', 'Sense_Minimal/sense_ops.h',
             'Sense_Minimal/sense_media_retry.h', 'Sense_Minimal/sense_media_retry_client.h',
             'Sense_Minimal/sense_user_activity.h', 'Sense_Minimal/sense_voice.h',
             'Sense_Minimal/sense_upload.h', 'Sense_Minimal/sense_op_queue.h',
             'LCD_Minimal/lcd_anim.h', 'LCD_Minimal/lcd_ui_task.h', 'LCD_Minimal/lcd_menu.h',
             'tools/test_voice_list_workflow.py', 'tools/test_fresh_upload_user_priority.py',
             'tools/test_media_network_corner_cases.py', 'tools/test_media_network_retry.py']
    result['source_sha256'] = {path: hashlib.sha256((a.source_root / path).read_bytes()).hexdigest()
                             for path in paths}
    (a.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
