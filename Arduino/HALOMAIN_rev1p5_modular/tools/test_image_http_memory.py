#!/usr/bin/env python3
"""Exercise production zero-copy PUT headers against partial writes and cancellation.

No network/device access. TLS is a byte-stream double; this does not prove the
physical AES allocation failure is fixed.
"""
import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CODE = r'''
#include <cassert>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>
#include <string>
#include "Sense_Minimal/sense_image_http.h"
static unsigned allocations=0, checks=0;
void* operator new(size_t n){++allocations;if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
static void checked(bool x,int line){++checks;if(!x){fprintf(stderr,"check failed line=%d\n",line);std::abort();}}
#define check(x) checked((x),__LINE__)
struct Client {
 char bytes[8192]{};size_t used=0,max_write=0,calls=0,partial=512,stop_at=8192;
 bool oversize=false;
 size_t write(const uint8_t* p,size_t n){
  ++calls;max_write=std::max(max_write,n);
  if(oversize)return n+1;
  if(used>=stop_at)return 0;
  n=std::min(n,std::min(partial,stop_at-used));assert(used+n<=sizeof(bytes));
  memcpy(bytes+used,p,n);used+=n;return n;
 }
};
static std::string expected(const sense_image_http::Target& t,size_t length,const char* checksum){
 std::string s="PUT "+std::string(t.path,t.path_length)+" HTTP/1.1\r\nHost: "+t.host+
  "\r\nContent-Type: image/jpeg\r\nContent-Length: "+std::to_string(length)+"\r\n";
 if(checksum)s+="If-None-Match: *\r\nx-amz-checksum-sha256: "+std::string(checksum)+"\r\n";
 return s+"Connection: close\r\n\r\n";
}
static bool parse(const std::string& url,sense_image_http::Target& t){
 unsigned before=allocations;bool ok=sense_image_http::parse(url.data(),url.size(),t);
 check(allocations==before);return ok;
}
int main(){
 using namespace sense_image_http;
 const char* checksum="+cGrAmgp6P59Lr/1Fuwsv3cHoqlvOdvNH5eo9Sf6CRk=";
 for(size_t path_size:{1u,2u,400u,496u,497u,498u,499u,500u,501u,502u,503u,508u,509u,510u,511u,512u,513u,1024u,1750u,4000u}){
  std::string url="https://fixture.s3.amazonaws.com/"+std::string(path_size-1,'a');
  Target t;check(parse(url,t));check(t.path==url.data()+url.find('/',8)); // borrowed slash
  check(t.path_length==path_size&&t.https&&t.port==443);
  for(const char* sum:{static_cast<const char*>(nullptr),checksum}){
   const std::string wire=expected(t,149100,sum);
   for(size_t partial:{1u,3u,31u,511u,512u}){
    Client c;c.partial=partial;unsigned before=allocations;
    check(write_headers(c,t,"image/jpeg",149100,sum,[]{return true;}));
    check(allocations==before&&c.max_write<=512&&c.used==wire.size());
    check(!memcmp(c.bytes,wire.data(),wire.size()));
    if(partial==512)check(c.calls==(wire.size()+511)/512);
   }
  }
 }
 std::string signed_url="https://fixture.s3.us-east-1.amazonaws.com/images/a.jpg?X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=a%2Fb&X-Amz-Security-Token=ab%2Bcd%3D&x=+&x=%20";
 signed_url+=std::string(1700,'Z');Target t;check(parse(signed_url,t));
 const std::string wire=expected(t,149100,checksum);
 // Fail at every possible byte: never report success or duplicate a prefix.
 for(size_t stop=0;stop<wire.size();++stop){
  Client c;c.stop_at=stop;unsigned before=allocations;
  check(!write_headers(c,t,"image/jpeg",149100,checksum,[]{return true;}));
  check(allocations==before&&c.used==stop&&!memcmp(c.bytes,wire.data(),stop));
 }
 // Live deadline/user cancellation is checked before every transport write,
 // including after a short write. It stops without attempting the image body.
 for(size_t stop=0;stop<wire.size();++stop){
  Client c;c.partial=1;size_t polls=0;
  check(!write_headers(c,t,"image/jpeg",149100,checksum,[&]{++polls;return c.used<stop;}));
  check(c.used==stop&&polls==stop+1&&!memcmp(c.bytes,wire.data(),stop));
 }
 Client broken;broken.oversize=true;
 check(!write_headers(broken,t,"image/jpeg",1,nullptr,[]{return true;})&&broken.calls==1);
 Client invalid;Target empty;
 check(!write_headers(invalid,empty,"image/jpeg",1,nullptr,[]{return true;})&&invalid.calls==0);
 check(!write_headers(invalid,t,nullptr,1,nullptr,[]{return true;})&&invalid.calls==0);
 // No slash, explicit ports and exact case; invalid inputs reset the view.
 for(const std::string url:{"https://example.com","http://example.com/","https://EXAMPLE.com:8443/p?q=%2F"}){
  check(parse(url,t));Client c;const auto wire2=expected(t,0,nullptr);
  check(write_headers(c,t,"image/jpeg",0,nullptr,[]{return true;}));
  check(c.used==wire2.size()&&!memcmp(c.bytes,wire2.data(),c.used));
 }
 for(auto url:{"","https://","https:///x","ftp://host/x","https://a:0/","https://a:65536/",
  "https://a:9999999999999999999999/","https://a:/","https://a:443x/","https://a@b/",
  "https://a/hello world","https://a/\r\nInjected:yes","https://[::1]/"}){
  check(!parse(url,t));check(!t.path&&t.port==0&&!t.host[0]);
 }
 check(!parse(std::string("https://a/x\0y",13),t));
 check(!parse("https://"+std::string(254,'x')+"/",t));
 check(parse("https://"+std::string(253,'x')+"/",t));
 check(parse("https://a/"+std::string(4086,'x'),t));
 check(!parse("https://a/"+std::string(4087,'x'),t));
 // Non-NUL-terminated input is legal when its extent is supplied exactly.
 char exact[]={'h','t','t','p','s',':','/','/','a','/'};
 check(sense_image_http::parse(exact,sizeof(exact),t)&&t.path==exact+9&&t.path_length==1);
 printf("PASS %u production image HTTP checks: exact signed bytes, no C++ heap allocations, <=512-byte writes, all-byte failures/cancellation, parser bounds\n",checks);
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=ROOT)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-image-http-') as tmp:
        cpp, exe = Path(tmp)/'test.cpp', Path(tmp)/'test'
        cpp.write_text(CODE)
        subprocess.run([shutil.which('clang++') or 'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(args.source_root),
                        str(cpp), '-o', str(exe)], check=True, timeout=30)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
