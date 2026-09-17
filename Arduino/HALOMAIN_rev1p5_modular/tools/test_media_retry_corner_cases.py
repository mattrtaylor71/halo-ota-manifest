#!/usr/bin/env python3
"""Finite adversarial host execution of both media-arm peers and LCD user wake.

Reuses only SDK/RTOS/UI boundary doubles from the focused suites. The actual
Sense transaction, LCD UART admission, RTC adapter, timer policy, SDK-arm
function and UI/cancellation functions execute. UART delivery preserves FIFO
per direction; loss, duplicates, corruption and delay are injected explicitly.
No hardware/network access. Output directories must be new to retain failures.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def load(root, name):
    spec = importlib.util.spec_from_file_location(name, root / 'tools' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.ROOT = root
    return module


def paired_harness(root):
    base = load(root, 'test_lcd_media_retry').harness(root).split('int main(){', 1)[0]
    return base + r'''
#include <vector>
#include <algorithm>
#include "halo_common/MediaRetryPolicy.h"
static uint32_t rng=0x170c0a5e,case_number=0;
static unsigned draws=0,transactions=0,timer_cases=0;
static uint32_t random32(){++draws;rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
struct Event{uint64_t at;bool ack;std::string bytes;};
static std::vector<Event> events;
struct Plan{unsigned arm_delay[2],ack_delay[4],bad_ack[4];bool drop_arm[2],drop_ack[4],duplicate[2];int cancel_ms,busy_until_ms;};
static Plan plan;
static uint64_t epoch_ms,last_arm_at,last_ack_at,first_accepted_at;
static unsigned emitted_ack,accepted_arms,matched_acks;
static bool was_cancelled;
static std::vector<std::string> sent;
static void wire_send(const char*);
static void wire_pump();
namespace sense {
using String=std::string;
#define HALO_SENSE_PROD_WRAPPER 1
static unsigned generation=1,sequence=0;
static uint32_t millis(){return ::millis();}
static void delay(unsigned ms){::clock_us+=uint64_t(ms)*1000;}
static unsigned esp_random(){return ++sequence;}
static unsigned get_next_msg_id(){return ++sequence;}
static unsigned sense_user_action_generation(){return generation;}
static bool sense_uart_ordinary_tx_allowed(){return true;}
static bool sense_lcd_query_busy(){return false;}
static bool halo_provisioning_active(){return false;}
static void uart_send_json(const char* line){wire_send(line);}
static void pump_uart_rx_once(){wire_pump();}
#include "Sense_Minimal/sense_media_retry_transport.h"
}
static void wire_send(const char* bytes){
  const unsigned i=sent.size();check(i<2,"at most two sends");sent.emplace_back(bytes);
  if(plan.drop_arm[i])return;
  // A slow first frame cannot be overtaken by a retransmission on one UART.
  uint64_t at=std::max(clock_us/1000+plan.arm_delay[i],last_arm_at);
  events.push_back({at,false,bytes});last_arm_at=at;
  if(plan.duplicate[i]){events.push_back({at+1,false,bytes});last_arm_at=at+1;}
}
static void wire_pump(){
  const uint64_t now=clock_us/1000;
  if(plan.cancel_ms>=0 && now-epoch_ms>=unsigned(plan.cancel_ms) && !was_cancelled){
    ++sense::generation;was_cancelled=true;
  }
  g_lcd_sleep_commit_gate=plan.busy_until_ms>0 && now-epoch_ms<unsigned(plan.busy_until_ms);
  for(;;){
    auto it=std::min_element(events.begin(),events.end(),[](const Event&a,const Event&b){return a.at<b.at;});
    if(it==events.end()||it->at>now)break;
    Event event=*it;events.erase(it);
    JsonDocument doc;check(!deserializeJson(doc,event.bytes),"wire JSON decodes");
    if(event.ack){
      bool before=sense::g_media_retry_arm_acked;sense::media_retry_arm_ack(doc);
      if(!before&&sense::g_media_retry_arm_acked)++matched_acks;
    }else{
      senseSerial.output.clear();check(lcd_media_retry_uart(doc),"real LCD consumes typed frame");
      if(senseSerial.output.empty())continue;
      JsonDocument ack;check(!deserializeJson(ack,senseSerial.output),"real LCD ACK decodes");
      if(ack["ok"].as<unsigned>()==1){
        if(!accepted_arms)first_accepted_at=now;
        ++accepted_arms;
      }
      unsigned i=emitted_ack++;check(i<4,"bounded duplicates produce at most four ACKs");
      if(plan.drop_ack[i])continue;
      switch(plan.bad_ack[i]){
        case 1:ack["token"]="ffffffffffffffff";break;
        case 2:ack["wake_in_s"]=301;break;
        case 3:ack["ok"]=true;break;
        case 4:ack.remove("ok");break;
        case 5:ack["ok"]=0;break;
      }
      std::string bytes;serializeJson(ack,bytes);
      uint64_t at=std::max(now+plan.ack_delay[i],last_ack_at);
      events.push_back({at,true,bytes});last_ack_at=at;
    }
  }
}
static void init_case(){
  reset();events.clear();sent.clear();plan={};plan.cancel_ms=-1;
  emitted_ack=accepted_arms=matched_acks=0;was_cancelled=false;
  sense::generation=1;sense::g_media_retry_arm_acked=false;sense::g_media_retry_arm_token[0]=0;
  first_accepted_at=last_arm_at=last_ack_at=epoch_ms=0;
}
static void run_transaction(uint32_t interval){
  epoch_ms=clock_us/1000;uint32_t start=::millis();
  bool ok=sense::media_retry_arm_lcd(interval,1);++transactions;
  const uint32_t elapsed=uint32_t(::millis()-start);
  check(elapsed<=1800,"transaction deadline survives millis rollover");
  check(ok==(matched_acks!=0),"success requires actual matching delivered ACK");
  check(sent.size()>=1&&sent.size()<=2,"bounded transaction sends");
  if(sent.size()==2)check(sent[0]==sent[1],"retransmission is byte-identical");
  check(!sense::g_media_retry_arm_token[0],"completed transaction invalidates ACK identity");
  if(was_cancelled)check(elapsed<=unsigned(plan.cancel_ms)+5,"user dispatch exits transaction promptly");
  // Delayed arms may still be admitted after Sense times out. They cannot
  // manufacture a successful transaction or reset an existing same-token due.
  while(clock_us/1000<epoch_ms+8000){clock_us+=5000;wire_pump();}
  check(events.empty(),"finite injected delivery drained");
  check(sense::g_media_retry_arm_acked==ok,"late ACK cannot complete closed transaction");
  bool selected=false;uint32_t chosen=choose(21600,selected);
  if(accepted_arms&&interval){
    uint64_t elapsed_us=(clock_us/1000-first_accepted_at)*1000;
    uint64_t original=uint64_t(interval-5)*1000000;
    uint32_t remaining=uint32_t(((original>elapsed_us?original-elapsed_us:0)+999999)/1000000);
    remaining=std::max(5U,remaining);
    check(selected&&chosen==remaining,"all duplicates retain first admitted relative deadline");
  }else check(!selected&&!g_lcd_media_retry_timer.armed,"no accepted nonzero arm creates no media timer");
  check(!critical_depth&&!storage_depth,"paired transaction releases locks");
}
static void timer_matrix(){
  constexpr uint32_t intervals[]={60,300,900,3600,21600};
  for(unsigned iteration=0;iteration<4096;++iteration){
    case_number=iteration;init_case();uint32_t seconds=intervals[random32()%5];
    request(A,seconds);check(acknowledged(true),"matrix arm accepted");
    // Non-integral awake elapsed tests ceiling and minimum-delay boundaries.
    uint64_t awake=random32()%(uint64_t(seconds+7)*1000000);
    clock_us+=awake;uint32_t remaining=uint32_t((uint64_t(seconds-5)*1000000>awake?
      uint64_t(seconds-5)*1000000-awake+999999:0)/1000000);
    remaining=std::max(5U,remaining);
    uint32_t existing;
    switch(random32()%5){case 0:existing=0;break;case 1:existing=remaining;break;
      case 2:existing=remaining-1;break;case 3:existing=remaining+1;break;default:existing=21600;}
    bool selected=false;uint32_t chosen=choose(existing,selected);
    check(chosen==(!existing?remaining:std::min(existing,remaining)),"earliest relative deadline wins");
    check(selected==(!existing||remaining<existing),"OTA/safety timer wins every tie");
    bool aborted=(random32()%4)==0,sdk_ok=(random32()%4)!=0;
    timer_result=sdk_ok?ESP_OK:-1;
    bool configured=configure_sleep_sources(true,chosen);
    check(configured==sdk_ok&&sdk_timer_us==uint64_t(chosen)*1000000,"actual SDK arm argument/result");
    if(!aborted)lcd_media_retry_commit_sleep(chosen,selected,configured);
    bool deep=(random32()%4)!=0,timer=(random32()%3)!=0;
    // Non-RTC globals reset on every real reboot; only RTC state crosses it.
    g_lcd_media_retry_wait_until_ms=0;g_lcd_media_retry_boot=false;clock_us=10000;
    lcd_media_retry_note_boot(deep,timer);
    bool expected=deep&&timer&&!aborted&&configured&&selected;
    check(g_lcd_media_retry_boot==expected,"only committed SDK-success actual TIMER has media provenance");
    check(lcd_media_retry_wait_active()==expected,"receiver grace requires media provenance");
    if(expected){
      request(A,seconds);check(acknowledged(true)&&!g_lcd_media_retry_timer.armed,"post-consumption retransmission cannot rearm");
      clock_us+=119999000;check(lcd_media_retry_wait_active(),"grace still live before boundary");
      clock_us+=1000;check(!lcd_media_retry_wait_active(),"grace ends exactly at boundary");
    }
    if(!deep)check(!g_lcd_media_retry_timer.armed,"reset discards stale RTC arm");
    if(deep&&!expected)check(g_lcd_media_retry_timer.armed,"touch/abort/other timer retains pending hint");
    lcd_media_retry_wait_release("corner_complete");++timer_cases;
  }
}
int main(){
  // Directed transport edges plus deterministic fault schedules. Seed/case are
  // printed before execution so a sanitizer/assert failure is reproducible.
  printf("seed=0x%08x paired=1024 timer=4096\n",rng);fflush(stdout);
  constexpr unsigned delays[]={0,1,895,900,1795,1800,2400};
  for(unsigned i=0;i<1024;++i){
    case_number=i;init_case();
    if(i%8==0)clock_us=(uint64_t(UINT32_MAX)-500)*1000;
    for(unsigned j=0;j<2;++j){plan.arm_delay[j]=delays[random32()%7];plan.drop_arm[j]=random32()%5==0;plan.duplicate[j]=random32()%2;}
    for(unsigned j=0;j<4;++j){plan.ack_delay[j]=delays[random32()%7];plan.drop_ack[j]=random32()%3==0;plan.bad_ack[j]=random32()%6;}
    if(i%7==0)plan.cancel_ms=int((random32()%359)*5);
    if(i%5==0)plan.busy_until_ms=int(random32()%1801);
    printf("paired_case=%u state=0x%08x\n",i,rng);fflush(stdout);
    run_transaction(i%11==0?0:300);
  }
  timer_matrix();
  halo_media_retry::State policy;
  const uint32_t expected[]={300,900,3600,21600,21600};
  for(auto interval:expected){check(halo_media_retry::interval(policy,false)==interval,"policy cap schedule");halo_media_retry::sleep_committed(policy,false);}
  check(halo_media_retry::interval(policy,true)==60,"progress shortens next arm");
  for(unsigned i=0;i<1000;++i){
    auto store=(random32()%2)?halo_media_retry::VoiceSd:halo_media_retry::ImageSd;
    halo_media_retry::attempted(policy,store);
    check(policy.image_first==(store==halo_media_retry::VoiceSd),"attempt rotates next store");
    halo_media_retry::State restored;check(halo_media_retry::decode(halo_media_retry::encode(policy),restored),"policy RTC/NVS hint roundtrip");
    check(restored.pending==policy.pending&&restored.backoff==policy.backoff&&restored.image_first==policy.image_first,"all hint fields retained");
  }
  printf("PASS paired transactions=%u timer/reset/provenance cases=%u policy transitions=1005 checks=%u draws=%u\n",transactions,timer_cases,checks,draws);
}
'''


def wake_harness(root):
    base = load(root, 'test_lcd_media_wake').harness().split('int main(int argc,char**argv){', 1)[0]
    return base + r'''
int main(int argc,char**argv){
  assert(argc==2);unsigned seed=unsigned(strtoul(argv[1],nullptr,10));
  unsigned original_seed=seed;auto next=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
  now_ms=1000;backlight_apply_pct(80);assert(!pwm_positive&&!pwm_inits);
  assert(lcd_media_retry_arm("0123456789abcdef",300));
  bool selected=false;auto seconds=lcd_media_retry_choose_timer(3500,&selected);
  assert(selected&&seconds==295);lcd_media_retry_commit_sleep(seconds,selected,true);
  now_ms=10;lcd_media_retry_note_boot(true,true);assert(g_lcd_media_retry_boot);
  g_lcd_maintenance_timer_armed=1;g_lcd_maintenance_wake_in_s=3500;g_lcd_maintenance_remaining_s=3600;
  g_background_wake_dark=true;init_ui_stack(0);
  assert(pwm_inits==1&&!pwm_positive&&!panel_on&&menus==1);
  unsigned passive=next()%30;
  for(unsigned i=0;i<passive;++i){
    switch(next()%4){
      case 0:lcd_set_backlight_binary(true,"background");break;
      case 1:g_sleep_transition=g_in_light_sleep=true;abort_sleep_transition("media_timer_arm_failed",false);break;
      case 2:now_ms+=next()%7000;break;
      case 3:lcd_media_retry_wait_active();break;
    }
    assert(g_background_wake_dark&&!pwm_positive&&!panel_on&&!wake_notices&&!g_lcd_media_user_session);
  }
  bool save=next()%2;assert(lcd_media_try_claim(!save,false));
  const char* input=next()%2?"touch_press":"scroll_evt";
  ensure_awake_for_ui(input);
  assert(!g_background_wake_dark&&!g_idle_screen_dark&&g_panel_enabled&&g_lvgl_running);
  assert(pwm_positive==1&&panel_on==1&&!lcd_media_retry_wait_active());
  assert(wake_notices==1&&g_lcd_media_user_session);
  assert(lcd_media_replay_cancelled()==!save);
  assert(g_lcd_media_mode==(save?LCD_MEDIA_SAVE:LCD_MEDIA_REPLAY));
  lcd_media_release();
  for(unsigned i=0;i<30;++i){
    now_ms+=next()%10000;ensure_awake_for_ui(next()%2?"touch_press":"scroll_evt");
    assert(!lcd_media_try_claim(true,false));
    assert(lcd_media_try_claim(false,false));lcd_media_release();
    assert(wake_notices==1&&pwm_positive==1&&panel_on==1);
  }
  assert(g_lcd_maintenance_timer_armed&&g_lcd_maintenance_wake_in_s==3500&&g_lcd_maintenance_remaining_s==3600);
  printf("PASS dark/user ordering seed=%u passive=%u active=30 custody=%s input=%s\n",original_seed,passive,save?"save retained":"replay cancelled",input);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix='halo-media-corners-'))
    if args.out:
        out.mkdir(parents=True, exist_ok=False)
    compiler = shutil.which('c++')
    if not compiler:
        raise SystemExit('C++ compiler required')
    json_include = root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'
    results = []
    for name, source, runs in (
        ('paired', paired_harness(root), [[]]),
        ('wake', wake_harness(root), [[str(seed)] for seed in range(1, 65)]),
    ):
        cpp = out / (name + '.cpp')
        binary = out / name
        cpp.write_text(source)
        command = [compiler, '-std=c++17', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I', str(root), '-I', str(json_include), str(cpp), '-o', str(binary)]
        with (out / (name + '-build.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        for arguments in runs:
            label = name + ('-' + arguments[0] if arguments else '')
            with (out / (label + '.log')).open('w') as log:
                run = subprocess.run([str(binary)] + arguments, stdout=log, stderr=subprocess.STDOUT, timeout=30)
            results.append({'case': label, 'returncode': run.returncode})
            if run.returncode:
                print((out / (label + '.log')).read_text()[-12000:])
                raise SystemExit('FAIL retained reproducer: ' + str(cpp) + ' ' + ' '.join(arguments))
    receipt = {'status': 'PASS', 'source_head': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
               'paired_transactions': 1024, 'timer_provenance_cases': 4096, 'policy_transitions': 1005,
               'dark_user_ordering_processes': 64, 'runs': results,
               'boundary_limits': ['Host clocks/RTC/SDK/RTOS/UART/panel doubles; no physical acceptance.',
                                   'UART fault injection preserves same-direction FIFO.',
                                   'Actual transport/admission/policy/timer/UI helpers execute; full board loops do not.'],
               'hashes': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.iterdir()) if p.is_file()}}
    (out / 'RESULT.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print((out / 'paired.log').read_text().splitlines()[-1])
    print('PASS dark/user ordering processes=64; ASan+UBSan; receipts=' + str(out))


if __name__ == '__main__':
    main()
