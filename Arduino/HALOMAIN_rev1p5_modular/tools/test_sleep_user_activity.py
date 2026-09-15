#!/usr/bin/env python3
"""Compile actual user-admission/dedupe code; prove retries and telemetry do not cancel sleep."""
import argparse,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def source(path,ref):
 if ref:return subprocess.check_output(['git','show',ref+':Arduino/HALOMAIN_rev1p5_modular/'+path],cwd=ROOT,text=True)
 return (ROOT/path).read_text()

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--negative-ref');args=ap.parse_args()
 ino=source('Sense_Minimal/Sense_Minimal.ino',args.negative_ref)
 start=ino.index('  bool is_user_input_message =',ino.index('static bool parse_input_message(const char* json_str) {'))
 end=ino.index('  // Drain replies from the LCD',start)
 block=ino[start:end]
 msg=source('Sense_Minimal/sense_uart_msg.h',args.negative_ref)
 dedupe=msg[msg.index('#ifndef SENSE_INPUT_SEEN_RING'):msg.index('#if HALO_SPOOL_TEST')]
 # The real admission block must remain after both protocol validation and
 # diagnostic-message handling, not become a pre-validation wake trigger.
 assert ino.index('if (!validate_protocol_message(doc))',ino.index('static bool parse_input_message(const char* json_str) {'))<start
 prefix=r'''
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include "sense_user_activity.h"
struct SerialMock {template<class... A>void printf(const char*,A...) {}} Serial;
std::vector<std::string> calls;
void cancel_pending_sleep_for_user_action(const char*) {calls.push_back("cancel");}
void sense_spool_drain_yield_to_user(){calls.push_back("yield");}
void uart_send_input_ack(uint32_t){calls.push_back("ack");}
struct Field {uint32_t value;uint32_t operator|(int)const{return value;}};
struct Doc {uint32_t id;Field operator[](const char*)const{return {id};}};
int checks=0;
void check(bool ok,const char* msg){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",msg);std::exit(2);}}
'''
 code=prefix+'\n'+dedupe+'\nbool admit(const char* type,uint32_t id){Doc doc{id};\n'+block+'\nreturn true;\n}\n'+r'''
int main(){
 (void)&sense_note_admitted_user_action; // Keep old admission compilable for the behavioral negative control.
 const char* passive[]={"INPUT_PING","INPUT_SLEEP","INPUT_SENSE_FW","INPUT_FW_INFO","INPUT_TEST_ERRORS","INPUT_MAINT_TEST","LINK_HB","LCD_DIAG","UNKNOWN","INPUT_NOT_A_COMMAND"};
 uint32_t id=100;
 for(const char* type:passive){calls.clear();auto old=sense_user_action_generation();admit(type,id++);check(sense_user_action_generation()==old,"passive frame does not advance user generation");check(std::find(calls.begin(),calls.end(),"cancel")==calls.end(),"passive frame does not cancel cleanup");}
 const char* active[]={"INPUT_WAKE","INPUT_TOUCH","INPUT_MENU_PRESS","INPUT_MENU_SELECT","INPUT_SCROLL","INPUT_DELETE","INPUT_EXPIRY_DATE","INPUT_DISCARD_OPTIONS","INPUT_LONG_PRESS_START","INPUT_LONG_PRESS_END","INPUT_RETRY","INPUT_RESET_WIFI","INPUT_OTA_CHECK","INPUT_WIFI_SCAN","INPUT_WIFI_TEST"};
 for(const char* type:active){calls.clear();auto old=sense_user_action_generation();uint32_t current=id++;admit(type,current);check(sense_user_action_generation()==old+1,"new action increments exactly once");check(calls==std::vector<std::string>({"ack","cancel","yield"}),"ACK precedes cancellation and background yield");calls.clear();admit(type,current);check(sense_user_action_generation()==old+1,"duplicate cannot renew flush generation");check(calls==std::vector<std::string>({"ack"}),"duplicate re-ACK only");}
 check(!sense_user_action_cancels_flush(nullptr),"null is not user activity");
 g_sense_user_action_generation.store(UINT32_MAX);admit("INPUT_MENU_SELECT",id++);check(sense_user_action_generation()==0,"generation wraps and still changes");
 auto before=sense_user_action_generation();admit("INPUT_MENU_SELECT",id++);admit("INPUT_MENU_SELECT",id++);check(sense_user_action_generation()==before+2,"distinct inputs in same clock tick both count");
 std::printf("PASS %d actual-source user-admission checks\n",checks);
}
'''
 code=code.replace('#include <cstdio>','#include <cstdio>\n#include <algorithm>')
 with tempfile.TemporaryDirectory(prefix='halo-user-admission-') as td:
  td=Path(td);cpp=td/'test.cpp';cpp.write_text(code)
  subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(ROOT/'Sense_Minimal'),str(cpp),'-o',str(td/'test')],check=True)
  outcome=subprocess.run([str(td/'test')])
  if args.negative_ref:
   if outcome.returncode==0:raise SystemExit('Negative control unexpectedly passed')
   print('PASS negative control reproduces unsafe baseline admission')
  elif outcome.returncode:raise SystemExit(outcome.returncode)
if __name__=='__main__':main()
