#!/usr/bin/env python3
"""Exercise bounded production failure-hook records, owner lifetime and loss reporting."""
import argparse,hashlib,json,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
HEADER='Sense_Minimal/sense_memory_diag.h'
PREFIX=r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
static constexpr int ESP_OK=0,MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2,MALLOC_CAP_DMA=4;
static unsigned samples=0,registrations=0,prints=0;static bool in_hook=false;
static size_t free_bytes=30000,largest=12000;static std::string output;
static size_t heap_caps_get_free_size(int){assert(!in_hook);++samples;return free_bytes;}
static size_t heap_caps_get_largest_free_block(int){assert(!in_hook);++samples;return largest;}
using Hook=void(*)(size_t,uint32_t,const char*);static Hook installed=nullptr;
static int heap_caps_register_failed_alloc_callback(Hook h){++registrations;installed=h;return 0;}
static struct {template<class...A>void printf(const char*fmt,A...args){assert(!in_hook);++prints;char b[700];snprintf(b,sizeof b,fmt,args...);output+=b;}}Serial;
#define HALO_MEMORY_DIAGNOSTICS_TEST 1
'''
TESTS=r'''
using namespace sense_memory;
static unsigned checks=0;
static void ok(bool x){++checks;assert(x);}
static void reset(){
 current=nullptr;failures.guard.clear();failures.sequence=0;failures.dropped=0;failures.phase=Idle;
 for(auto& x:failures.ring)x={};output.clear();samples=prints=0;free_bytes=30000;largest=12000;
}
int main(){
 begin();begin();ok(registrations==1&&registered&&installed==failed_alloc);
 reset();failures.phase=Write;in_hook=true;installed(544,5,"heap_caps_aligned_alloc");in_hook=false;
 ok(samples==0&&prints==0);Failure ring[4];uint32_t seq=0,lost=0;
 ok(failures.copy(ring,seq,lost));ok(seq==1&&lost==0);
 ok(ring[0].bytes==544&&ring[0].caps==5&&ring[0].phase==Write);
 ok(ring[0].function_hash==Failures::function_hash("heap_caps_aligned_alloc"));
 failures.guard.test_and_set();in_hook=true;installed(12,5,nullptr);in_hook=false;
 ok(!failures.copy(ring,seq,lost));failures.guard.clear();ok(failures.copy(ring,seq,lost));ok(seq==1&&lost==1);
 reset();{
  VoiceTrace trace(17,1);ok(samples==3&&current==&trace);
  point(BeforeConnect,Connect);free_bytes=10000;largest=800;
  point(AfterConnect,Connect);point(AfterConnect,Connect);ok(samples==9);
  point(FirstWrite,Write);in_hook=true;installed(544,5,"heap_caps_aligned_alloc");in_hook=false;
  wrote(512);trace.response(-3);ok(prints==0);
  {VoiceTrace nested(18,1);ok(current==&trace);}
  ok(prints==0);free_bytes=30000;largest=12000;
 }
 ok(!current&&failures.phase==Idle);ok(output.find("http=-3 tls_bytes=512")!=std::string::npos);
 ok(output.find("failures=1 dropped=0 complete=1")!=std::string::npos);
 ok(output.find("bytes=544 caps=00000005 phase=3")!=std::string::npos);
 ok(output.find("point=2 internal=10000 dma_free=10000 dma_largest=800")!=std::string::npos);
 ok(output.find("point=5 internal=30000 dma_free=30000 dma_largest=12000")!=std::string::npos);
 reset();{
  VoiceTrace trace(20,2);for(unsigned i=1;i<=7;++i)installed(i,5,"heap_caps_malloc");trace.response(202);
 }
 ok(output.find("failures=7 dropped=0 complete=0")!=std::string::npos);
 size_t pos=0,count=0;while((pos=output.find("[ALLOC_FAIL]",pos))!=std::string::npos){++count;++pos;}ok(count==4);
 reset();{
  VoiceTrace trace(21,1);failures.guard.test_and_set();installed(90,5,"heap_caps_malloc");failures.guard.clear();
 }
 ok(output.find("failures=0 dropped=1 complete=0")!=std::string::npos);
 reset();registered=false;{VoiceTrace trace(22,1);}ok(output.find("complete=0")!=std::string::npos);registered=true;
 reset();std::vector<std::thread> threads;
 for(unsigned i=0;i<6;++i)threads.emplace_back([i]{for(unsigned n=0;n<10000;++n)failures.record(100+i,100+i,"stress");});
 for(auto& t:threads)t.join();ok(failures.copy(ring,seq,lost));ok(seq+lost==60000);
 for(auto& x:ring){ok(x.bytes==x.caps);ok(x.bytes>=100&&x.bytes<106);ok(uint32_t(seq-x.sequence)<4);}
 printf("PASS memory diagnostic: %u checks; hook storage=%zu trace_stack=%zu concurrent_events=%u\n",checks,sizeof(Failures),sizeof(VoiceTrace),seq+lost);
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--source-root',type=Path,default=ROOT);p.add_argument('--out',type=Path);a=p.parse_args();root=a.source_root.resolve()
 text=(root/HEADER).read_text();client=(root/'Sense_Minimal/sense_media_retry_client.h').read_text();voice=(root/'Sense_Minimal/sense_voice.h').read_text()
 assert '#include "sense_memory_diag.h"' in client
 assert voice.index('sense_memory::VoiceTrace memory_trace')<voice.index('SenseMediaRetryClient client;')
 assert 'memory_trace.response(httpResponseCode);' in voice
 assert client.index('const int connected = WiFiClientSecure::connect')<client.index('point(sense_memory::AfterConnect')
 code=PREFIX+'\n#include "'+str(root/HEADER)+'"\n'+TESTS
 with tempfile.TemporaryDirectory(prefix='halo-memory-diag-') as tmp:
  c=Path(tmp)/'test.cpp';b=Path(tmp)/'test';c.write_text(code)
  compiler=shutil.which('clang++') or shutil.which('g++');assert compiler
  build=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-pthread',str(c),'-o',str(b)],capture_output=True,text=True,timeout=40)
  run=subprocess.run([str(b)],capture_output=True,text=True,timeout=30) if not build.returncode else None
 result={'status':'PASS' if run and run.returncode==0 else 'FAIL','scope':'Production header with SDK boundary doubles, loss/overflow/lifetime and concurrent callback coverage. No physical heap or TLS claim.','source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':build.stdout+build.stderr+(run.stdout+run.stderr if run else '')}
 if a.out:a.out.mkdir(parents=True,exist_ok=True);(a.out/'RESULT.json').write_text(json.dumps(result,indent=2)+'\n')
 print(result['output'],end='');return result['status']!='PASS'
if __name__=='__main__':raise SystemExit(main())
