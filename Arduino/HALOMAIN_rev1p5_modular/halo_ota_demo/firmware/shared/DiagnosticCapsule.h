#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

// Pure, allocation-free format. No device/NVS/network dependency. Each board
// owns one instance; firmware integration must serialize the borrowed scratch.
namespace halo_diag {
constexpr uint32_t MAGIC=0x31474448u;
constexpr uint8_t VERSION=3;
constexpr size_t HEADER=24, MAX_RECORD=256, SLOT_COUNT=6;
constexpr uint8_t CONTEXT_SLOT=0, FAILURE_FIRST=1, STATE_FIRST=3,
                  CHECKPOINT_FIRST=4;
constexpr uint16_t MAX_STATE_RESERVATIONS=16;
// v3 allowed96 total writes. v4 reserves control room without increasing
// ordinary logging: at most88 ordinary generations,104 absolute generations.
// Existing exhausted v3 can use exactly8 migration/drain/retirement commits.
constexpr uint64_t MAX_ORDINARY_CHECKPOINT_COMMITS=88, MAX_CHECKPOINT_COMMITS=104;
constexpr uint8_t CHECKPOINT_VERSION=4;
enum class Lifecycle:uint8_t { Active,Sealed,HandoffReady,HandoffAcked,OrphanClosing };
constexpr uint8_t STATE_ORPHAN_CLOSURE=0x80;
inline bool handoff_lifecycle(Lifecycle l){return l==Lifecycle::HandoffReady||l==Lifecycle::HandoffAcked;}
constexpr size_t LOGICAL_BYTES=4*256+2*64;
static_assert(LOGICAL_BYTES==1152,"persistent logical profile");
enum class Kind:uint8_t { Context=1, Failure=2, State=3, Transition=4, Checkpoint=5 };
enum class Board:uint8_t { Sense=1,Lcd=2 };
enum class TimeQuality:uint8_t { Unknown=0,Retained=1,Fresh=2 };
enum class HashKind:uint8_t { Unknown=0,ExpectedBin=1,VerifiedBin=2,CompiledElf=3 };
enum class Stage:uint16_t { Boot=1,Ready,Admission,ProxyBegin,ProxyCleaned,SelfBegin,
  Http,Write,End,SelectBoot,Failure,Cleanup,PeerQuery,PeerAck,TimerSelected,
  TimerSdk,PreSleep,Retry,JournalBefore,JournalAfter,ReportBefore,ReportAfter };
inline size_t slot_size(uint8_t slot) {return slot<4?256:(slot<6?64:0);}
inline Kind slot_kind(uint8_t slot) {return slot==0?Kind::Context:(slot<3?Kind::Failure:(slot<4?Kind::State:Kind::Checkpoint));}
inline uint16_t get16(const uint8_t*p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline uint32_t get32(const uint8_t*p){return uint32_t(get16(p))|(uint32_t(get16(p+2))<<16);}
inline uint64_t get64(const uint8_t*p){return uint64_t(get32(p))|(uint64_t(get32(p+4))<<32);}
inline void put16(uint8_t*p,uint16_t v){p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
inline void put32(uint8_t*p,uint32_t v){put16(p,uint16_t(v));put16(p+2,uint16_t(v>>16));}
inline void put64(uint8_t*p,uint64_t v){put32(p,uint32_t(v));put32(p+4,uint32_t(v>>32));}
inline uint32_t crc32(const uint8_t*p,size_t n){uint32_t c=~uint32_t(0);while(n--){c^=*p++;for(unsigned j=0;j<8;++j)c=(c>>1)^(0xedb88320u & (0u-(c&1)));}return ~c;}
inline bool text_ok(const char*p,size_t n,bool empty=false){if(!p||(!empty&&!p[0]))return false;for(size_t i=0;i<n;++i){if(!p[i])return true;if(uint8_t(p[i])<32||uint8_t(p[i])>126)return false;}return false;}
inline bool nonzero(const uint8_t*p,size_t n){uint8_t b=0;while(n--)b|=*p++;return b!=0;}
inline bool text_copy(uint8_t*out,const char*in,size_t n,bool empty=false){if(!text_ok(in,n,empty))return false;size_t k=0;while(in[k])++k;memset(out,0,n);memcpy(out,in,k);return true;}
inline void start(uint8_t*out,uint8_t slot,uint64_t seq,uint32_t context_crc,uint16_t payload){size_t n=slot_size(slot);memset(out,0,n);put32(out,MAGIC);out[4]=VERSION;out[5]=uint8_t(slot_kind(slot));out[6]=slot;put64(out+8,seq);put32(out+16,context_crc);put16(out+20,payload);}
inline void seal(uint8_t*out,uint8_t slot){size_t n=slot_size(slot);put32(out+n-4,crc32(out,n-4));}
inline bool valid(const uint8_t*p,size_t n,uint8_t slot){return n==slot_size(slot)&&n>=HEADER+4&&get32(p)==MAGIC&&(p[4]==VERSION||(slot>=4&&p[4]==CHECKPOINT_VERSION))&&p[5]==uint8_t(slot_kind(slot))&&p[6]==slot&&p[7]==0&&get64(p+8)!=0&&get16(p+20)<=n-HEADER-4&&get16(p+22)==0&&get32(p+n-4)==crc32(p,n-4);}
inline uint64_t sequence(const uint8_t*p){return get64(p+8);}
inline uint32_t record_crc(const uint8_t*p,uint8_t slot){return get32(p+slot_size(slot)-4);}

struct Context {
  uint8_t journal[16],campaign[16],target_sha[32];
  char target_version[32],origin[64];
  uint8_t device_mac[6];Board board;HashKind target_hash_kind;
  uint32_t expected_bytes;uint64_t created_epoch;uint8_t owner_binding_sha[32];
};
inline bool encode_context(uint8_t*out,const Context&c){
  if(!nonzero(c.journal,16)||!nonzero(c.campaign,16)||!nonzero(c.device_mac,6)||
     !nonzero(c.target_sha,32)||!c.expected_bytes||!text_ok(c.target_version,32)||!text_ok(c.origin,64)||
     (c.board!=Board::Sense&&c.board!=Board::Lcd)||c.target_hash_kind!=HashKind::ExpectedBin)return false;
  start(out,0,1,0,212);uint8_t*p=out+HEADER;
  memcpy(p,c.journal,16);memcpy(p+16,c.campaign,16);memcpy(p+32,c.target_sha,32);
  text_copy(p+64,c.target_version,32);text_copy(p+96,c.origin,64);memcpy(p+160,c.device_mac,6);
  p[166]=uint8_t(c.board);p[167]=uint8_t(c.target_hash_kind);put32(p+168,c.expected_bytes);
  put64(p+172,c.created_epoch);memcpy(p+180,c.owner_binding_sha,32);seal(out,0);return true;
}
inline bool valid_context(const uint8_t*p){if(!valid(p,256,0)||get16(p+20)!=212||get32(p+16)!=0||sequence(p)!=1)return false;const uint8_t*q=p+HEADER;return nonzero(q,16)&&nonzero(q+16,16)&&nonzero(q+32,32)&&text_ok((const char*)q+64,32)&&text_ok((const char*)q+96,64)&&nonzero(q+160,6)&&(q[166]==1||q[166]==2)&&q[167]==uint8_t(HashKind::ExpectedBin)&&get32(q+168)>0;}
inline bool context_matches(const uint8_t*p,const Context&c){if(!valid_context(p)||!text_ok(c.target_version,32)||!text_ok(c.origin,64))return false;const uint8_t*q=p+HEADER;return !memcmp(q,c.journal,16)&&!memcmp(q+16,c.campaign,16)&&!memcmp(q+32,c.target_sha,32)&&!strcmp((const char*)q+64,c.target_version)&&!strcmp((const char*)q+96,c.origin)&&!memcmp(q+160,c.device_mac,6)&&q[166]==uint8_t(c.board)&&q[167]==uint8_t(c.target_hash_kind)&&get32(q+168)==c.expected_bytes&&get64(q+172)==c.created_epoch&&!memcmp(q+180,c.owner_binding_sha,32);}

struct Failure {
  uint8_t attempt_id[16];uint64_t boot_id,epoch;uint32_t uptime_ms;
  Stage stage;int16_t http;int32_t sdk;uint32_t accepted,expected,internal_free,internal_largest,internal_min;
  int32_t cleanup_sdk;uint32_t flags;char build[96];uint8_t image_hash[32];
  uint32_t running_offset,boot_offset,image_state;uint8_t attempt_ordinal;HashKind hash_kind;char fw[12];
};
inline bool encode_failure(uint8_t*out,uint8_t slot,uint64_t seq,uint32_t ctx,const Failure&f){
  if(slot<1||slot>2||f.attempt_ordinal!=slot-1||!nonzero(f.attempt_id,16)||!f.boot_id||
     !text_ok(f.build,96)||!text_ok(f.fw,12)||f.accepted>f.expected||uint16_t(f.stage)<1||uint16_t(f.stage)>uint16_t(Stage::ReportAfter)||uint8_t(f.hash_kind)>3)return false;
  start(out,slot,seq,ctx,228);uint8_t*p=out+HEADER;memcpy(p,f.attempt_id,16);put64(p+16,f.boot_id);put64(p+24,f.epoch);put32(p+32,f.uptime_ms);put16(p+36,uint16_t(f.stage));put16(p+38,uint16_t(f.http));put32(p+40,uint32_t(f.sdk));put32(p+44,f.accepted);put32(p+48,f.expected);put32(p+52,f.internal_free);put32(p+56,f.internal_largest);put32(p+60,f.internal_min);put32(p+64,uint32_t(f.cleanup_sdk));put32(p+68,f.flags);text_copy(p+72,f.build,96);memcpy(p+168,f.image_hash,32);put32(p+200,f.running_offset);put32(p+204,f.boot_offset);put32(p+208,f.image_state);p[212]=f.attempt_ordinal;p[213]=uint8_t(f.hash_kind);text_copy(p+216,f.fw,12);seal(out,slot);return true;
}
struct State {
  uint64_t boot_id,epoch;uint32_t uptime_ms;char build[96];uint64_t timer_us,selected_epoch;int32_t timer_sdk;Stage stage;uint8_t attempt,timer_flags;char fw[12];uint8_t image_hash[32];
  uint32_t running_offset,boot_offset,partition_size;int32_t image_state_sdk;
  uint8_t image_state,wake,reset;TimeQuality time_quality;HashKind hash_kind;
  uint8_t flags;uint16_t battery_mv;uint32_t internal_free,internal_largest,internal_min;
};
inline bool encode_state(uint8_t*out,uint8_t slot,uint64_t seq,uint32_t ctx,const State&s,uint32_t coalesced){
  if(slot!=3||!s.boot_id||!text_ok(s.build,96)||!text_ok(s.fw,12)||s.attempt>1||uint16_t(s.stage)<1||uint16_t(s.stage)>uint16_t(Stage::ReportAfter)||uint8_t(s.time_quality)>2||uint8_t(s.hash_kind)>3)return false;
  start(out,slot,seq,ctx,228);uint8_t*p=out+HEADER;put64(p,s.boot_id);put64(p+8,s.epoch);put32(p+16,s.uptime_ms);text_copy(p+20,s.build,96);put64(p+116,s.timer_us);put64(p+124,s.selected_epoch);put32(p+132,uint32_t(s.timer_sdk));put16(p+136,uint16_t(s.stage));p[138]=s.attempt;p[139]=s.timer_flags;text_copy(p+140,s.fw,12);memcpy(p+156,s.image_hash,32);put32(p+188,s.running_offset);put32(p+192,s.boot_offset);put32(p+196,s.partition_size);put32(p+200,uint32_t(s.image_state_sdk));p[204]=s.image_state;p[205]=s.wake;p[206]=s.reset;p[207]=uint8_t(s.time_quality);p[208]=uint8_t(s.hash_kind);p[209]=s.flags;put16(p+210,s.battery_mv);put32(p+212,s.internal_free);put32(p+216,s.internal_largest);put32(p+220,s.internal_min);put32(p+224,coalesced);seal(out,slot);return true;
}
// Compact profile has no ordinary progress ring. Timer values are the latest
// state only; full wire arm/challenge proof is not encoded by this profile.
struct Checkpoint {uint64_t generation,next_sequence,state_ack;uint32_t context_crc,dropped_full,dropped_io;uint16_t ack_mask,state_reservations,damage_flags;uint32_t state_ack_crc;Lifecycle lifecycle=Lifecycle::Active;uint32_t handoff_epoch=0,handoff_crc=0;};
inline void encode_checkpoint(uint8_t*out,uint8_t slot,const Checkpoint&c){
  start(out,slot,c.generation,c.context_crc,36);out[4]=CHECKPOINT_VERSION;uint8_t*p=out+HEADER;
  put32(p,uint32_t(c.next_sequence));put32(p+4,c.handoff_crc);put64(p+8,c.state_ack);put32(p+16,c.dropped_full);put32(p+20,c.dropped_io);
  p[24]=uint8_t(c.ack_mask);p[25]=uint8_t(c.state_reservations);p[26]=uint8_t(c.damage_flags);p[27]=uint8_t(c.lifecycle);put32(p+28,c.state_ack_crc);put32(p+32,c.handoff_epoch);seal(out,slot);
}
inline bool decode_checkpoint(const uint8_t*p,uint8_t slot,Checkpoint&c){
  if(slot<4||slot>5||!valid(p,64,slot))return false;const uint8_t*q=p+HEADER;c=Checkpoint{};
  c.generation=sequence(p);c.context_crc=get32(p+16);c.next_sequence=get64(q);c.state_ack=get64(q+8);c.dropped_full=get32(q+16);c.dropped_io=get32(q+20);
  if(p[4]==VERSION){if(get16(p+20)!=34||c.generation>96)return false;c.ack_mask=get16(q+24);c.state_reservations=get16(q+26);c.damage_flags=get16(q+28);c.state_ack_crc=get32(q+30);}
  else{if(get16(p+20)!=36)return false;c.next_sequence=get32(q);c.handoff_crc=get32(q+4);c.ack_mask=q[24];c.state_reservations=q[25];c.damage_flags=q[26];c.lifecycle=Lifecycle(q[27]);c.state_ack_crc=get32(q+28);c.handoff_epoch=get32(q+32);}
  return c.next_sequence>=2&&(!(!c.state_ack&&c.state_ack_crc))&&c.state_ack<c.next_sequence&&c.state_reservations<=MAX_STATE_RESERVATIONS&&c.generation<=MAX_CHECKPOINT_COMMITS&&c.damage_flags<=1&&!(c.ack_mask&~uint16_t(7))&&uint8_t(c.lifecycle)<=4&&
    (handoff_lifecycle(c.lifecycle) ? c.handoff_epoch>=1577836800u : c.handoff_epoch==0&&c.handoff_crc==0);
}
inline bool checkpoint_equal(const Checkpoint&a,const Checkpoint&b){return a.generation==b.generation&&a.next_sequence==b.next_sequence&&a.state_ack==b.state_ack&&a.context_crc==b.context_crc&&a.dropped_full==b.dropped_full&&a.dropped_io==b.dropped_io&&a.ack_mask==b.ack_mask&&a.state_reservations==b.state_reservations&&a.damage_flags==b.damage_flags&&a.state_ack_crc==b.state_ack_crc&&a.lifecycle==b.lifecycle&&a.handoff_epoch==b.handoff_epoch&&a.handoff_crc==b.handoff_crc;}
// One marker replaces a checkpoint only after all intact retained records and
// their current-time handoff have matching cloud receipts. It is deleted LAST.
struct Retirement {uint64_t generation;uint32_t context_crc,record_crc[3],other_crc,epoch;uint8_t journal[16];uint8_t present_mask=0;};
inline bool retirement_tag(const uint8_t*p){return get32(p)==MAGIC&&p[4]==CHECKPOINT_VERSION&&p[5]==6;}
inline void encode_retirement(uint8_t*out,uint8_t slot,const Retirement&r){start(out,slot,r.generation,r.context_crc,36);out[4]=CHECKPOINT_VERSION;out[5]=6;out[7]=r.present_mask;uint8_t*p=out+HEADER;memcpy(p,r.journal,16);for(unsigned i=0;i<3;++i)put32(p+16+i*4,r.record_crc[i]);put32(p+28,r.other_crc);put32(p+32,r.epoch);seal(out,slot);}
inline bool decode_retirement(const uint8_t*p,uint8_t slot,Retirement&r){
  if(slot<4||slot>5||!retirement_tag(p)||p[6]!=slot||(p[7]&~uint8_t(7))||get16(p+20)!=36||get16(p+22)||get32(p+60)!=crc32(p,60)||!sequence(p)||sequence(p)>MAX_CHECKPOINT_COMMITS)return false;
  r=Retirement{};r.present_mask=p[7];r.generation=sequence(p);r.context_crc=get32(p+16);const uint8_t*q=p+HEADER;memcpy(r.journal,q,16);for(unsigned i=0;i<3;++i)r.record_crc[i]=get32(q+16+i*4);r.other_crc=get32(q+28);r.epoch=get32(q+32);return nonzero(r.journal,16)&&r.epoch>=1577836800u;
}
inline void increment(uint32_t&v){if(v<UINT32_MAX)++v;}
} // namespace halo_diag
