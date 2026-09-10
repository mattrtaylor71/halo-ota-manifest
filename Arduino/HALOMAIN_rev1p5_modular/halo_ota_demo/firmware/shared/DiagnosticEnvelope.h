#pragma once
#ifndef HALO_DIAGNOSTIC_ENVELOPE_H
#define HALO_DIAGNOSTIC_ENVELOPE_H
#include "DiagnosticReceipt.h"
namespace halo_diag {
constexpr size_t MAX_ENVELOPE_BYTES=1536;
struct EnvelopeIdentity {Ack ack;uint64_t epoch;char device[32],build[96],fw[12],request[64];uint32_t context_crc;uint8_t slot,board;};
inline void hex_into(char*out,const uint8_t*p,size_t n){static const char d[]="0123456789abcdef";for(size_t i=0;i<n;++i){out[2*i]=d[p[i]>>4];out[2*i+1]=d[p[i]&15];}out[2*n]=0;}
inline bool prepare_envelope(const uint8_t*ctx,const uint8_t*record,uint8_t slot,EnvelopeIdentity&id){
  if(!ctx||!record||slot<1||slot>3||!valid_context(ctx)||!valid(record,slot_size(slot),slot))return false;
  if(get32(record+16)!=record_crc(ctx,0))return false;
  const uint8_t*c=ctx+HEADER,*p=record+HEADER;const char*build=nullptr;const char*fw=nullptr;
  if(slot==1||slot==2){if(get16(record+20)!=228||!get64(p+16)||p[212]!=slot-1||get32(p+44)>get32(p+48)||get32(p+48)!=get32(c+168))return false;build=(const char*)p+72;fw=(const char*)p+216;}
  if(slot==3){if(get16(record+20)!=228||!get64(p))return false;build=(const char*)p+20;fw=(const char*)p+140;}
  if(!key_component(build,96)||!key_component(fw,12)||build[0]==' '||build[strlen(build)-1]==' '||fw[0]==' '||fw[strlen(fw)-1]==' ')return false;id=EnvelopeIdentity{};memcpy(id.ack.journal,c,16);id.ack.sequence=sequence(record);id.ack.crc=record_crc(record,slot);id.slot=slot;id.board=c[166];
  // Same existing firmware canonical ID, derived from the STORED ESP_MAC_ETH.
  memcpy(id.device,"halo-",5);hex_into(id.device+5,c+162,2);id.device[9]='-';hex_into(id.device+10,c+164,2);
  memcpy(id.build,build,strlen(build)+1);memcpy(id.fw,fw,strlen(fw)+1);id.context_crc=record_crc(ctx,0);id.epoch=slot==3?get64(p+8):get64(p+24);
  memcpy(id.request,"d3-",3);hex_into(id.request+3,c,16);id.request[35]='-';
  uint8_t number[8];for(unsigned i=0;i<8;++i)number[i]=uint8_t(id.ack.sequence>>(56-8*i));hex_into(id.request+36,number,8);id.request[52]='-';
  uint8_t crc[4];for(unsigned i=0;i<4;++i)crc[i]=uint8_t(id.ack.crc>>(24-8*i));hex_into(id.request+53,crc,4);return true;
}
// Streaming sink: no JSON document, String, full payload mirror or TLS object.
// Caller holds both source buffers immutable until the synchronous POST ends.
class EnvelopeWriter {
  void*arg_;bool(*write_)(void*,const char*,size_t);size_t n_=0,used_=0;bool ok_=true;char chunk_[64];
  bool flush(){if(used_&&write_&&!write_(arg_,chunk_,used_)){ok_=false;return false;}used_=0;return true;}
 public:
  EnvelopeWriter(void*a,bool(*w)(void*,const char*,size_t)):arg_(a),write_(w){}
  void bytes(const char*s,size_t n){if(!ok_||n>MAX_ENVELOPE_BYTES-n_){ok_=false;return;}n_+=n;if(!write_)return;while(n){size_t take=n<sizeof(chunk_)-used_?n:sizeof(chunk_)-used_;memcpy(chunk_+used_,s,take);used_+=take;s+=take;n-=take;if(used_==sizeof(chunk_)&&!flush())return;}}
  void text(const char*s){bytes(s,strlen(s));}
  void ch(char c){bytes(&c,1);}
  void string(const char*s){ch('"');for(;*s;++s){if(*s=='"'||*s=='\\')ch('\\');ch(*s);}ch('"');}
  void number(uint64_t v){char rev[20];unsigned n=0;do{rev[n++]=char('0'+v%10);v/=10;}while(v);while(n)ch(rev[--n]);}
  void base64(const uint8_t*p,size_t n){static const char a[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";ch('"');for(size_t i=0;i<n;i+=3){const size_t remain=n-i;uint32_t v=uint32_t(p[i])<<16;if(remain>1)v|=uint32_t(p[i+1])<<8;if(remain>2)v|=p[i+2];char b[4]={a[(v>>18)&63],a[(v>>12)&63],remain>1?a[(v>>6)&63]:'=',remain>2?a[v&63]:'='};bytes(b,4);}ch('"');}
  bool finish(size_t&n){if(ok_)flush();n=ok_?n_:0;return ok_;}
};
inline bool write_envelope(const uint8_t*ctx,const uint8_t*record,const EnvelopeIdentity&id,
    void*arg,bool(*sink)(void*,const char*,size_t),size_t&length){
  length=0;if(!key_component(id.device,32)||!key_component(id.build,96)||!key_component(id.fw,12)||!key_component(id.request,64))return false;EnvelopeIdentity actual;if(!prepare_envelope(ctx,record,id.slot,actual)||
    actual.epoch!=id.epoch||memcmp(actual.ack.journal,id.ack.journal,16)||actual.ack.sequence!=id.ack.sequence||actual.ack.crc!=id.ack.crc||
    actual.context_crc!=id.context_crc||strcmp(actual.fw,id.fw)||strcmp(actual.device,id.device)||strcmp(actual.build,id.build)||strcmp(actual.request,id.request)||actual.board!=id.board)return false;
  EnvelopeWriter w(arg,sink);w.text("{\"device_id\":");w.string(id.device);w.text(",\"device_type\":");w.string(id.board==1?"sense":"lcd");
  // Explicit export marker: backend latest from this heartbeat is not a fresh
  // running-firmware/VALID attestation. Exact captured build lives in record.
  w.text(",\"fw\":");w.string(id.fw);w.text(",\"build\":");w.string(id.build);w.text(",\"report_type\":\"heartbeat\",\"diag_export\":1,\"diag_schema\":3,\"request_id\":");w.string(id.request);
  w.text(",\"ts_epoch\":");w.number(id.epoch);w.text(",\"diag_slot\":");w.number(id.slot);w.text(",\"diag_sequence\":");w.number(id.ack.sequence);w.text(",\"diag_crc\":");w.number(id.ack.crc);
  w.text(",\"diag_context_b64\":");w.base64(ctx,256);w.text(",\"diag_record_b64\":");w.base64(record,slot_size(id.slot));w.ch('}');return w.finish(length);
}
inline bool envelope_key_material(char*out,size_t cap,const EnvelopeIdentity&id){return key_material(out,cap,id.device,id.build,id.request,id.epoch);}
// Receipt is bound to the exact frozen source identity and expected key. Local
// UART receipt does not call this method and cannot retire either board's data.
inline Result acknowledge_envelope(Journal&journal,const EnvelopeIdentity&id,const uint8_t sha1[20],int status,const char*reply,size_t n){char expected[MAX_EVENT_KEY];if(!event_key(expected,sizeof(expected),id.epoch,sha1))return Result::Invalid;if(!receipt_accepted(status,reply,n,expected))return Result::Invalid;return journal.acknowledge_pair(id.ack,id.context_crc);}
} // namespace halo_diag

#endif
