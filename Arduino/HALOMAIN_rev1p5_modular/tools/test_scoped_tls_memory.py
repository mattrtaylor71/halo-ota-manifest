#!/usr/bin/env python3
"""Exercise the actual scoped mbedTLS allocator with task/heap SDK boundaries."""
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = 'halo_ota_demo/firmware/shared/ScopedTlsMemory.h'
STUBS = {
    'esp_heap_caps.h': '''#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM 0x400
#define MALLOC_CAP_8BIT 0x004
extern "C" void* heap_caps_calloc(size_t,size_t,uint32_t);
extern "C" void heap_caps_free(void*);
''',
    'freertos/FreeRTOS.h': '#pragma once\nusing TaskHandle_t=void*;\n',
    'freertos/task.h': '#pragma once\n#include "FreeRTOS.h"\nextern "C" TaskHandle_t xTaskGetCurrentTaskHandle();\n',
}
NATIVE = r'''
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>
#include "halo_ota_demo/firmware/shared/ScopedTlsMemory.h"
namespace tls=halo_tls_memory;
extern "C" void peer_initialize(bool);
extern "C" bool peer_shared_state();
extern "C" void* peer_scoped_allocation(size_t);
extern "C" tls::Counters peer_snapshot();
static std::atomic<unsigned> checks{0},external_calls{0},free_calls{0};
static std::atomic<bool> fail_external{false};
static thread_local TaskHandle_t task=reinterpret_cast<void*>(1);
static void ok(bool value){
 ++checks;
 if(!value){std::fprintf(stderr,"FAIL scoped TLS check %u\n",checks.load());std::abort();}
}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(){return task;}
extern "C" void* heap_caps_calloc(size_t n,size_t size,uint32_t caps){
 ok(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 ok(task&&tls::detail::owner.load()==task);
 ok(!size||n<=SIZE_MAX/size);++external_calls;
 if(fail_external)return nullptr;
 return ::calloc(n,size);
}
extern "C" void heap_caps_free(void* pointer){++free_calls;::free(pointer);}
static void zeroed(void* pointer,size_t bytes){
 ok(pointer!=nullptr);
 for(size_t i=0;i<bytes;++i)ok(static_cast<unsigned char*>(pointer)[i]==0);
 std::memset(pointer,0xa5,bytes);
}
static void default_allocation(){
 const auto calls=external_calls.load();void* p=tls::calloc(3,31);
 zeroed(p,93);ok(external_calls==calls);tls::free(p);
}
struct Client {
 void* pointer=tls::calloc(2,64);
 Client(){zeroed(pointer,128);}
 ~Client(){
  // Scope must still own the task while the client's cleanup runs.
  ok(tls::detail::owner.load()==task);
  void* final=tls::calloc(1,8);zeroed(final,8);tls::free(final);tls::free(pointer);
 }
};
static void early_exit(unsigned where){
 tls::Scope scope;ok(scope.active());
 if(where==0)return;
 Client client;
 if(where==1)return;
 if(where==2)throw 7;
}
int main(){
 static_assert(!std::is_copy_constructible<tls::Scope>::value,"Scope cannot copy");
 static_assert(!std::is_move_constructible<tls::Scope>::value,"Scope cannot move tasks");
 peer_initialize(false);{tls::Scope disabled;ok(!disabled.active());default_allocation();}
 ok(peer_shared_state());
 tls::initialize(true);default_allocation();
 task=nullptr;{tls::Scope no_task;ok(!no_task.active());default_allocation();}task=reinterpret_cast<void*>(1);
 const auto before=tls::snapshot();void* retained=nullptr;
 {
  tls::Scope outer;ok(outer.active());
  for(size_t bytes:{size_t(1),size_t(32),size_t(4096),size_t(4097)}){
   void* p=tls::calloc(1,bytes);zeroed(p,bytes);tls::free(p);
  }
  {tls::Scope nested;ok(nested.active());retained=tls::calloc(7,9);zeroed(retained,63);}
  ok(tls::detail::owner.load()==task);
  // A foreign task cannot adopt, release, or use this task's preference.
  task=reinterpret_cast<void*>(2);
  {tls::Scope foreign;ok(!foreign.active());default_allocation();}
  ok(tls::detail::owner.load()==reinterpret_cast<void*>(1));task=reinterpret_cast<void*>(1);
  fail_external=true;void* fallback=tls::calloc(2,800);zeroed(fallback,1600);tls::free(fallback);fail_external=false;
  const auto calls=external_calls.load();const auto snap=tls::snapshot();
  void* zero=tls::calloc(0,32);tls::free(zero);zero=tls::calloc(32,0);tls::free(zero);
  ok(tls::calloc(SIZE_MAX/2+1,2)==nullptr);ok(tls::calloc(SIZE_MAX,2)==nullptr);
  ok(external_calls==calls&&tls::snapshot().external_calls==snap.external_calls);
 }
 ok(tls::detail::owner.load()==nullptr);task=reinterpret_cast<void*>(2);
 tls::free(retained);tls::free(nullptr);default_allocation();
 auto after=tls::snapshot();
 ok(after.external_calls-before.external_calls==6);
 ok(after.external_requested_bytes-before.external_requested_bytes==1+32+4096+4097+63+1600);
 ok(after.small_external_calls-before.small_external_calls==5);
 ok(after.small_external_requested_bytes-before.small_external_requested_bytes==1+32+4096+63+1600);
 ok(after.external_failures-before.external_failures==1&&after.default_fallbacks-before.default_fallbacks==1);
 {
  const auto initial=tls::snapshot();tls::Scope scope;ok(scope.active());
  void* peer=peer_scoped_allocation(65);zeroed(peer,65);tls::free(peer);
  ok(tls::detail::owner.load()==task);
  ok(peer_snapshot().external_calls==initial.external_calls+1);
  ok(tls::snapshot().external_requested_bytes==initial.external_requested_bytes+65);
 }
 ok(tls::detail::owner.load()==nullptr);
 for(unsigned where=0;where<4;++where){
  try{early_exit(where);ok(where!=2);}catch(int value){ok(where==2&&value==7);}
  ok(tls::detail::owner.load()==nullptr);default_allocation();
 }
 // Actual overlapping host threads, with distinct modeled FreeRTOS tasks.
 std::atomic<bool> held{false},foreign_done{false},released{false};
 std::thread owner([&]{task=reinterpret_cast<void*>(3);{
  tls::Scope scope;ok(scope.active());held=true;
  while(!foreign_done.load())std::this_thread::yield();
  void* p=tls::calloc(1,64);zeroed(p,64);tls::free(p);
 }released=true;});
 std::thread foreign([&]{task=reinterpret_cast<void*>(4);
  while(!held.load())std::this_thread::yield();
  const auto calls=external_calls.load();
  for(unsigned n=0;n<64;++n){tls::Scope scope;ok(!scope.active());default_allocation();}
  ok(external_calls==calls);foreign_done=true;
  while(!released.load())std::this_thread::yield();
  tls::Scope scope;ok(scope.active());void* p=tls::calloc(1,64);zeroed(p,64);tls::free(p);
 });owner.join();foreign.join();ok(tls::detail::owner.load()==nullptr);
 std::atomic<unsigned> active_tasks{0};std::vector<std::thread> workers;
 for(uintptr_t id=5;id<9;++id){workers.emplace_back([id,&active_tasks]{task=reinterpret_cast<void*>(id);
  for(unsigned n=0;n<300;++n){tls::Scope scope;
   if(scope.active()){
    ok(active_tasks.fetch_add(1)==0);{tls::Scope nested;ok(nested.active());}
    void* p=tls::calloc(1,16);zeroed(p,16);tls::free(p);ok(active_tasks.fetch_sub(1)==1);
   }else{
    // Other owner calls may race the process-wide counter, so assert this
    // path's isolation inside the SDK double instead of comparing snapshots.
    void* p=tls::calloc(1,16);zeroed(p,16);tls::free(p);
   }
  }
 });}
 for(auto& worker:workers)worker.join();
 ok(tls::detail::owner.load()==nullptr&&active_tasks==0);
 ok(tls::snapshot().external_calls==external_calls.load());
 std::printf("PASS scoped TLS allocator: %u checks, 1200 concurrent claims, two-TU shared state; external_calls=%u frees=%u\n",
             checks.load(),external_calls.load(),free_calls.load());
}
'''
PEER = r'''
// Deliberately differs from the sketch's production-wrapper macro. The shared
// allocation state and class layout must not depend on translation-unit role.
#define HALO_SCOPED_TLS_PEER_TRANSLATION_UNIT 1
#include "halo_ota_demo/firmware/shared/ScopedTlsMemory.h"
namespace tls=halo_tls_memory;
extern "C" void peer_initialize(bool ready){tls::initialize(ready);}
extern "C" bool peer_shared_state(){return !tls::detail::enabled.load()&&tls::detail::owner.load()==nullptr;}
extern "C" void* peer_scoped_allocation(size_t bytes){
 tls::Scope nested;
 return nested.active()?tls::calloc(1,bytes):nullptr;
}
extern "C" tls::Counters peer_snapshot(){return tls::snapshot();}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    root = args.source_root.resolve()
    with tempfile.TemporaryDirectory(prefix='halo-scoped-tls-') as directory:
        directory = Path(directory)
        for relative, content in STUBS.items():
            target = directory / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        source, binary = directory / 'test.cpp', directory / 'test'
        source.write_text(NATIVE)
        peer = directory / 'peer.cpp'
        peer.write_text(PEER)
        compiler = shutil.which('clang++') or shutil.which('g++')
        assert compiler
        built = subprocess.run([compiler, '-std=gnu++17', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-pthread',
                                '-I', str(directory), '-I', str(root), str(source), str(peer),
                                '-o', str(binary)], capture_output=True, text=True, timeout=40)
        ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30) if built.returncode == 0 else None
    result = {
        'status': 'PASS' if ran and ran.returncode == 0 else 'FAIL',
        'source_sha256': hashlib.sha256((root / HEADER).read_bytes()).hexdigest(),
        'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'scope': 'Actual production header in two GNU++17 translation units; task and heap SDK boundaries doubled. ASan/UBSan, shared enable/owner/counters, zeroing, overflow, default/external failure fallback, nesting, foreign tasks, early return/exception cleanup, lifetime after scope and concurrent owner claims. No physical heap/TLS savings claim.',
        'compiled': built.returncode == 0,
        'exit_code': ran.returncode if ran else None,
        'output': built.stdout + built.stderr + (ran.stdout + ran.stderr if ran else ''),
    }
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(result['output'], end='')
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
