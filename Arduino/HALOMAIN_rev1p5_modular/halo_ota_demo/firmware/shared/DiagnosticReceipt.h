#pragma once
#include "DiagnosticStore.h"

namespace halo_diag {
constexpr size_t MAX_RECEIPT_BYTES=256,MAX_EVENT_KEY=64,MAX_KEY_MATERIAL=256;
// Existing deployed handler identity, not a new backend schema. Caller uses the
// existing SHA-1 implementation on this exact material, then event_key(). The
// receipt parser needs no JSON allocator, String, TLS or network object.
inline bool key_component(const char*s,size_t cap){if(!text_ok(s,cap))return false;for(;*s;++s)if(*s=='|')return false;return true;}
class KeyWriter {
  char*out_;size_t capacity_,used_=0;bool good_=true;
 public:
  KeyWriter(char*out,size_t cap):out_(out),capacity_(cap){if(!out||!cap)good_=false;}
  void ch(char c){if(!good_||used_+1>=capacity_){good_=false;return;}out_[used_++]=c;}
  void text(const char*s){while(*s)ch(*s++);}
  void decimal(uint64_t value,unsigned width=0){char reverse[20];unsigned n=0;do{reverse[n++]=char('0'+value%10);value/=10;}while(value);while(width>n){ch('0');--width;}while(n)ch(reverse[--n]);}
  bool finish(size_t maximum){if(!good_||used_>=maximum){if(out_&&capacity_)out_[0]=0;return false;}out_[used_]=0;return true;}
};
inline bool key_material(char*out,size_t cap,const char*device,const char*build,const char*request,uint64_t epoch){
  if(!out||!key_component(device,32)||!key_component(build,96)||!key_component(request,64))return false;
  KeyWriter w(out,cap);w.text(device);w.text("|heartbeat|");w.decimal(epoch);w.ch('|');w.text(build);w.ch('|');w.text(request);return w.finish(MAX_KEY_MATERIAL);
}
inline bool event_key(char*out,size_t cap,uint64_t epoch,const uint8_t sha1[20]){
  if(!out||!sha1)return false;static const char digits[]="0123456789abcdef";
  KeyWriter w(out,cap);w.decimal(epoch,10);w.text("#heartbeat#");
  for(unsigned i=0;i<6;++i){w.ch(digits[sha1[i]>>4]);w.ch(digits[sha1[i]&15]);}return w.finish(MAX_EVENT_KEY);
}
class ReceiptParser {
  const char*p_;const char*end_;
  void ws(){while(p_!=end_&&(*p_==' '||*p_=='\n'||*p_=='\r'||*p_=='\t'))++p_;}
  bool ch(char c){ws();if(p_==end_||*p_!=c)return false;++p_;return true;}
  bool literal(const char*s){ws();size_t n=strlen(s);if(size_t(end_-p_)<n||memcmp(p_,s,n))return false;p_+=n;return true;}
  bool string(char*out,size_t cap){if(!ch('"'))return false;size_t n=0;
    while(p_!=end_){unsigned char c=*p_++;if(c=='"'){if(n>=cap)return false;out[n]=0;return true;}
      if(c=='\\'){if(p_==end_)return false;c=*p_++;if(c!='"'&&c!='\\'&&c!='/')return false;}
      if(c<32||c>126||n+1>=cap)return false;out[n++]=char(c);}
    return false;
  }
  bool scalar(){ws();if(p_==end_)return false;if(*p_=='"'){char value[64];return string(value,sizeof(value));}
    if(*p_=='t')return literal("true");if(*p_=='f')return literal("false");if(*p_=='n')return literal("null");
    if(*p_=='-')++p_;if(p_==end_||*p_<'0'||*p_>'9')return false;
    if(*p_=='0'){++p_;return p_==end_||*p_<'0'||*p_>'9';}
    while(p_!=end_&&*p_>='0'&&*p_<='9')++p_;return true;
  }
 public:
  ReceiptParser(const char*p,size_t n):p_(p),end_(p+n){}
  bool parse(const char*expected){
    if(!ch('{'))return false;bool ok=false,ingested=false,key=false;uint8_t fields=0;
    // Current handler has five flat fields. Limit accepted extension fields and
    // reject duplicate names, including unknown names, rather than last-wins.
    char names[8][24]{};
    while(true){if(fields==8)return false;char name[24];if(!string(name,sizeof(name))||!ch(':'))return false;
      for(uint8_t i=0;i<fields;++i)if(!strcmp(name,names[i]))return false;
      memcpy(names[fields++],name,strlen(name)+1);
      if(!strcmp(name,"ok")){if(!literal("true"))return false;ok=true;}
      else if(!strcmp(name,"ingested")){if(!literal("true"))return false;ingested=true;}
      else if(!strcmp(name,"event_ts_key")){char value[MAX_EVENT_KEY];if(!string(value,sizeof(value))||strcmp(value,expected))return false;key=true;}
      else if(!scalar())return false;
      ws();if(p_!=end_&&*p_=='}'){++p_;ws();return p_==end_&&ok&&ingested&&key;}
      if(!ch(','))return false;
    }
  }
};
inline bool receipt_accepted(int status,const char*body,size_t n,const char*expected){
  if(status!=200||!body||n==0||n>MAX_RECEIPT_BYTES||!text_ok(expected,MAX_EVENT_KEY))return false;
  for(size_t i=0;i<n;++i)if(body[i]==0)return false;
  ReceiptParser parser(body,n);return parser.parse(expected);
}
// The existing transport must durably bind expected to its frozen export
// envelope before calling this seam. This function never treats HTTP/UART
// success alone as permission to retire local evidence.
inline Result acknowledge_cloud_receipt(Journal&journal,int status,const char*body,size_t n,
    const char*expected,const Ack&ack){
  if(!receipt_accepted(status,body,n,expected))return Result::Invalid;
  return journal.acknowledge(ack);
}
} // namespace halo_diag
