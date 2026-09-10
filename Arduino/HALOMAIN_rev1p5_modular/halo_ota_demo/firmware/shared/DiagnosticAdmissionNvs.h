#pragma once
#include "DiagnosticAdmissionStore.h"
#include <nvs.h>

namespace halo_admission {
struct NvsGuard {
  void*arg;
  bool(*take)(void*);void(*give)(void*);
  bool(*safe)(void*);
  bool(*authorize_capture)(void*,const Record&);
  size_t(*missing_profile)(void*);
  bool(*policy_present)(void*);
  void(*diagnostic_uncertain)(void*);
};
// Future isolated adapter. Existing ota_diag namespace only, one additional
// typed blob. Never erase a namespace, journal slot, policy, user/legacy data.
class NvsAdapter {
  NvsGuard g_;bool enabled_,held_=false,writable_=false,fault_=false;
  nvs_handle_t h_=0;
  static constexpr const char* key(){return "admit_v1";}
  bool safe()const{return held_&&!fault_&&g_.safe(g_.arg);}
  void uncertain(){fault_=true;g_.diagnostic_uncertain(g_.arg);}
  bool take(){
    if(!enabled_||fault_||held_||!g_.take(g_.arg))return false;
    held_=true;if(!safe()){give();return false;}
    const auto e=nvs_open("ota_diag",NVS_READONLY,&h_);
    if(e==ESP_ERR_NVS_NOT_FOUND){h_=0;return true;}
    if(e!=ESP_OK){uncertain();give();return false;}return true;
  }
  void give(){if(h_)nvs_close(h_);h_=0;writable_=false;if(held_)g_.give(g_.arg);held_=false;}
  Read read(uint8_t*out,size_t*n){
    *n=0;if(!safe())return Read::Error;if(!h_)return Read::Missing;
    size_t count=0;auto e=nvs_get_blob(h_,key(),nullptr,&count);
    if(e==ESP_ERR_NVS_NOT_FOUND)return Read::Missing;
    if(e!=ESP_OK)return Read::Error;
    *n=count;if(count!=BYTES)return Read::Present; // occupied corrupt shape
    e=nvs_get_blob(h_,key(),out,n);return e==ESP_OK&&*n==BYTES?Read::Present:Read::Error;
  }
  bool write_handle(){
    if(!safe()||!h_)return false; // no new namespace; capture needs real Context
    if(writable_)return true;
    nvs_close(h_);h_=0;
    if(!safe()||nvs_open("ota_diag",NVS_READWRITE,&h_)!=ESP_OK)return false;
    writable_=true;return safe();
  }
  bool room(size_t n){
    if(n!=BYTES||!safe()||!h_)return false;
    size_t existing=0;const auto e=nvs_get_blob(h_,key(),nullptr,&existing);
    if(e!=ESP_ERR_NVS_NOT_FOUND)return false; // never replacement
    const size_t missing=g_.missing_profile(g_.arg);
    if(missing>49||!safe())return false;
    nvs_stats_t stats{};if(nvs_get_stats(nullptr,&stats)!=ESP_OK||
       stats.available_entries>stats.free_entries||stats.free_entries>stats.total_entries)return false;
    // IDF5.5.4 first-chunk page-tail rule admits64B as exactly4 entries. Existing
    // maximum27-entry policy replacement already exceeds this serialized peak.
    // Reserve the optional provider even if provisioning has not run yet, so
    // capture-first and auth-first orders both preserve the96-entry floor.
    size_t auth_size=0;const auto auth=nvs_get_blob(h_,"auth_v1",nullptr,&auth_size);
    const size_t auth_reserve=auth==ESP_ERR_NVS_NOT_FOUND?3:0;
    if(auth!=ESP_ERR_NVS_NOT_FOUND&&(auth!=ESP_OK||auth_size!=32))return false;
    return stats.available_entries>=auth_reserve&&capacity(stats.available_entries-auth_reserve,missing,g_.policy_present(g_.arg),true)&&safe();
  }
  bool write(const uint8_t*b,size_t n){
    if(!room(n)||!write_handle()||!safe())return false;
    // One set/commit only, never retry on an uncertain return. Store reads back
    // exact bytes; late persistence is retained as a real observation.
    return nvs_set_blob(h_,key(),b,n)==ESP_OK&&nvs_commit(h_)==ESP_OK;
  }
  bool erase(){
    if(!write_handle()||!safe())return false;
    size_t n=0;if(nvs_get_blob(h_,key(),nullptr,&n)!=ESP_OK||n!=BYTES)return false;
    return nvs_erase_key(h_,key())==ESP_OK&&nvs_commit(h_)==ESP_OK;
  }
public:
  explicit NvsAdapter(NvsGuard g,bool enabled=false):g_(g),enabled_(enabled&&g.take&&g.give&&g.safe&&
    g.authorize_capture&&g.missing_profile&&g.policy_present&&g.diagnostic_uncertain){}
  Port port(){return {this,[](void*p){return ((NvsAdapter*)p)->take();},[](void*p){((NvsAdapter*)p)->give();},
    [](void*p){return ((NvsAdapter*)p)->safe();},
    [](void*p,const Record&r){auto&a=*(NvsAdapter*)p;return a.safe()&&a.g_.authorize_capture(a.g_.arg,r);},
    [](void*p,uint8_t*b,size_t*n){return ((NvsAdapter*)p)->read(b,n);},
    [](void*p,size_t n){return ((NvsAdapter*)p)->room(n);},
    [](void*p,const uint8_t*b,size_t n){return ((NvsAdapter*)p)->write(b,n);},
    [](void*p){return ((NvsAdapter*)p)->erase();},
    [](void*p){((NvsAdapter*)p)->uncertain();}};}
};
} // namespace halo_admission
