#!/usr/bin/env python3
"""Exercise actual LCD voice status handling with ArduinoJson and fake UI queues.

No hardware/network. Tests cover acknowledgment-time errors, delayed recording
identity, event-queue exhaustion and old failures racing a newer local action.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
static unsigned checks=0,failures=0;
static void check(bool okay,const char* name){++checks;if(!okay){++failures;fprintf(stderr,"FAIL %s\n",name);}}
static uint32_t millis(){return 1000;}
static struct Logger {template<class...T> void printf(const char*,T...){} void println(const char*){}} Serial;
enum {SCREEN_HOME,SCREEN_VOICE_ACK,SCREEN_AI_LISTENING,SCREEN_PROCESSING,SCREEN_SETTINGS,SCREEN_HOLD_STILL};
static int ui_screen_state=SCREEN_HOME;
static const char* ui_screen_state_name(int){return "host";}
static char g_ship_ui_phase[24]={},g_ship_ui_op[16]={},g_ship_ui_mode[24]={};
static char g_ship_ui_text[128]={},g_ship_ui_policy[24]={};
static char g_last_ui_status_phase[24]={},g_last_ui_status_op[16]={},g_last_ui_status_mode[24]={};
static uint32_t g_ship_ui_job_id=0,g_last_ui_status_msg_id=0,g_last_ui_status_job_id=0;
static uint32_t g_ship_ui_last_ms=0,g_ship_ui_finalized_job_id=0,scan_request_sent_ms=0;
static bool g_voice_fire_and_forget_ignore_ui=false,waiting_for_voice_response=false,g_ship_voice_json_pending=false;
static uint32_t voice_response_deadline_ms=0;
static bool g_ship_ui_finalized=false,g_ship_ui_busy=false,g_ship_ui_terminal=false,g_ship_ui_error=false,g_ship_ui_dirty=false;
static bool waiting_for_scan_response=false;
enum {EVT_SHIP_UI_STATUS=1};
struct Status {char op[16]={},mode[24]={},phase[24]={},text[128]={},ui_policy[24]={};uint32_t job_id=0,msg_id=0;};
struct app_event_t {int type=0;struct {Status ship_ui_status;} data;};
static std::vector<app_event_t> queued;
static void* app_event_queue=&queued;
static bool queue_full=false;
static const int pdTRUE=1;
static uint32_t pdMS_TO_TICKS(uint32_t v){return v;}
static int xQueueSend(void*,const app_event_t* evt,uint32_t){if(queue_full)return 0;queued.push_back(*evt);return pdTRUE;}
'''

TESTS = r'''
static uint32_t next_msg=1;
static void status(const char* phase,uint32_t job,bool bind=true){
  JsonDocument d;d["op"]="VOICE";d["phase"]=phase;d["text"]="Voice not saved. Please try again.";
  d["msg_id"]=next_msg++;if(bind)d["job_id"]=job;ship_menu_handle_ui_status(d);
}
static void reset(){
  queued.clear();queue_full=false;app_event_queue=&queued;g_last_ui_status_msg_id=0;
  g_ship_ui_job_id=0;g_ship_ui_op[0]=g_ship_ui_phase[0]=0;
  g_voice_fire_and_forget_ignore_ui=false;waiting_for_voice_response=false;
  g_ship_voice_json_pending=false;voice_response_deadline_ms=0;
  g_ship_ui_finalized=g_ship_ui_busy=g_ship_ui_terminal=g_ship_ui_error=g_ship_ui_dirty=false;
  g_ship_ui_finalized_job_id=0;waiting_for_scan_response=false;ui_screen_state=SCREEN_AI_LISTENING;
  ship_voice_status_begin();
}
static void acknowledgment(){
  queued.clear();g_ship_ui_dirty=false;ui_screen_state=SCREEN_VOICE_ACK;
  g_voice_fire_and_forget_ignore_ui=true;waiting_for_voice_response=true;
  voice_response_deadline_ms=9000;g_ship_voice_json_pending=true;
}
static bool error_queued(){return queued.size()==1&&!strcmp(queued.back().data.ship_ui_status.phase,"ERROR");}
int main(){
  reset();status("RECORDING",10);acknowledgment();status("ERROR",10);
  check(error_queued(),"matching failure during acknowledgment reaches UI event queue");
  check(g_ship_ui_dirty&&!strcmp(g_ship_ui_phase,"ERROR")&&g_ship_ui_job_id==10,"matching error retained in latched fallback");
  check(!g_voice_fire_and_forget_ignore_ui&&!waiting_for_voice_response&&!voice_response_deadline_ms&&!g_ship_voice_json_pending,"matching error clears only its voice wait state");

  reset();acknowledgment();status("RECORDING",11);
  check(g_ship_ui_job_id==11&&queued.empty()&&!g_ship_ui_dirty&&g_voice_fire_and_forget_ignore_ui,"late recording binds identity without replacing acknowledgment");
  status("ERROR",11);check(error_queued(),"failure follows delayed recording status");

  for(const char* phase:{"UPLOADING","PROCESSING","DONE"}){
    reset();status("RECORDING",12);acknowledgment();status(phase,12);
    check(queued.empty()&&!g_ship_ui_dirty&&ui_screen_state==SCREEN_VOICE_ACK,"success and progress preserve existing acknowledgment");
  }
  reset();status("RECORDING",13);acknowledgment();queue_full=true;status("ERROR",13);
  check(queued.empty()&&g_ship_ui_dirty&&!strcmp(g_ship_ui_phase,"ERROR"),"full event queue retains failure for normal latched apply");

  reset();status("RECORDING",14);acknowledgment();status("ERROR",99);
  check(queued.empty()&&!g_ship_ui_dirty&&g_voice_fire_and_forget_ignore_ui&&g_ship_ui_job_id==14,"old job cannot interrupt current acknowledgment");
  status("DONE",99);check(g_voice_fire_and_forget_ignore_ui&&waiting_for_voice_response,"old completion cannot clear current voice wait");

  reset();status("RECORDING",15);acknowledgment();ship_voice_status_begin();status("ERROR",15);
  check(g_ship_ui_job_id==0&&queued.empty()&&!g_ship_ui_dirty&&g_voice_fire_and_forget_ignore_ui,"new local voice retires old identity before recording status");
  status("RECORDING",16);status("ERROR",15);
  check(g_ship_ui_job_id==16&&queued.empty()&&g_voice_fire_and_forget_ignore_ui,"late old failure cannot replace newly bound recording");
  status("ERROR",16);check(error_queued(),"new recording still receives its own failure");

  reset();status("RECORDING",17);acknowledgment();
  snprintf(g_ship_ui_op,sizeof(g_ship_ui_op),"SCAN");g_ship_ui_job_id=18;waiting_for_scan_response=true;ui_screen_state=SCREEN_HOLD_STILL;
  status("ERROR",17);check(queued.empty()&&!strcmp(g_ship_ui_op,"SCAN")&&g_ship_ui_job_id==18&&waiting_for_scan_response,"old voice error leaves new scan state untouched");

  reset();status("RECORDING",19);acknowledgment();ui_screen_state=SCREEN_SETTINGS;status("ERROR",19);
  check(queued.empty()&&!g_ship_ui_dirty,"old voice error cannot replace settings");
  reset();status("RECORDING",20);acknowledgment();ui_screen_state=SCREEN_HOME;g_voice_fire_and_forget_ignore_ui=false;status("ERROR",20);
  check(error_queued(),"current voice failure can surface after acknowledgment returns home");

  reset();status("RECORDING",21);acknowledgment();status("ERROR",21,false);
  check(queued.empty()&&g_voice_fire_and_forget_ignore_ui,"unbound status is not confused with message id");
  reset();g_ship_ui_finalized=true;g_ship_ui_dirty=true;g_ship_ui_job_id=22;ship_voice_status_begin();
  check(g_ship_ui_job_id==0&&!g_ship_ui_finalized&&!g_ship_ui_dirty&&!strcmp(g_ship_ui_op,"VOICE"),"local start clears stale finalization and pending status");
  printf("%s %u actual-source LCD voice status checks (%u failures)\n",failures?"FAIL":"PASS",checks,failures);
  return failures?1:0;
}
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=ROOT)
    parser.add_argument('--negative-control',action='store_true',
                        help='Require committed HEAD handler to reproduce the suppressed-error failure')
    args=parser.parse_args();root=args.source_root.resolve()
    compiler=shutil.which('clang++') or shutil.which('g++')
    if not compiler: raise SystemExit('C++ compiler required')
    screens=(root/'LCD_Minimal/lcd_ship_screens.h').read_text()
    action=(root/'LCD_Minimal/lcd_ship_action.h').read_text()
    if args.negative_control:
        prefix=subprocess.run(['git','rev-parse','--show-prefix'],cwd=root,
                              text=True,capture_output=True,check=True).stdout.strip()
        action=subprocess.run(['git','show','HEAD:'+prefix+'LCD_Minimal/lcd_ship_action.h'],
                              cwd=root,text=True,capture_output=True,check=True).stdout
    source=PREFIX+definition(screens,'static void ship_voice_status_begin(')+'\n'
    source+=definition(action,'static void ship_menu_handle_ui_status(')+'\n'+TESTS
    aj=root/'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    with tempfile.TemporaryDirectory(prefix='halo-voice-status-') as td:
        work=Path(td);(work/'main.cpp').write_text(source)
        subprocess.run([compiler,'-std=c++17','-Wno-deprecated-declarations','-I',str(aj),str(work/'main.cpp'),'-o',str(work/'test')],check=True)
        result=subprocess.run([str(work/'test')],check=False,timeout=10,text=True,capture_output=True)
    print(result.stdout+result.stderr,end='')
    if args.negative_control:
        reproduced=result.returncode==1 and 'FAIL matching failure during acknowledgment reaches UI event queue' in result.stderr
        print('PASS committed-handler negative control' if reproduced else 'FAIL negative control did not reproduce suppression')
        raise SystemExit(0 if reproduced else 1)
    raise SystemExit(result.returncode)


if __name__=='__main__': main()
