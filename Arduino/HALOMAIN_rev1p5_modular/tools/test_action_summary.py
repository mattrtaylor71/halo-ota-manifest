#!/usr/bin/env python3
"""Native tests of the real boot-scoped summary, including concurrent reads.

Use the bundled ArduinoJson parser to validate appended reports. Only millis,
FreeRTOS critical sections and Arduino String allocation failures are simulated.
"""
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <string>
#include <atomic>
#include "ArduinoJson.h"
#define portMUX_TYPE std::mutex
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) (mux)->lock()
#define portEXIT_CRITICAL(mux) (mux)->unlock()
static uint32_t now_ms;
static uint32_t millis(){return now_ms;}
#include "sense_action_summary.h"
namespace summary=sense_action_summary;
struct Text {
  std::string value;
  bool reserve_fail=false, concat_fail=false, partial_fail=false;
  size_t reserved=0;
  Text(std::string initial):value(initial){}
  size_t length()const{return value.size();}
  char operator[](size_t i)const{return value[i];}
  bool reserve(size_t n){reserved=n;return !reserve_fail;}
  bool concat(const char* p,size_t n){
    if(partial_fail){value.append(p,n/2);return false;}
    if(concat_fail)return false;
    value.append(p,n);return true;
  }
  void remove(size_t i){value.resize(i);}
  void setCharAt(size_t i,char c){value[i]=c;}
};
static void reset(){
  std::lock_guard<std::mutex> lock(summary::state_mux);
  summary::state=summary::Snapshot{};now_ms=0;
}
static JsonDocument parse(const Text& text){
  JsonDocument doc;assert(!deserializeJson(doc,text.value));return doc;
}
static void consistent(const summary::Snapshot& s){
  assert(s.attempts==s.successes+s.failures+unsigned(s.active));
  if(s.active)assert(s.result==summary::Result::Pending);
  else if(s.attempts)assert(s.result!=summary::Result::Pending);
}
int main(){
  reset();Text none("{\"fw\":\"6.4.117\"}");
  assert(summary::append(none,17,8192));auto d=parse(none);
  assert(d["fw"]=="6.4.117"&&d["action_scope"]=="boot_v1");
  assert(d["action_boot_id"]==17&&d["list_attempts"]==0);
  assert(d["list_last_result"].isNull()&&d["list_last_duration_ms"].isNull());
  {
    summary::ListAttempt attempt;now_ms=100;
    Text pending("{}");assert(summary::append(pending,17,8192));d=parse(pending);
    assert(d["list_attempts"]==1&&d["list_successes"]==0&&d["list_failures"]==0);
    assert(d["list_last_result"]=="pending"&&d["list_last_duration_ms"].isNull());
    // Failure detail belongs to the completed attempt, not a torn snapshot.
    summary::note_failure("wifi_connect_timeout");
    assert(summary::snapshot().result==summary::Result::Pending);
    now_ms=6000;
  }
  Text failed("{}");assert(summary::append(failed,17,8192));d=parse(failed);
  assert(d["list_failures"]==1&&d["list_last_result"]=="wifi_timeout");
  assert(d["list_last_duration_ms"]==6000);
  now_ms=UINT32_MAX-99;
  {summary::ListAttempt attempt;now_ms=100;attempt.complete(true);}
  auto s=summary::snapshot();consistent(s);
  assert(s.successes==1&&s.failures==1&&s.duration_ms==200);
  // Repeated report snapshots don't count another user action or reset totals.
  Text again("{}");assert(summary::append(again,17,8192));
  auto before=summary::snapshot();assert(summary::append(failed,17,8192));
  assert(summary::snapshot().attempts==before.attempts);
  std::puts("PASS boot scope, missing vs zero, pending/terminal results, wrap and repeat snapshots");

  reset();
  {std::lock_guard<std::mutex> lock(summary::state_mux);
   summary::state.attempts=summary::state.failures=UINT32_MAX;
   summary::state.duration_ms=UINT32_MAX;
   summary::state.result=summary::Result::WifiUnavailable;}
  char group[summary::GROUP_BYTES];
  auto n=summary::format_group(summary::snapshot(),UINT32_MAX,false,group,sizeof(group));
  assert(n>0&&n<sizeof(group));
  // Two original JSON framing bytes + padding: exact limit is accepted; one
  // extra existing byte omits the entire group and preserves all original data.
  const size_t base=8192-(n-1);
  Text exact("{\"pad\":\""+std::string(base-10,'x')+"\"}");
  assert(exact.length()==base&&summary::append(exact,UINT32_MAX,8192));
  assert(exact.length()==8192&&exact.reserved==8192);d=parse(exact);
  assert(d["list_last_duration_ms"]==UINT32_MAX);
  Text full("{\"pad\":\""+std::string(base-9,'x')+"\"}");
  std::string original=full.value;assert(!summary::append(full,UINT32_MAX,8192));
  assert(full.value==original&&full.reserved==0);
  for(unsigned mode=0;mode<3;++mode){
    Text fault("{\"keep\":123}");original=fault.value;
    fault.reserve_fail=mode==0;fault.concat_fail=mode==1;fault.partial_fail=mode==2;
    assert(!summary::append(fault,17,8192)&&fault.value==original);parse(fault);
  }
  Text unknown("{\"keep\":123}");original=unknown.value;
  assert(!summary::append(unknown,0,8192)&&unknown.value==original);
  summary::format_owner.test_and_set();
  assert(!summary::append(unknown,17,8192)&&unknown.value==original);
  summary::format_owner.clear();
  {summary::ListAttempt attempt;summary::note_failure("arbitrary\"secret");}
  assert(summary::snapshot().attempts==UINT32_MAX);
  assert(summary::snapshot().failures==UINT32_MAX);
  Text safe("{}");assert(summary::append(safe,17,8192));d=parse(safe);
  assert(d["list_last_result"]=="failed"&&safe.value.find("secret")==std::string::npos);
  std::printf("PASS 8192-byte boundary, whole-group omission, allocation/append rollback, enum safety (%zu-byte worst-case group)\n",n);

  reset();std::atomic<bool> done(false);std::atomic<unsigned> reads(0);
  std::thread writer([&]{
    for(unsigned i=0;i<20000;++i){
      summary::ListAttempt attempt;++now_ms;
      if(i%2)summary::note_failure("http_error");else attempt.complete(true);
      std::this_thread::yield();
    }
    done=true;
  });
  auto reader=[&]{
    do{
      consistent(summary::snapshot());
      Text body("{\"existing\":true}");
      if(summary::append(body,18,8192)){
        auto report=parse(body);assert(report["existing"]==true);
        const unsigned attempts=report["list_attempts"],successes=report["list_successes"],failures=report["list_failures"];
        const bool active=report["list_last_result"]=="pending";
        assert(attempts==successes+failures+unsigned(active));
        assert(active?report["list_last_duration_ms"].isNull():
               (attempts==0||report["list_last_duration_ms"].is<uint32_t>()));
      }else assert(body.value=="{\"existing\":true}");
      ++reads;
    }while(!done);
  };
  std::thread first(reader),second(reader);writer.join();first.join();second.join();
  s=summary::snapshot();consistent(s);assert(s.attempts==20000&&s.successes==10000&&s.failures==10000);
  assert(reads>0);std::printf("PASS concurrent writer/two report readers: %u consistent snapshots\n",reads.load());
}
'''


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def main():
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('C++ compiler unavailable')
    with tempfile.TemporaryDirectory(prefix='halo-action-summary-') as directory:
        source = Path(directory) / 'test.cpp'
        binary = Path(directory) / 'test'
        source.write_text(HARNESS)
        subprocess.run([compiler, '-std=c++11', '-pthread', '-Wall', '-Wextra',
                        '-I', str(ROOT / 'Sense_Minimal'), '-I',
                        str(ROOT / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
                        str(source), '-o', str(binary)], check=True, timeout=30,
                       preexec_fn=no_core)
        subprocess.run([str(binary)], check=True, timeout=15, preexec_fn=no_core)


if __name__ == '__main__':
    main()
