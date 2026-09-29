#pragma once
// Sense report transport only. Included after the existing TLS/NTP/DMA policy.
// One cooperative deadline; the existing unfed watchdog still covers a stalled
// SDK, allocator, mutex or TCPIP task. This is not a kernel WCET guarantee.
#include <atomic>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>
#include <esp_idf_version.h>
#include "SystemPowerTransport.h"
#if ESP_IDF_VERSION_MAJOR!=5 || ESP_IDF_VERSION_MINOR!=5 || ESP_IDF_VERSION_PATCH!=4
#error "Bounded report transport requires the reviewed IDF5.5.4 connection/cleanup paths"
#endif
#if !defined(CONFIG_MBEDTLS_SSL_PROTO_TLS1_2) || defined(CONFIG_MBEDTLS_SSL_PROTO_TLS1_3)
#error "Bounded report transport requires the reviewed TLS1.2-only SDK read path"
#endif

namespace sense_post {
static constexpr uint32_t REQUEST_MS=2000, CLEANUP_MS=1500, SOCKET_MS=100;
static constexpr size_t BODY_MAX=256, PAYLOAD_MAX=8192, HEADER_LINE_MAX=512, HEADERS_MAX=2048, CHUNK=256;
enum class Result:uint8_t {Received,Busy,Invalid,UnsupportedTls,Dns,Connect,Tls,Write,Response,Deadline,Cleanup};
enum class Stage:uint8_t {Admission,Dns,Connect,Handshake,Write,Headers,Body,Done};
struct Request {const char*url;const char*device;const char*owner;const char*request;const uint8_t*payload;size_t length;bool require_body=true;const char*b1_key_id=nullptr;const char*b1_signature=nullptr;};
struct Lease {void*arg;bool(*live)(void*);uint32_t deadline_ms,io_timeout_ms,started_ms;};
struct Response {int status=0;size_t length=0,bytes_sent=0;char body[BODY_MAX+1]{};Stage stage=Stage::Admission;bool http_observed=false,cleanup_ok=true;uint32_t elapsed_ms=0;};
inline uint32_t remaining(uint32_t end){int32_t n=int32_t(end-millis());return n>0?uint32_t(n):0;}
inline bool live(const Lease&l,uint32_t end){return l.live&&l.live(l.arg)&&remaining(end);}
inline bool token(const char*p,size_t max){if(!p)return true;size_t n=strnlen(p,max);if(n==max)return false;for(size_t i=0;i<n;++i)if(uint8_t(p[i])<33||uint8_t(p[i])>126)return false;return true;}
struct Endpoint {char host[192]{};char authority[208]{};char path[256]{};uint16_t port=443;};
inline bool endpoint(const char*url,Endpoint&e){
  if(!url||strncmp(url,"https://",8))return false;
  const char*p=url+8;size_t n=0;
  while(p[n]&&p[n]!='/'&&p[n]!=':'){char c=p[n];if(n+1>=sizeof(e.host)||!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'))return false;e.host[n]=c;++n;}
  if(!n)return false;e.host[n]=0;p+=n;
  if(*p==':'){++p;uint32_t port=0;unsigned digits=0;while(*p>='0'&&*p<='9'){if(++digits>5)return false;port=port*10+unsigned(*p++-'0');}if(!digits||!port||port>65535)return false;e.port=uint16_t(port);}
  if(*p&&*p!='/')return false;const size_t authority_length=size_t(p-(url+8));if(authority_length>=sizeof(e.authority))return false;
  memcpy(e.authority,url+8,authority_length);e.authority[authority_length]=0;if(!*p)p="/";
  n=strnlen(p,sizeof(e.path));if(n==sizeof(e.path)||!token(p,sizeof(e.path))||strchr(p,'#'))return false;
  memcpy(e.path,p,n+1);return true;
}

// Static mailbox, never stack callback data. A timed-out request closes
// acceptance under the same TCPIP lock but cannot recycle an outstanding slot.
struct DnsSlot {std::atomic<bool>pending{false},done{false};bool accepting=false,valid=false;uint32_t until=0;ip_addr_t address{};};
static DnsSlot dns_slot;
static std::atomic_flag owner=ATOMIC_FLAG_INIT;
inline void dns_callback(const char*,const ip_addr_t*a,void*arg){
  DnsSlot&s=*static_cast<DnsSlot*>(arg);
  s.valid=s.accepting&&remaining(s.until)&&a;
  if(s.valid)s.address=*a;
  s.done.store(true,std::memory_order_release);
  s.pending.store(false,std::memory_order_release);
}
inline void close_dns(){LOCK_TCPIP_CORE();dns_slot.accepting=false;UNLOCK_TCPIP_CORE();}
inline bool resolve(const char*host,const Lease&lease,uint32_t end,char*out,size_t cap){
  if(!live(lease,end))return false;
  LOCK_TCPIP_CORE();
  if(dns_slot.pending.load(std::memory_order_acquire)){UNLOCK_TCPIP_CORE();return false;}
  dns_slot.accepting=true;dns_slot.valid=false;dns_slot.until=end;dns_slot.done.store(false,std::memory_order_relaxed);dns_slot.pending.store(true,std::memory_order_release);
  ip_addr_t address{};
  err_t e=dns_gethostbyname_addrtype(host,&address,dns_callback,&dns_slot,LWIP_DNS_ADDRTYPE_IPV4_IPV6);
  if(e==ERR_OK)dns_callback(host,&address,&dns_slot);
  else if(e!=ERR_INPROGRESS)dns_callback(host,nullptr,&dns_slot);
  UNLOCK_TCPIP_CORE();
  while(live(lease,end)&&!dns_slot.done.load(std::memory_order_acquire))delay(1);
  const bool eligible=live(lease,end);
  LOCK_TCPIP_CORE();
  bool ok=eligible&&remaining(end)&&dns_slot.done.load(std::memory_order_acquire)&&dns_slot.valid;
  if(ok)ok=ipaddr_ntoa_r(&dns_slot.address,out,int(cap))!=nullptr;
  dns_slot.accepting=false;UNLOCK_TCPIP_CORE();return ok;
}

inline bool configure(esp_tls_cfg_t&cfg,const char*host){
  cfg=esp_tls_cfg_t{};cfg.non_block=true;cfg.timeout_ms=SOCKET_MS;cfg.common_name=host;cfg.skip_common_name=false;
  // Every currently reviewed bench/shipping profile uses verified HTTPS. Do
  // not silently turn an unsupported insecure override into verified success.
#if OTA_REPORT_HTTP_INSECURE
  return false;
#else
  const char*ca=OTA_REPORT_HTTP_ROOT_CA;
#if HAS_CRT_BUNDLE
  if(!ca||!ca[0]){
    extern const uint8_t bundle_start[] asm("_binary_x509_crt_bundle_start");
    extern const uint8_t bundle_end[] asm("_binary_x509_crt_bundle_end");
    if(esp_crt_bundle_set(bundle_start,size_t(bundle_end-bundle_start))!=ESP_OK)return false;
    cfg.crt_bundle_attach=esp_crt_bundle_attach;return true;
  }
#else
  if(!ca||!ca[0])ca=rootCA;
#endif
  if(!ca||!ca[0])return false;size_t n=strnlen(ca,16384);if(n==16384)return false;
  cfg.cacert_buf=reinterpret_cast<const unsigned char*>(ca);cfg.cacert_bytes=n+1;return true;
#endif
}

inline Result wait_connected(esp_tls_t*tls,const Lease&lease,uint32_t end){
  int fd=-1;if(esp_tls_get_conn_sockfd(tls,&fd)!=ESP_OK||fd<0)return Result::Connect;
  const int flags=fcntl(fd,F_GETFL,0);if(flags<0||!(flags&O_NONBLOCK))return Result::Connect;
  while(live(lease,end)){
    fd_set rd,wr;FD_ZERO(&rd);FD_ZERO(&wr);FD_SET(fd,&rd);FD_SET(fd,&wr);
    uint32_t n=remaining(end);if(n>SOCKET_MS)n=SOCKET_MS;if(!n)break;
    timeval tv{0,int(n*1000)};
    const int ready=lwip_select(fd+1,&rd,&wr,nullptr,&tv);
    if(!live(lease,end))return Result::Deadline;
    if(ready<0){if(errno==EINTR)continue;return Result::Connect;}
    if(!ready)continue;
    int error=0;socklen_t bytes=sizeof(error);
    if(lwip_getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&bytes)!=0||error)return Result::Connect;
    return Result::Received;
  }return Result::Deadline;
}

// Fixed parser: no String, progress timeout, redirects or unbounded body.
struct Parser {
  char line[HEADER_LINE_MAX]{};size_t used=0,total=0,expected=0;bool status=false,headers=false,have_length=false,require_body=true;
  explicit Parser(bool body=true):require_body(body){}
  static bool name(const char*p,size_t n,const char*s){if(strlen(s)!=n)return false;for(size_t i=0;i<n;++i){char c=p[i];if(c>='A'&&c<='Z')c+=32;if(c!=s[i])return false;}return true;}
  bool line_done(Response&r){
    if(!status){
      if(used<12||(strncmp(line,"HTTP/1.1 ",9)&&strncmp(line,"HTTP/1.0 ",9)))return false;
      if(line[9]<'1'||line[9]>'5'||line[10]<'0'||line[10]>'9'||line[11]<'0'||line[11]>'9'||(used>12&&line[12]!=' '))return false;
      r.status=(line[9]-'0')*100+(line[10]-'0')*10+line[11]-'0';r.http_observed=true;status=true;return r.status>=200;
    }
    if(!used){if(require_body&&!have_length)return false;headers=true;r.stage=Stage::Body;return true;}
    if(line[0]==' '||line[0]=='\t')return false;
    const char*colon=strchr(line,':');if(!colon)return false;size_t n=size_t(colon-line);
    if(name(line,n,"transfer-encoding"))return !require_body;
    if(name(line,n,"content-length")){
      if(have_length)return false;const char*p=colon+1;while(*p==' '||*p=='\t')++p;
      if(*p<'0'||*p>'9')return false;size_t v=0;while(*p>='0'&&*p<='9'){const unsigned digit=unsigned(*p++-'0');if(v>(0xffffffffU-digit)/10)return false;v=v*10+digit;if(require_body&&v>BODY_MAX)return false;}
      while(*p==' '||*p=='\t')++p;if(*p)return false;expected=v;have_length=true;
    }
    return true;
  }
  bool consume(const uint8_t*p,size_t n,Response&r){
    for(size_t i=0;i<n;++i){uint8_t c=p[i];
      if(headers){if(!require_body)continue;if(r.length>=expected)return false;r.body[r.length++]=char(c);r.body[r.length]=0;continue;}
      if(++total>HEADERS_MAX)return false;
      if(c=='\n'){
        if(!used||line[used-1]!='\r')return false;line[--used]=0;
        if(!line_done(r))return false;used=0;line[0]=0;
      }else{if((c<32&&c!='\r'&&c!='\t')||c==127||used+1>=HEADER_LINE_MAX)return false;line[used++]=char(c);line[used]=0;}
    }return true;
  }
  bool complete(const Response&r)const{return headers&&(!require_body||r.length==expected);}
};

// These sequential phases deliberately have separate automatic storage. The
// caller retains the same TLS owner, original request end and cleanup custody.
static __attribute__((noinline)) Result write_request(esp_tls_t*tls,const Request&q,const Endpoint&ep,const Lease&lease,uint32_t request_end,Response&r){
  r.stage=Stage::Write;
  char header[768];
  int hn=snprintf(header,sizeof(header),"POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n",ep.path,ep.authority,unsigned(q.length));
  if(hn<0||size_t(hn)>=sizeof(header))return (Result::Invalid);size_t hlen=size_t(hn);
  const char*keys[]={"X-Device-Id","X-Owner-Id","X-Request-Id","X-Halo-B1-Key-Id","X-Halo-B1-Signature"};const char*values[]={q.device,q.owner,q.request,q.b1_key_id,q.b1_signature};
  for(unsigned i=0;i<5;++i)if(values[i]&&values[i][0]){int n=snprintf(header+hlen,sizeof(header)-hlen,"%s: %s\r\n",keys[i],values[i]);if(n<0||size_t(n)>=sizeof(header)-hlen)return (Result::Invalid);hlen+=size_t(n);}
  // Power describes this transmission, not the frozen diagnostic body. Write
  // it separately so the existing768-byte header and authenticated payload are
  // unchanged. Every fragment shares the original request/cleanup deadline.
  char power[halo_power_transport::JSON_BYTES];
  const size_t power_length=halo_power_transport::snapshot(power,sizeof(power));
  if(!power_length)return Result::Invalid;
  static constexpr char end_headers[]="\r\n\r\n";
  const uint8_t*parts[]={reinterpret_cast<const uint8_t*>(header),
      reinterpret_cast<const uint8_t*>(halo_power_transport::HEADER_PREFIX),
      reinterpret_cast<const uint8_t*>(power),
      reinterpret_cast<const uint8_t*>(end_headers),q.payload};
  const size_t sizes[]={hlen,sizeof(halo_power_transport::HEADER_PREFIX)-1,power_length,sizeof(end_headers)-1,q.length};
  for(unsigned part=0;part<5;++part){size_t off=0;
    while(off<sizes[part]){
      if(!live(lease,request_end))return (Result::Deadline);size_t n=sizes[part]-off;if(n>CHUNK)n=CHUNK;
      int got=int(esp_tls_conn_write(tls,parts[part]+off,n));
      if(!live(lease,request_end))return (Result::Deadline);
      if(got>0){if(size_t(got)>n)return (Result::Write);off+=size_t(got);r.bytes_sent+=size_t(got);}
      else if(got!=ESP_TLS_ERR_SSL_WANT_READ&&got!=ESP_TLS_ERR_SSL_WANT_WRITE)return (Result::Write);
      else delay(1);
    }
  }
  return Result::Received;
}

static __attribute__((noinline)) Result read_response(esp_tls_t*tls,const Request&q,const Lease&lease,uint32_t request_end,Response&r){
  r.stage=Stage::Headers;Parser parser(q.require_body);uint8_t input[CHUNK];
  while(!parser.complete(r)){
    if(!live(lease,request_end))return (Result::Deadline);
    int got=int(esp_tls_conn_read(tls,input,sizeof(input)));
    if(!live(lease,request_end))return (Result::Deadline);
    if(got>0){if(size_t(got)>sizeof(input)||!parser.consume(input,size_t(got),r))return (Result::Response);}
    else if(got!=ESP_TLS_ERR_SSL_WANT_READ&&got!=ESP_TLS_ERR_SSL_WANT_WRITE)return (Result::Response);
    else delay(1);
  }
  r.stage=Stage::Done;return Result::Received;
}

inline Result post_once(const Request&q,Lease lease,Response&r){
  r=Response{};const uint32_t entered=millis();
  const uint32_t budget=lease.io_timeout_ms<REQUEST_MS?lease.io_timeout_ms:REQUEST_MS;
  if(!budget||!lease.live||!lease.live(lease.arg)||!remaining(lease.deadline_ms)||remaining(lease.deadline_ms)<=CLEANUP_MS)return Result::Deadline;
  uint32_t request_end=lease.started_ms+budget;
  if(remaining(request_end)>remaining(lease.deadline_ms)-CLEANUP_MS)request_end=lease.deadline_ms-CLEANUP_MS;
  const uint32_t owner_end=request_end+CLEANUP_MS;
  if(!live(lease,request_end))return Result::Deadline;
  Endpoint ep;if(!endpoint(q.url,ep)||!q.payload||!q.length||q.length>PAYLOAD_MAX||!token(q.device,64)||!token(q.owner,64)||!token(q.request,64)||!token(q.b1_key_id,33)||!token(q.b1_signature,65))return Result::Invalid;
  if(owner.test_and_set(std::memory_order_acquire))return Result::Busy;
  esp_tls_t*tls=nullptr;
  auto finish=[&](Result result){
    close_dns();
    bool ok=true;
    if(tls){
      int fd=-1;if(esp_tls_get_conn_sockfd(tls,&fd)!=ESP_OK)ok=false;
      if(fd>=0){timeval tv{0,int(SOCKET_MS*1000)};
        // Initial cfg also installs this positive cap before TCP connect. If
        // this setter fails, destroy still inherits100ms; never reset it to0.
        if(lwip_setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv))!=0)ok=false;
      }
      if(esp_tls_conn_destroy(tls)!=0)ok=false;tls=nullptr;
    }
    r.cleanup_ok=ok&&remaining(owner_end)&&remaining(lease.deadline_ms);r.elapsed_ms=millis()-entered;
    owner.clear(std::memory_order_release);
    return r.cleanup_ok?result:Result::Cleanup;
  };
  r.stage=Stage::Dns;char numeric[64]{};
  if(!resolve(ep.host,lease,request_end,numeric,sizeof(numeric)))return finish(live(lease,request_end)?Result::Dns:Result::Deadline);
  esp_tls_cfg_t cfg{};if(!configure(cfg,ep.host))return finish(Result::UnsupportedTls);
  tls=esp_tls_init();if(!tls)return finish(Result::Tls);
  r.stage=Stage::Connect;
  while(live(lease,request_end)){
    uint32_t left=remaining(request_end);cfg.timeout_ms=int(left<SOCKET_MS?left:SOCKET_MS);if(cfg.timeout_ms<=0)break;
    int result=esp_tls_conn_new_async(numeric,int(strlen(numeric)),ep.port,&cfg,tls);
    if(!live(lease,request_end))return finish(Result::Deadline);
    if(result==1)break;if(result<0)return finish(Result::Tls);
    esp_tls_conn_state_t state;if(esp_tls_get_conn_state(tls,&state)!=ESP_OK)return finish(Result::Tls);
    if(state==ESP_TLS_CONNECTING){
      // SDK5.5.4 clears its fdsets on the initial100ms timeout. Retain that
      // same socket and100ms send cap, but wait with fresh owner fdsets for the
      // remaining ORIGINAL request budget. No second TCP connect or POST.
      const Result connected=wait_connected(tls,lease,request_end);
      if(connected!=Result::Received)return finish(connected);
      // CONNECTING skips SDK select only. INIT is not re-entered, so this does
      // not change the already O_NONBLOCK socket. Verified5.5.4 TLS setup does
      // not consult non_block; handshake returns WANT. Restore before polling.
      cfg.non_block=false;
      result=esp_tls_conn_new_async(numeric,int(strlen(numeric)),ep.port,&cfg,tls);
      cfg.non_block=true;
      if(!live(lease,request_end))return finish(Result::Deadline);
      int fd=-1;if(esp_tls_get_conn_sockfd(tls,&fd)!=ESP_OK||fd<0)return finish(Result::Connect);
      const int flags=fcntl(fd,F_GETFL,0);if(flags<0||!(flags&O_NONBLOCK))return finish(Result::Connect);
      if(result==1)break;if(result<0)return finish(Result::Tls);
      if(esp_tls_get_conn_state(tls,&state)!=ESP_OK)return finish(Result::Tls);
    }
    if(state!=ESP_TLS_HANDSHAKE)return finish(Result::Tls);
    r.stage=Stage::Handshake;delay(1);
  }
  if(!live(lease,request_end))return finish(Result::Deadline);
  const Result written=write_request(tls,q,ep,lease,request_end,r);
  if(written!=Result::Received)return finish(written);
  return finish(read_response(tls,q,lease,request_end,r));
}
} // namespace sense_post
