#pragma once
#include "DiagnosticAdmissionAuth.h"
#include <nvs.h>
namespace halo_diag_auth {
static constexpr size_t KEY_BYTES=32,KEY_ENTRIES=3,FLOOR=96;
static constexpr const char* KEY="auth_v1";
struct Request {
 char key_id[16]{},nonce[33]{};uint8_t key[32]{};uint32_t sense_boot=0,peer_boot=0;
 ~Request(){halo_admission::wipe(key,sizeof(key));}
};
// Deliberately flat strict grammar: no duplicates, escapes, nested values,
// floats, signs, or coercion. Raw input is never included in any error.
inline bool parse(const char*raw,size_t n,Request&r){
 if(!raw||n>512)return false;size_t p=0;unsigned seen=0;
 auto ws=[&](){while(p<n&&(raw[p]==' '||raw[p]=='\r'||raw[p]=='\n'||raw[p]=='\t'))++p;};
 auto text=[&](const char*&s,size_t&z){ws();if(p==n||raw[p++]!='"')return false;s=raw+p;z=0;
   while(p<n&&raw[p]!='"'){const unsigned char c=raw[p++];if(c<32||c>=127||c=='\\')return false;++z;}
   return p<n&&raw[p++]=='"';};
 ws();if(p==n||raw[p++]!='{')return false;
 for(unsigned count=0;count<7;++count){const char*k;size_t kn;if(!text(k,kn))return false;
   const char*names[]={"type","op","key_id","key_hex","nonce","sense_boot","peer_boot"};unsigned i=0;
   for(;i<7;++i)if(kn==strlen(names[i])&&!memcmp(k,names[i],kn))break;
   if(i==7||(seen&(1u<<i)))return false;seen|=1u<<i;ws();if(p==n||raw[p++]!=':')return false;ws();
   if(i<5){const char*v;size_t vn;if(!text(v,vn))return false;
     if(i<2){const char*expected=i?"install":"OTA_DIAG_AUTH";if(vn!=strlen(expected)||memcmp(v,expected,vn))return false;}
     else if(i==2){if(vn!=15||memcmp(v,"b1-",3))return false;for(size_t x=3;x<15;++x)if(!((v[x]>='0'&&v[x]<='9')||(v[x]>='a'&&v[x]<='f')))return false;memcpy(r.key_id,v,15);}
     else {const size_t z=i==3?64:32;if(vn!=z)return false;for(size_t x=0;x<z;++x){const char c=v[x];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;}
       if(i==4)memcpy(r.nonce,v,32);else for(size_t x=0;x<32;++x){auto nib=[](char c){return c<='9'?c-'0':c-'a'+10;};r.key[x]=uint8_t(nib(v[x*2])*16+nib(v[x*2+1]));}}
   }else {if(p==n||raw[p]<'1'||raw[p]>'9')return false;uint64_t v=0;
     while(p<n&&raw[p]>='0'&&raw[p]<='9'){v=v*10+unsigned(raw[p++]-'0');if(v>UINT32_MAX)return false;}
     if(i==5)r.sense_boot=uint32_t(v);else r.peer_boot=uint32_t(v);
   }
   ws();if(p==n||raw[p++]!=(count==6?'}':','))return false;
 }
 ws();uint8_t any=0;for(uint8_t b:r.key)any|=b;return p==n&&seen==127&&any;
}
inline bool proof(const Request&r,char(&out)[65]){
 static constexpr char prefix[]="HALO_B1_PROVISION_V1\n";uint8_t digest[32]{};
 halo_admission::Hmac h(r.key);const bool ok=h.append(prefix,sizeof(prefix)-1)&&h.append(r.nonce,32)&&h.append("\n",1)&&h.append(r.key_id,15)&&h.finish(digest);
 out[0]=0;if(ok)halo_diag::hex_into(out,digest,32);halo_admission::wipe(digest,sizeof(digest));return ok;
}
struct Capacity {
 bool stats_known=false;size_t free_entries=0,available_entries=0,missing_profile=0,policy_reserve=0,breadcrumb_reserve=0;
 size_t auth_entries=KEY_ENTRIES,required_entries=0;
};
struct Guard {void*arg;bool(*take)(void*);void(*give)(void*);bool(*safe)(void*);bool(*policy_present)(void*);};
enum class Result:uint8_t {Declined,Installed,Unknown};
// The caller owns the original deadline and idle/health predicate. This class
// never creates a namespace, overwrites a key, retries, or changes policy data.
class Storage {
 Guard g_;bool held_=false; nvs_handle_t h_=0;
 bool safe()const{return held_&&g_.safe(g_.arg);}
 struct End {Storage&s;~End(){if(s.h_)nvs_close(s.h_);s.h_=0;if(s.held_)s.g_.give(s.g_.arg);s.held_=false;}};
 bool open(){if(!g_.take(g_.arg))return false;held_=true;return safe()&&nvs_open("ota_diag",NVS_READONLY,&h_)==ESP_OK;}
public:
 explicit Storage(Guard g):g_(g){}
 bool read(uint8_t(&key)[32]){End end{*this};size_t n=0;
   if(!open()||nvs_get_blob(h_,KEY,nullptr,&n)!=ESP_OK||n!=32||!safe())return false;
   return nvs_get_blob(h_,KEY,key,&n)==ESP_OK&&n==32&&safe();}
 Result install(const uint8_t(&key)[32],Capacity&c){End end{*this};c={};
   if(!open())return Result::Declined;
   // Report only the real SAME-lease sample, including capacity refusals.
   nvs_stats_t stats{};if(!safe()||nvs_get_stats(nullptr,&stats)!=ESP_OK||stats.available_entries>stats.free_entries||stats.free_entries>stats.total_entries)return Result::Declined;
   c.stats_known=true;c.free_entries=stats.free_entries;c.available_entries=stats.available_entries;
   const char*keys[]={"ctx","fail0","fail1","state","cp0","cp1"};
   for(unsigned i=0;i<6;++i){size_t n=0;if(!safe())return Result::Declined;const auto e=nvs_get_blob(h_,keys[i],nullptr,&n);const size_t expected=i<4?256:64;
     if(e==ESP_ERR_NVS_NOT_FOUND)c.missing_profile+=2+(expected+31)/32;
     else if(e!=ESP_OK||n!=expected)return Result::Declined;}
   c.policy_reserve=g_.policy_present(g_.arg)?27:54;
   size_t n=0;auto e=nvs_get_blob(h_,"admit_v1",nullptr,&n);
   if(e==ESP_ERR_NVS_NOT_FOUND)c.breadcrumb_reserve=4;else if(e!=ESP_OK||n!=64)return Result::Declined;
   c.required_entries=FLOOR+c.missing_profile+c.policy_reserve+c.breadcrumb_reserve+c.auth_entries;
   // IDF's32B blob is one index+one chunk header+one data:3 entries. The protected
   //27-entry policy replacement peak exceeds this split-page write peak.
   n=0;if(!safe()||nvs_get_blob(h_,KEY,nullptr,&n)!=ESP_ERR_NVS_NOT_FOUND||c.available_entries<c.required_entries)return Result::Declined;
   nvs_close(h_);h_=0;if(!safe()||nvs_open("ota_diag",NVS_READWRITE,&h_)!=ESP_OK)return Result::Declined;
   if(!safe())return Result::Declined;
   // Any uncertain write/commit/readback is terminal; never retry or erase.
   if(nvs_set_blob(h_,KEY,key,32)!=ESP_OK||nvs_commit(h_)!=ESP_OK)return Result::Unknown;
   uint8_t actual[32]{};n=32;const bool same=nvs_get_blob(h_,KEY,actual,&n)==ESP_OK&&n==32&&!memcmp(key,actual,32);halo_admission::wipe(actual,sizeof(actual));
   return same&&safe()?Result::Installed:Result::Unknown;
 }
};
} // namespace halo_diag_auth
