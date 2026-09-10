#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

namespace halo_maintenance_witness {
// Exact accepted durable arm identity, not a whole JSON/transport hash.
static constexpr char DOMAIN_TAG[]="HALO_LCD_MAINT_WINDOW_V1";
static constexpr size_t MAX_MATERIAL=sizeof(DOMAIN_TAG)+9*4+1+63;
static_assert(MAX_MATERIAL<=128,"bounded window digest material");
struct Window {
  uint32_t prior_lcd=0,prior_sense=0,armed=0,wake_s=0,remaining_s=0;
  uint32_t start_epoch=0,duration_s=0,grace_before_s=0,grace_after_s=0;
  char request[64]{};
};
using Sha256=bool(*)(const uint8_t*,size_t,uint8_t*);
inline void put32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=uint8_t(n>>(8*i));}
inline bool material(const Window&w,uint8_t(&out)[MAX_MATERIAL],size_t&bytes) {
  bytes=0;const size_t n=strnlen(w.request,sizeof(w.request));
  if(!w.prior_lcd||!w.prior_sense||w.armed!=1||!w.wake_s||w.wake_s>INT32_MAX/1000UL||
     w.remaining_s>INT32_MAX/1000UL||w.start_epoch<1700000000UL||
     uint64_t(w.start_epoch)+w.duration_s+w.grace_after_s>UINT32_MAX||!n||n>=sizeof(w.request))return false;
  memcpy(out,DOMAIN_TAG,sizeof(DOMAIN_TAG));size_t at=sizeof(DOMAIN_TAG);
  const uint32_t fields[]={w.prior_lcd,w.prior_sense,w.armed,w.wake_s,w.remaining_s,
    w.start_epoch,w.duration_s,w.grace_before_s,w.grace_after_s};
  for(uint32_t v:fields){put32(out+at,v);at+=4;}
  out[at++]=uint8_t(n);memcpy(out+at,w.request,n);bytes=at+n;return true;
}
inline bool digest(const Window&w,Sha256 hash,uint8_t(&out)[32]) {
  uint8_t raw[MAX_MATERIAL];size_t n=0;return hash&&material(w,raw,n)&&hash(raw,n,out);
}
} // namespace halo_maintenance_witness
