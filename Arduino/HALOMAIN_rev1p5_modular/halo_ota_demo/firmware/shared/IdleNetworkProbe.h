#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace halo_idle_probe {
struct Request {char nonce[33]{};uint8_t session[8]{};uint32_t boot=0,peer_boot=0,generation=0;bool probe=false;};
// A flat bounded command. No duplicates, escapes, signed/coerced numbers,
// unknown fields, or truncated-prefix acceptance. The USB accumulator rejects
// overflow before this parser; neither parser nor errors echo input bytes.
inline bool parse(const char*raw,size_t n,Request&r){
 if(!raw||n>512)return false;size_t p=0;unsigned seen=0;
 auto ws=[&](){while(p<n&&(raw[p]==' '||raw[p]=='\r'||raw[p]=='\n'||raw[p]=='\t'))++p;};
 auto text=[&](const char*&s,size_t&z){ws();if(p==n||raw[p++]!='"')return false;s=raw+p;z=0;
  while(p<n&&raw[p]!='"'){const unsigned char c=raw[p++];if(c<32||c>=127||c=='\\')return false;++z;}return p<n&&raw[p++]=='"';};
 ws();if(p==n||raw[p++]!='{')return false;
 for(unsigned count=0;count<7;++count){const char*k;size_t kn;if(!text(k,kn))return false;
  const char*names[]={"type","op","nonce","session","boot","peer_boot","gen"};unsigned i=0;
  for(;i<7;++i)if(kn==strlen(names[i])&&!memcmp(k,names[i],kn))break;
  if(i==7||(seen&(1u<<i)))return false;seen|=1u<<i;ws();if(p==n||raw[p++]!=':')return false;ws();
  if(i<4){const char*v;size_t z;if(!text(v,z))return false;
   if(i==0){if(z!=12||memcmp(v,"OTA_IDLE_NET",12))return false;}
   else if(i==1){if(z==5&&!memcmp(v,"probe",5))r.probe=true;else if(z==4&&!memcmp(v,"read",4))r.probe=false;else return false;}
   else {const size_t expected=i==2?32:16;if(z!=expected)return false;unsigned any=0;
    for(size_t j=0;j<z;++j){const char c=v[j];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;any|=unsigned(c!='0');}if(!any)return false;
    if(i==2)memcpy(r.nonce,v,32);else for(unsigned j=0;j<8;++j){auto nib=[](char c){return c<='9'?c-'0':c-'a'+10;};r.session[j]=uint8_t(nib(v[2*j])*16+nib(v[2*j+1]));}}
  }else {if(p==n||raw[p]<'1'||raw[p]>'9')return false;uint64_t v=0;while(p<n&&raw[p]>='0'&&raw[p]<='9'){v=v*10+unsigned(raw[p++]-'0');if(v>UINT32_MAX)return false;}
   if(i==4)r.boot=uint32_t(v);else if(i==5)r.peer_boot=uint32_t(v);else r.generation=uint32_t(v);}
  ws();if(p==n||raw[p++]!=(count==6?'}':','))return false;
 }
 ws();return p==n&&seen==127;
}
} // namespace halo_idle_probe
