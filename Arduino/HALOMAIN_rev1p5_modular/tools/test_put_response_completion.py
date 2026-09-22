#!/usr/bin/env python3
"""Execute the production PUT response parser/cleanup with bounded stream doubles.

No socket/device access. Includes fragmented framing, EOF, partial/error replies,
deadline wrap, cancellation and the actual caller's status/lock cleanup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_manual_ota_clock import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root):
    text = (root / 'Sense_Minimal/sense_upload_exec.h').read_text()
    framing = ''
    if 'struct UploadPutResponseFraming' in text:
        framing = definition(text, 'struct UploadPutResponseFraming') + ';\n'
        framing += definition(text, 'static bool upload_put_drain_response(')
    framing += definition(text, 'static bool upload_put_reset_with_budget(')
    tail = text[text.index("  String status_line = tls.readStringUntil('\\n');"):]
    tail = tail[:tail.index('\n\n\n#endif')]
    return r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
struct String : std::string {
 using std::string::string; using std::string::operator=;
 String(const std::string& s):std::string(s){} String()=default;
 bool startsWith(const char* s)const{return rfind(s,0)==0;}
 int indexOf(char c)const{auto n=find(c);return n==npos?-1:int(n);}
 String substring(size_t p)const{return substr(p);}
 int toInt()const{return std::atoi(c_str());}
 void trim(){auto a=find_first_not_of(" \r\n\t");if(a==npos){clear();return;}
  *this=substr(a,find_last_not_of(" \r\n\t")-a+1);}
};
static uint32_t now_ms,started,elapsed,read_step,deadline;
static uint32_t cancel_at,close_at; static unsigned cancel_read,reads,stops,unlocks;
static unsigned trace_finishes;static int trace_code;static bool trace_has_response;
struct MemoryTrace {
 bool finished=false;
 void response(int code){trace_code=code;trace_has_response=true;}
 void finish(){if(finished)return;assert(stops==1);finished=true;++trace_finishes;}
 ~MemoryTrace(){finish();}
};
static bool paused; static unsigned checks;
static uint32_t millis(){return now_ms;}
static void delay(uint32_t n){now_ms+=n;elapsed+=n;if(cancel_at&&elapsed>=cancel_at)paused=true;}
static bool media_retry_network_cancelled(){return paused;}
static bool media_retry_network_active(){return false;}
static bool deadline_expired(uint32_t d){return d&&int32_t(now_ms-d)>=0;}
static unsigned radio_resets;
static const uint32_t ACTION_MIN_REMAINING_MS=1000;
static uint32_t deadline_remaining_ms(uint32_t){return 5000;}
namespace sense_media_network{static uint32_t reset_wait_timeout_ms(uint32_t n){return n;}}
static bool wifi_hard_reset_and_reconnect(const char*,uint32_t){++radio_resets;return true;}
struct Event{uint32_t at;std::string bytes;};
struct SenseMediaRetryClient {
 std::vector<Event> events; size_t event=0,offset=0;
 int available(){
  while(event<events.size()&&offset==events[event].bytes.size()){++event;offset=0;}
  return event<events.size()&&elapsed>=events[event].at?int(events[event].bytes.size()-offset):0;
 }
 int read(){if(!available())return -1;int c=(unsigned char)events[event].bytes[offset++];
  if(++reads==cancel_read)paused=true;if(read_step)delay(read_step);return c;}
 bool connected(){return elapsed<close_at;}
 void stop(){++stops;}
 String readStringUntil(char until){String s;const uint32_t begin=elapsed;
  while(elapsed-begin<2000){if(paused)break;if(available()){int c=read();if(c==until)break;s+=char(c);}
   else if(!connected())break;else delay(1);}return s;}
};
static struct{template<class...A>void printf(const char*,A...){}
 template<class T>void print(T const&){}template<class T>void println(T const&){} }Serial;
static void http_queue_unlock(const char*,unsigned){++unlocks;}
static void uart_send_sense_diag(const char*,const char*,const char*,int32_t,const char*){}
static void diag_record_error_persistent(const char*,int,const char*){}
''' + framing + r'''
static bool response(SenseMediaRetryClient& tls,uint32_t deadline_ms,int* response_code){
 const unsigned effective_job=7;
 MemoryTrace memory_trace;
''' + tail + r'''
static void require(bool ok,const char* test){++checks;if(!ok){
 std::fprintf(stderr,"FAIL %s (elapsed=%u reads=%u stops=%u unlocks=%u)\n",test,elapsed,reads,stops,unlocks);std::exit(1);}}
static void reset(uint32_t start=1000){now_ms=started=start;elapsed=0;read_step=0;deadline=0;
 cancel_at=0;close_at=UINT32_MAX;cancel_read=0;reads=stops=unlocks=trace_finishes=0;paused=false;
 trace_code=0;trace_has_response=false;}
static void run(const char* name,std::vector<Event> events,bool success,int code,uint32_t max_ms,
 uint32_t eof=UINT32_MAX,uint32_t cancel_ms=0,uint32_t budget=0,uint32_t start=1000,unsigned cancel_byte=0){
 reset(start);close_at=eof;cancel_at=cancel_ms;cancel_read=cancel_byte;
 if(budget)deadline=now_ms+budget;SenseMediaRetryClient stream{events};int actual=99;
 bool ok=response(stream,deadline,&actual);
 require(ok==success,name);require(actual==code,name);require(elapsed<=max_ms,name);
 require(stops==1&&unlocks==1,name);
 require(trace_finishes==1&&(!trace_has_response||trace_code==actual),name);
}
int main(){
 const std::string status="HTTP/1.1 200 OK\r\n";
 const std::string empty="Content-Length: 0\r\nConnection: keep-alive\r\n\r\n";
 run("zero body no fixed wait",{{0,status+empty}},true,200,0);
 run("fragmented headers",{{0,status+"Content-L"},{10,"ength: 0\r\n"},{30,"\r\n"}},true,200,30);
 run("sized body keep alive",{{0,status+"Content-Length: 5\r\n\r\nhe"},{50,"llo"}},true,200,50);
 run("case whitespace",{{0,status+"cOnTeNt-LeNgTh:\t0 \t\r\n\r\n"}},true,200,0);
 run("equal duplicate length",{{0,status+"Content-Length: 0\r\nContent-Length: 0\r\n\r\n"}},true,200,0);
 run("empty EOF body",{{0,status+"Server: S3\r\n\r\n"}},true,200,30,30);
 run("EOF body",{{0,status+"Server: S3\r\n\r\nhello"}},true,200,40,40);
 run("204 no body",{{0,"HTTP/1.1 204 No Content\r\nServer: S3\r\n\r\n"}},true,204,0);
 run("500 preserves error",{{0,"HTTP/1.1 500 Failed\r\nContent-Length: 5\r\n\r\nerror"}},false,500,0);
 run("412 preserves verification path",{{0,"HTTP/1.1 412 Failed\r\nContent-Length: 0\r\n\r\n"}},false,412,0);
 const std::string chunk=status+"Transfer-Encoding: chunked\r\n\r\n";
 run("chunked empty",{{0,chunk+"0\r\n\r\n"}},true,200,0);
 run("chunked data extension and trailer",{{0,chunk+"2;test=value\r\nhi\r\n"},{20,"3\r\nbye\r\n0\r\nETag: value\r\n\r\n"}},true,200,20);
 run("unknown length open times out",{{0,status+"Server: S3\r\n\r\n"}},false,-1,2000);
 run("partial headers EOF",{{0,status+"Content-Length: 0\r\n"}},false,-1,0,0);
 run("short body EOF",{{0,status+"Content-Length: 5\r\n\r\nhi"}},false,-1,0,0);
 run("partial sized body timeout",{{0,status+"Content-Length: 5\r\n\r\nhi"}},false,-1,2000);
 run("partial 412 not receipt",{{0,"HTTP/1.1 412 Failed\r\nContent-Length: 5\r\n\r\nhi"}},false,-1,0,0);
 for(const char* bad:{"Content-Length: -1\r\n","Content-Length: 4294967296\r\n","Content-Length: 0x0\r\n",
  "Content-Length: 1\r\nContent-Length: 2\r\n","Transfer-Encoding: gzip\r\n",
  "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n","bad header\r\n","Content-Length: 0\n"})
  run("invalid framing",{{0,status+bad+"\r\n"}},false,-1,0,0);
 for(const char* bad:{"z\r\n","100000000\r\n","2\r\nhiXX","0\r\nbadtrailer\r\n\r\n"})
  run("invalid chunk",{{0,chunk+bad}},false,-1,0,0);
 run("short chunk EOF",{{0,chunk+"5\r\nhi"}},false,-1,0,0);
 run("header line bound",{{0,status+"X: "+std::string(260,'x')+"\r\n\r\n"}},false,-1,0,0);
 std::string large_headers;for(unsigned i=0;i<1000;++i)large_headers+="X: value\r\n";
 run("total header bound",{{0,status+large_headers+"\r\n"}},false,-1,0,0);
 run("large fixed body diagnostic truncation",{{0,status+"Content-Length: 4096\r\n\r\n"+std::string(4096,'x')}},true,200,0);
 run("cancel during wait",{{0,status+"Content-Length: 4\r\n\r\n"},{100,"done"}},false,-1,20,UINT32_MAX,20);
 run("cancel during read",{{0,status+empty}},false,-1,0,UINT32_MAX,0,0,1000,unsigned(status.size()+4));
 run("action budget",{{0,status+"Content-Length: 4\r\n\r\n"},{100,"done"}},false,-1,20,UINT32_MAX,0,20);
 run("millis wrap complete",{{0,status+"Content-Length: 4\r\n\r\n"},{40,"done"}},true,200,40,UINT32_MAX,0,100,UINT32_MAX-20);
 run("millis wrap timeout",{{0,status+"Content-Length: 4\r\n\r\n"}},false,-1,40,UINT32_MAX,0,40,UINT32_MAX-20);
 run("body after fixed deadline",{{0,status+"Content-Length: 4\r\n\r\n"},{2010,"done"}},false,-1,2000);
 // Deterministic split boundaries exercise every position of the actual reply.
 std::string reply=status+"Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
 for(size_t i=1;i<reply.size();++i)
  run("every fragmented byte boundary",{{0,reply.substr(0,i)},{20,reply.substr(i)}},true,200,20);
 // A cancelled reply releases the owner once; a later foreground request can complete.
 run("fresh request after cancel",{{0,status+empty}},true,200,0);
 paused=true;radio_resets=0;
 require(!upload_put_reset_with_budget("cancelled fresh",0)&&radio_resets==0,"fresh cancellation prohibits radio reset");
 paused=false;
 require(upload_put_reset_with_budget("fresh",0)&&radio_resets==1,"ordinary fresh reset remains");
 std::printf("PASS %u production PUT response assertions\n",checks);return 0;
}
'''


def execute(root, out, sanitize=False):
    out.mkdir(parents=True, exist_ok=True)
    cpp = out / 'put_response.cpp'
    cpp.write_text(harness(root))
    cmd = [shutil.which('clang++') or 'c++', '-std=c++17', '-O1', '-g', str(cpp), '-o', str(out / 'put_response')]
    if sanitize:
        cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    build = subprocess.run(cmd, text=True, capture_output=True)
    (out / 'build.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        return {'compiled': False, 'exit_code': build.returncode, 'output': build.stderr}
    run = subprocess.run([str(out / 'put_response')], text=True, capture_output=True, timeout=20)
    (out / 'run.log').write_text(run.stdout + run.stderr)
    return {'compiled': True, 'exit_code': run.returncode, 'output': run.stdout + run.stderr}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--out', type=Path)
    p.add_argument('--negative-source-root', type=Path)
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-put-response-') as temp:
        out = a.out or Path(temp)
        result = {'current': execute(a.source_root, out / 'current', a.sanitize)}
        source = a.source_root / 'Sense_Minimal/sense_upload_exec.h'
        result['source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
        if a.negative_source_root:
            result['negative'] = execute(a.negative_source_root, out / 'negative')
        good = result['current']['exit_code'] == 0 and result['current']['compiled']
        if 'negative' in result:
            good &= result['negative']['compiled'] and result['negative']['exit_code'] != 0
        result['pass'] = bool(good)
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
        print(json.dumps(result, indent=2))
        return 0 if good else 1


if __name__ == '__main__':
    raise SystemExit(main())
