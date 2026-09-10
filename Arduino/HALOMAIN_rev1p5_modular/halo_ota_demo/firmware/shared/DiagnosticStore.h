#pragma once
#include "DiagnosticCapsule.h"

namespace halo_diag {
enum class Read:uint8_t { Missing,Ok,Error };
enum class Result:uint8_t { Ok,Empty,Disabled,Busy,Invalid,Corrupt,Uncertain,Full,Already,NotFound,Incomplete };
// Port contract: one exact typed blob per slot. Exact-slot retirement only. The
// board adapter must hold its existing NVS capacity lease for the entire call,
// authorize peak old+new footprint and preserve its essential cushion. No live
// adapter is enabled by this checkpoint. Native tests implement only this port.
struct Store {
  void* arg;
  bool qualified;
  bool (*acquire)(void*);
  void (*release)(void*);
  bool (*live)(void*); // same caller deadline/OTA/user ownership, never renewed
  Read (*read)(void*,uint8_t,uint8_t*,size_t,size_t*);
  bool (*write)(void*,uint8_t,const uint8_t*,size_t);
  bool (*commit)(void*);
  bool (*capacity)(void*,uint8_t,size_t);
  bool (*erase)(void*,uint8_t)=nullptr; // only Journal retirement, never namespace clear
};
inline Store disabled_store(){Store s{};return s;}
class Lease {
  Store&s_;bool held_;
 public:
  explicit Lease(Store&s):s_(s),held_(s.qualified&&s.acquire&&s.acquire(s.arg)){}
  ~Lease(){if(held_)s_.release(s_.arg);}
  explicit operator bool()const{return held_;}
};
struct Ack {uint8_t journal[16];uint64_t sequence;uint32_t crc;};
struct Watermark {uint64_t highest_committed,state_ack,checkpoint_generation;uint16_t pending,acknowledged;uint32_t dropped_full,dropped_io,coalesced,state_ack_crc;bool uncertainty,drops_may_be_lower_bound;};
struct Handoff {uint8_t journal[16];uint64_t sequence[3];uint32_t crc[3],context_crc,epoch,dropped_full,dropped_io;uint8_t source_slot;};
constexpr size_t HANDOFF_BYTES=72;
inline void encode_handoff(uint8_t*out,const Handoff&h){memset(out,0,HANDOFF_BYTES);memcpy(out,h.journal,16);for(unsigned i=0;i<3;++i){put64(out+16+12*i,h.sequence[i]);put32(out+24+12*i,h.crc[i]);}put32(out+52,h.context_crc);put32(out+56,h.epoch);put32(out+60,h.dropped_full);put32(out+64,h.dropped_io);out[68]=h.source_slot;}
inline bool decode_handoff(const uint8_t*p,size_t n,Handoff&h){if(!p||n!=HANDOFF_BYTES||p[69]||p[70]||p[71]||!nonzero(p,16)||get32(p+56)<1577836800u||p[68]<1||p[68]>3)return false;h=Handoff{};memcpy(h.journal,p,16);for(unsigned i=0;i<3;++i){h.sequence[i]=get64(p+16+12*i);h.crc[i]=get32(p+24+12*i);if(!h.sequence[i]&&h.crc[i])return false;}h.context_crc=get32(p+52);h.epoch=get32(p+56);h.dropped_full=get32(p+60);h.dropped_io=get32(p+64);h.source_slot=p[68];return h.sequence[h.source_slot-1]>1;}
inline uint32_t handoff_fingerprint(const Handoff&h){uint8_t bytes[HANDOFF_BYTES];encode_handoff(bytes,h);return crc32(bytes,sizeof(bytes));}
inline bool handoff_equal(const Handoff&a,const Handoff&b){return !memcmp(a.journal,b.journal,16)&&!memcmp(a.sequence,b.sequence,sizeof(a.sequence))&&!memcmp(a.crc,b.crc,sizeof(a.crc))&&a.context_crc==b.context_crc&&a.epoch==b.epoch&&a.dropped_full==b.dropped_full&&a.dropped_io==b.dropped_io&&a.source_slot==b.source_slot;}
class Journal {
  Store&s_;uint8_t*b_;Checkpoint cp_{};uint8_t cp_slot_=5;
  bool ready_=false,fault_=false,migrated_=false;uint32_t volatile_io_=0,volatile_full_=0;uint32_t expected_=0;
  Read read(uint8_t slot){if(!live())return Read::Error;size_t got=0;Read r=s_.read(s_.arg,slot,b_,slot_size(slot),&got);if(r==Read::Ok&&got!=slot_size(slot))return Read::Error;return r;}
  bool data_valid(uint8_t slot)const{
    if(!valid(b_,slot_size(slot),slot)||get32(b_+16)!=cp_.context_crc)return false;
    const uint8_t*p=b_+HEADER;
    if(slot<3)return get16(b_+20)==228&&nonzero(p,16)&&get64(p+16)&&text_ok((const char*)p+72,96)&&text_ok((const char*)p+216,12)&&get32(p+44)<=get32(p+48)&&get32(p+48)==expected_&&get16(p+36)>=1&&get16(p+36)<=uint16_t(Stage::ReportAfter)&&p[212]==slot-1&&p[213]<=3;
    if(slot==3)return get16(b_+20)==228&&get64(p)&&text_ok((const char*)p+20,96)&&get16(p+136)>=1&&get16(p+136)<=uint16_t(Stage::ReportAfter)&&p[138]<=1&&p[207]<=2&&p[208]<=3&&text_ok((const char*)p+140,12);
    return false;
  }
  Result failed(){fault_=true;increment(volatile_io_);return Result::Uncertain;}
  bool live()const{return s_.live&&s_.live(s_.arg);}
  bool write_checked(uint8_t slot){
    if(!live()||!s_.capacity(s_.arg,slot,slot_size(slot))||!live())return false;
    if(!s_.write(s_.arg,slot,b_,slot_size(slot))||!live()||!s_.commit(s_.arg)||!live())return false;
    uint8_t verify[MAX_RECORD];size_t got=0;
    return s_.read(s_.arg,slot,verify,slot_size(slot),&got)==Read::Ok&&got==slot_size(slot)&&memcmp(verify,b_,got)==0&&live();
  }
  Result checkpoint(Checkpoint next){
    if(cp_.generation>=MAX_CHECKPOINT_COMMITS||next.next_sequence>UINT32_MAX)return Result::Full;
    next.generation=cp_.generation+1;uint8_t slot=uint8_t(cp_slot_==4?5:4);
    encode_checkpoint(b_,slot,next);if(!write_checked(slot))return failed();cp_=next;cp_slot_=slot;return Result::Ok;
  }
  Result reserve(uint64_t&seq,bool state=false){
    if(cp_.next_sequence==UINT64_MAX)return Result::Full;
    if(state&&cp_.state_reservations>=MAX_STATE_RESERVATIONS)return overflow();
    seq=cp_.next_sequence;Checkpoint next=cp_;++next.next_sequence;if(state)++next.state_reservations;
    if(cp_.generation>=MAX_ORDINARY_CHECKPOINT_COMMITS)return overflow();
    return checkpoint(next); // reserve before data: a reset cannot reuse ID
  }
  Result writable()const{return fault_?Result::Uncertain:(ready_?Result::Ok:Result::Invalid);}
  Result append_done(uint8_t slot,uint64_t*seq){if(!write_checked(slot))return failed();if(seq)*seq=sequence(b_);return Result::Ok;}
  Result overflow(){increment(volatile_full_);return Result::Full;}
  Result inspect_handoff(Handoff&h){
    h=Handoff{};if(cp_.damage_flags||fault_)return Result::Corrupt;
    Read r=read(0);if(r==Read::Error)return failed();if(r!=Read::Ok||!valid_context(b_)||record_crc(b_,0)!=cp_.context_crc)return Result::Corrupt;
    memcpy(h.journal,b_+HEADER,16);h.context_crc=cp_.context_crc;h.epoch=cp_.handoff_epoch;h.dropped_full=cp_.dropped_full;h.dropped_io=cp_.dropped_io;
    if(!(cp_.ack_mask&1))return Result::Busy;uint64_t latest=0;
    for(uint8_t i=1;i<4;++i){r=read(i);if(r==Read::Error)return failed();if(r==Read::Missing){if((i<3&&(cp_.ack_mask&(1u<<i)))||(i==3&&cp_.state_reservations))return Result::Corrupt;continue;}
      if(!data_valid(i))return Result::Corrupt;const uint64_t n=sequence(b_);const uint32_t c=record_crc(b_,i);
      if(i==3?(n!=cp_.state_ack||c!=cp_.state_ack_crc):!(cp_.ack_mask&(1u<<i)))return Result::Busy;
      h.sequence[i-1]=n;h.crc[i-1]=c;if(n>latest){latest=n;h.source_slot=i;}}
    if(handoff_lifecycle(cp_.lifecycle)&&handoff_fingerprint(h)!=cp_.handoff_crc)return Result::Corrupt;
    return h.source_slot?Result::Ok:Result::Busy; // context-only cannot fabricate a captured-source handoff
  }
  bool erase_checked(uint8_t slot){
    if(!s_.erase||!live()||!s_.erase(s_.arg,slot)||!live()||!s_.commit(s_.arg)||!live())return false;
    return read(slot)==Read::Missing&&live();
  }
  Result resume_retirement(const Retirement&m,uint8_t marker){
    // Validate ALL still-present exact old bytes before any further erase.
    // Wrong/new context, a replaced record, malformed ordinary CP, or corruption
    // refuses; no generic interpretation of absence or "next export empty".
    bool ctx_present=false;Read r=read(0);if(r==Read::Error)return failed();
    if(r==Read::Ok){if(!valid_context(b_)||record_crc(b_,0)!=m.context_crc||memcmp(b_+HEADER,m.journal,16))return Result::Corrupt;ctx_present=true;}
    for(uint8_t i=1;i<4;++i){r=read(i);if(r==Read::Error)return failed();if(r==Read::Ok&&(!ctx_present||!(m.present_mask&(1u<<(i-1)))||!valid(b_,256,i)||get32(b_+16)!=m.context_crc||record_crc(b_,i)!=m.record_crc[i-1]))return Result::Corrupt;}
    const uint8_t other=marker==4?5:4;r=read(other);if(r==Read::Error)return failed();
    if(r==Read::Ok){Checkpoint c;if(!decode_checkpoint(b_,other,c)||c.context_crc!=m.context_crc||record_crc(b_,other)!=m.other_crc||c.lifecycle!=Lifecycle::HandoffAcked)return Result::Corrupt;}
    const uint8_t order[]={1,2,3,0,other};
    for(uint8_t slot:order){r=read(slot);if(r==Read::Error)return failed();if(r==Read::Ok&&!erase_checked(slot))return failed();}
    for(uint8_t slot:order)if(read(slot)!=Read::Missing)return failed();
    if(!erase_checked(marker))return failed();
    for(uint8_t i=0;i<6;++i)if(read(i)!=Read::Missing)return failed();
    ready_=false;migrated_=false;cp_=Checkpoint{};cp_slot_=5;return Result::Empty;
  }
 public:
  // scratch is exclusively borrowed during calls, not a second profile mirror.
  Journal(Store&s,uint8_t (&scratch)[MAX_RECORD]):s_(s),b_(scratch){}
  Result open(){
    if(fault_)return Result::Uncertain;
    ready_=false;volatile_io_=volatile_full_=0;
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;
    if(!live())return Result::Busy;
    // A durable retirement marker is interpreted BEFORE missing context or
    // ordinary checkpoint recovery. Never fall back around a corrupt marker.
    bool marker_found=false;Retirement marker{};uint8_t marker_slot=6;
    for(uint8_t i=4;i<6;++i){Read mr=read(i);if(mr==Read::Error)return failed();if(mr==Read::Ok&&retirement_tag(b_)){Retirement m;if(marker_found||!decode_retirement(b_,i,m)){fault_=true;return Result::Corrupt;}marker=m;marker_slot=i;marker_found=true;}}
    if(marker_found){Result rr=resume_retirement(marker,marker_slot);if(rr==Result::Corrupt)fault_=true;return rr;}
    Read r=read(0);if(r==Read::Error)return failed();
    if(r==Read::Missing){for(uint8_t i=1;i<SLOT_COUNT;++i){r=read(i);if(r==Read::Error)return failed();if(r!=Read::Missing){fault_=true;return Result::Corrupt;}}return Result::Empty;}
    if(!valid_context(b_)){fault_=true;return Result::Corrupt;}const uint32_t ctx=record_crc(b_,0);expected_=get32(b_+HEADER+168);
    bool have=false,occupied=false;unsigned sealed_copies=0;bool old_format=false,checkpoint_damage=false;Checkpoint best{};uint8_t chosen=5;
    for(uint8_t i=4;i<6;++i){r=read(i);if(r==Read::Error)return failed();Checkpoint c{};
      occupied|=r==Read::Ok;if(r==Read::Ok&&decode_checkpoint(b_,i,c)){
        if(c.context_crc!=ctx){fault_=true;return Result::Corrupt;}
        old_format|=b_[4]==VERSION;if(b_[4]==CHECKPOINT_VERSION&&c.lifecycle!=Lifecycle::Active)++sealed_copies;
        if(have&&c.generation==best.generation&&!checkpoint_equal(c,best)){fault_=true;return Result::Corrupt;}
        if(!have||c.generation>best.generation){best=c;chosen=i;have=true;}}else if(r==Read::Ok)checkpoint_damage=true;
    }
    if(!have){if(!occupied){for(uint8_t i=1;i<4;++i){r=read(i);if(r==Read::Error)return failed();if(r!=Read::Missing){fault_=true;return Result::Corrupt;}}return Result::Incomplete;}fault_=true;return Result::Corrupt;}cp_=best;cp_slot_=chosen;if(checkpoint_damage)cp_.damage_flags|=1;
    // Highest intact record is additional protection against an older recovered
    // checkpoint. Corrupt immutable slots remain occupied and never reused.
    uint64_t highest=1,seen[3]{};unsigned seen_count=0;uint16_t present=1;
    for(uint8_t i=1;i<4;++i){r=read(i);if(r==Read::Error)return failed();if(r==Read::Ok&&data_valid(i)){uint64_t n=sequence(b_);if(n==1){fault_=true;return Result::Corrupt;}for(unsigned k=0;k<seen_count;++k)if(seen[k]==n){fault_=true;return Result::Corrupt;}seen[seen_count++]=n;if(n>highest)highest=n;present|=uint16_t(1u<<i);}else if(r==Read::Ok)cp_.damage_flags|=1;}
    if(cp_.state_reservations && !(present&(1u<<3))&&cp_.lifecycle!=Lifecycle::OrphanClosing)cp_.damage_flags|=1;
    if(highest==UINT64_MAX){fault_=true;return Result::Full;}
    if(cp_.next_sequence<=highest)cp_.next_sequence=highest+1;
    // A lost/corrupt acknowledged record is evidence uncertainty; do not let
    // restored metadata manufacture a complete watermark or recycle its slot.
    if((cp_.ack_mask&~present)!=0){fault_=true;return Result::Corrupt;}
    migrated_=old_format||(cp_.lifecycle!=Lifecycle::Active&&sealed_copies<2);
    ready_=true;return Result::Ok;
  }
  Result initialize(const Context&ctx){
    if(fault_)return Result::Uncertain;
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;
    if(!live())return Result::Busy;
    bool existing_context=false;uint32_t existing_crc=0;
    for(uint8_t i=0;i<6;++i){Read r=read(i);if(r==Read::Error)return failed();if(r!=Read::Missing){if(i!=0)return Result::Already;if(!context_matches(b_,ctx))return Result::Invalid;existing_context=true;existing_crc=record_crc(b_,0);}}
    if(!encode_context(b_,ctx))return Result::Invalid;
    uint32_t crc=record_crc(b_,0);expected_=ctx.expected_bytes;if(existing_context){if(crc!=existing_crc)return Result::Invalid;}else if(!write_checked(0))return failed();
    cp_=Checkpoint{};migrated_=false;cp_.context_crc=crc;cp_.next_sequence=2;cp_slot_=5;
    Result r=checkpoint(cp_);ready_=r==Result::Ok;return r;
  }
  // Recover only never-started bootstrap metadata into an explicit CURRENT
  // journal-closure observation. This does not reconstruct an OTA attempt.
  // OrphanClosing is persisted before reservation, so interruption can resume
  // without reusing a sequence or attributing another campaign's work.
  Result close_orphan(const State&observation){
    if(observation.stage!=Stage::JournalBefore||observation.flags!=STATE_ORPHAN_CLOSURE)return Result::Invalid;
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(fault_)return Result::Uncertain;
    Read r=read(0);if(r==Read::Error)return failed();if(r!=Read::Ok||!valid_context(b_))return Result::Invalid;
    const uint32_t context_crc=record_crc(b_,0);expected_=get32(b_+HEADER+168);
    for(uint8_t i=1;i<3;++i){r=read(i);if(r==Read::Error)return failed();if(r!=Read::Missing)return Result::Busy;}
    bool have=false;Checkpoint best{};uint8_t chosen=5;
    for(uint8_t i=4;i<6;++i){r=read(i);if(r==Read::Error)return failed();if(r==Read::Missing)continue;Checkpoint c;
      if(!decode_checkpoint(b_,i,c)||c.context_crc!=context_crc||c.damage_flags||c.ack_mask||c.state_ack||c.handoff_epoch)return Result::Corrupt;
      if(c.lifecycle!=Lifecycle::OrphanClosing&&!(c.lifecycle==Lifecycle::Active&&c.generation==1&&c.next_sequence==2&&c.state_reservations==0))return Result::Busy;
      if(have&&c.generation==best.generation&&!checkpoint_equal(c,best))return Result::Corrupt;if(!have||c.generation>best.generation){best=c;chosen=i;have=true;}}
    if(have){cp_=best;cp_slot_=chosen;}else{cp_=Checkpoint{};cp_.context_crc=context_crc;cp_.next_sequence=2;cp_slot_=5;}
    r=read(3);if(r==Read::Error)return failed();const bool written=r==Read::Ok;
    if(written&&(!have||cp_.lifecycle!=Lifecycle::OrphanClosing||!data_valid(3)||get16(b_+HEADER+136)!=uint16_t(Stage::JournalBefore)||b_[HEADER+209]!=STATE_ORPHAN_CLOSURE||sequence(b_)>=cp_.next_sequence))return Result::Corrupt;
    ready_=true;migrated_=false;
    if(!written){
      // Close both legacy-readable copies before writing closure evidence.
      if(cp_.lifecycle!=Lifecycle::OrphanClosing){Checkpoint next=cp_;next.lifecycle=Lifecycle::OrphanClosing;Result rr=checkpoint(next);if(rr!=Result::Ok)return rr;}
      for(unsigned pass=0;pass<2;++pass){bool both=true;for(uint8_t i=4;i<6;++i){Read rr=read(i);if(rr==Read::Error)return failed();Checkpoint c;if(rr!=Read::Ok||!decode_checkpoint(b_,i,c)||b_[4]!=CHECKPOINT_VERSION||c.lifecycle!=Lifecycle::OrphanClosing)both=false;}
        if(both)break;Result rr=checkpoint(cp_);if(rr!=Result::Ok)return rr;}
      if(cp_.state_reservations>=MAX_STATE_RESERVATIONS||cp_.next_sequence==UINT32_MAX)return Result::Full;
      if(!encode_state(b_,3,cp_.next_sequence,context_crc,observation,0))return Result::Invalid;
      const uint64_t seq=cp_.next_sequence;Checkpoint next=cp_;++next.next_sequence;++next.state_reservations;Result rr=checkpoint(next);if(rr!=Result::Ok)return rr;
      if(!encode_state(b_,3,seq,context_crc,observation,0))return Result::Invalid;if(!write_checked(3))return failed();
    }
    Checkpoint next=cp_;next.lifecycle=Lifecycle::Sealed;return checkpoint(next);
  }
  bool orphan_closing()const{return ready_&&!fault_&&cp_.lifecycle==Lifecycle::OrphanClosing;}
  bool capture_enabled()const{return ready_&&!fault_&&!cp_.damage_flags&&!migrated_&&cp_.lifecycle==Lifecycle::Active;}
  bool sealed()const{return ready_&&(cp_.damage_flags||migrated_||cp_.lifecycle!=Lifecycle::Active);}
  Result seal(){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    if(cp_.lifecycle==Lifecycle::OrphanClosing)return Result::Busy;
    if(cp_.lifecycle==Lifecycle::Active){Checkpoint next=cp_;next.lifecycle=Lifecycle::Sealed;Result r=checkpoint(next);if(r!=Result::Ok)return r;}
    // Ensure BOTH copies fail closed in older schema3 readers, including a
    // power loss between the two writes. No mutable record can follow seal.
    for(unsigned pass=0;pass<2;++pass){bool both=true;for(uint8_t i=4;i<6;++i){Read r=read(i);if(r==Read::Error)return failed();Checkpoint c;
        if(r!=Read::Ok||!decode_checkpoint(b_,i,c)||b_[4]!=CHECKPOINT_VERSION||c.context_crc!=cp_.context_crc||c.lifecycle==Lifecycle::Active)both=false;}
      if(both){migrated_=false;return Result::Ok;}Result r=checkpoint(cp_);if(r!=Result::Ok)return r;}
    return failed();
  }
  Result prepare_handoff(uint32_t epoch){
    if(epoch<1577836800u)return Result::Invalid;if(!sealed())return Result::Busy;Result r=seal();if(r!=Result::Ok)return r;
    Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    Handoff h;r=inspect_handoff(h);if(r!=Result::Ok)return r;
    if(handoff_lifecycle(cp_.lifecycle))return Result::Already;
    Checkpoint next=cp_;next.lifecycle=Lifecycle::HandoffReady;next.handoff_epoch=epoch;h.epoch=epoch;next.handoff_crc=handoff_fingerprint(h);return checkpoint(next);
  }
  Result read_handoff(Handoff&h){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    if(migrated_||!handoff_lifecycle(cp_.lifecycle))return Result::Busy;return inspect_handoff(h);
  }
  // Exact cloud receipt validation is performed by DiagnosticHandoff; token is
  // rederived here under the SAME writer lease. UART receipt alone is invalid.
  Result acknowledge_handoff(const Handoff&expected){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    if(migrated_||!handoff_lifecycle(cp_.lifecycle))return Result::Busy;Handoff actual;Result r=inspect_handoff(actual);if(r!=Result::Ok)return r;if(!handoff_equal(expected,actual))return Result::Invalid;
    if(cp_.lifecycle==Lifecycle::HandoffAcked)return Result::Already;Checkpoint next=cp_;next.lifecycle=Lifecycle::HandoffAcked;return checkpoint(next);
  }
  Result retire_if_ready(){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    if(migrated_||cp_.lifecycle!=Lifecycle::HandoffAcked)return Result::Busy;Handoff h;Result r=inspect_handoff(h);if(r!=Result::Ok)return r;if(cp_.generation>=MAX_CHECKPOINT_COMMITS)return Result::Full;
    Retirement m{};m.generation=cp_.generation+1;m.context_crc=h.context_crc;m.epoch=h.epoch;memcpy(m.journal,h.journal,16);memcpy(m.record_crc,h.crc,sizeof(m.record_crc));for(unsigned i=0;i<3;++i)if(h.sequence[i])m.present_mask|=uint8_t(1u<<i);
    if(read(cp_slot_)!=Read::Ok)return failed();m.other_crc=record_crc(b_,cp_slot_);const uint8_t marker=cp_slot_==4?5:4;
    encode_retirement(b_,marker,m);if(!write_checked(marker))return failed();r=resume_retirement(m,marker);if(r==Result::Corrupt)fault_=true;return r;
  }
  Result failure(const Failure&f,uint64_t*seq=nullptr){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;
    if(writable()!=Result::Ok)return writable();if(!capture_enabled())return Result::Busy;if(f.attempt_ordinal>1||f.expected!=expected_)return Result::Invalid;
    uint8_t slot=uint8_t(1+f.attempt_ordinal);Read r=read(slot);if(r==Read::Error)return failed();
    if(r==Read::Ok){if(!data_valid(slot))return Result::Corrupt;if(memcmp(b_+HEADER,f.attempt_id,16))return overflow();return Result::Already;}
    if(!encode_failure(b_,slot,2,cp_.context_crc,f))return Result::Invalid;
    uint64_t n;Result rr=reserve(n);if(rr!=Result::Ok)return rr;
    if(f.expected!=expected_||!encode_failure(b_,slot,n,cp_.context_crc,f))return Result::Invalid;return append_done(slot,seq);
  }
  Result state(const State&s,uint64_t*seq=nullptr){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;
    if(writable()!=Result::Ok)return writable();if(!capture_enabled())return Result::Busy;if(!encode_state(b_,3,2,cp_.context_crc,s,0))return Result::Invalid;
    uint32_t coalesced=0;Read old=read(3);if(old==Read::Error)return failed();
    if(old==Read::Ok){if(!data_valid(3)){cp_.damage_flags|=1;return Result::Corrupt;}
      // The final timer is recorded after that boot's last network report.
      // Retain its exact source boot/build/time until end-to-end ACK; neither
      // Boot nor Admission on a later boot may silently erase that evidence.
      // Busy is observational only: no reservation, write or quota consumption.
      if(sequence(b_)>cp_.state_ack && get64(b_+HEADER)!=s.boot_id)return Result::Busy;
      coalesced=get32(b_+HEADER+224);if(sequence(b_)>cp_.state_ack)increment(coalesced);}
    uint64_t n;Result r=reserve(n,true);if(r!=Result::Ok)return r;
    if(!encode_state(b_,3,n,cp_.context_crc,s,coalesced))return Result::Invalid;return append_done(3,seq);
  }
  Result checkpoint_counters(){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    if(!capture_enabled())return Result::Busy;
    if(!volatile_full_&&!volatile_io_)return Result::Already;
    if(cp_.generation>=MAX_ORDINARY_CHECKPOINT_COMMITS)return Result::Full;
    Checkpoint next=cp_;uint64_t full=uint64_t(next.dropped_full)+volatile_full_,io=uint64_t(next.dropped_io)+volatile_io_;
    next.dropped_full=full>UINT32_MAX?UINT32_MAX:uint32_t(full);next.dropped_io=io>UINT32_MAX?UINT32_MAX:uint32_t(io);
    Result r=checkpoint(next);if(r==Result::Ok)volatile_full_=volatile_io_=0;return r;
  }
  // Caller passes ONLY a validated end-to-end receipt. Local UART receipt has
  // no API here. Journal identity+sequence+CRC must match retained exact bytes.
  Result acknowledge(const Ack&a){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    if(read(0)!=Read::Ok||!valid_context(b_))return failed();if(memcmp(a.journal,b_+HEADER,16))return Result::Invalid;
    uint8_t found=6;for(uint8_t i=0;i<4;++i){Read r=read(i);if(r==Read::Error)return failed();if(r==Read::Ok&&(i==0?valid_context(b_):data_valid(i))&&sequence(b_)==a.sequence&&record_crc(b_,i)==a.crc){found=i;break;}}
    if(found==6)return Result::NotFound;Checkpoint next=cp_;
    if(found==3){if(a.sequence<=next.state_ack)return Result::Already;next.state_ack=a.sequence;next.state_ack_crc=a.crc;}
    else{uint16_t bit=uint16_t(1u<<found);if(next.ack_mask&bit)return Result::Already;next.ack_mask|=bit;}
    return checkpoint(next);
  }
  Result acknowledge_pair(const Ack&a,uint32_t context_crc){
    if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    if(read(0)!=Read::Ok||!valid_context(b_))return failed();if(memcmp(a.journal,b_+HEADER,16)||record_crc(b_,0)!=context_crc)return Result::Invalid;
    uint8_t found=6;for(uint8_t i=1;i<4;++i){Read r=read(i);if(r==Read::Error)return failed();if(r==Read::Ok&&data_valid(i)&&sequence(b_)==a.sequence&&record_crc(b_,i)==a.crc){found=i;break;}}
    if(found==6)return Result::NotFound;Checkpoint next=cp_;next.ack_mask|=1;
    if(found==3){if(a.sequence>next.state_ack){next.state_ack=a.sequence;next.state_ack_crc=a.crc;}}else next.ack_mask|=uint16_t(1u<<found);
    if(checkpoint_equal(next,cp_))return Result::Already;return checkpoint(next);
  }
  Result next_export(uint8_t*out,size_t capacity,size_t&length,uint8_t&slot){
    length=0;if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    for(uint8_t i=1;i<4;++i){Read r=read(i);if(r==Read::Error)return failed();if(r!=Read::Ok||!data_valid(i))continue;
      bool ack=i==3?sequence(b_)<=cp_.state_ack:bool(cp_.ack_mask&(1u<<i));if(ack)continue;
      if(!out||capacity<slot_size(i))return Result::Invalid;memcpy(out,b_,slot_size(i));length=slot_size(i);slot=i;return Result::Ok;}return Result::Empty;
  }
  // Copies at most one retained record to caller-owned transport space. Zero
  // retire_if_ready is the only deletion path and requires a sealed cloud handoff.
  Result read_retained(uint8_t slot,uint8_t*out,size_t capacity,size_t&length){
    length=0;if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;
    if(fault_)return Result::Uncertain;if(slot>=4||capacity<slot_size(slot)||!out)return Result::Invalid;
    if(slot!=0&&writable()!=Result::Ok)return writable();
    Read r=read(slot);if(r==Read::Error)return failed();if(r==Read::Missing)return Result::Empty;
    if(!(slot==0?valid_context(b_):data_valid(slot)))return Result::Corrupt;
    memcpy(out,b_,slot_size(slot));length=slot_size(slot);return Result::Ok;
  }
  Result next_pending(uint8_t*out,size_t capacity,size_t&length,uint8_t&slot){
    length=0;if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(writable()!=Result::Ok)return writable();
    for(uint8_t i=0;i<4;++i){Read r=read(i);if(r==Read::Error)return failed();if(r!=Read::Ok)continue;
      if(!(i==0?valid_context(b_):data_valid(i)))continue;
      bool ack=(i==3)?sequence(b_)<=cp_.state_ack:(cp_.ack_mask&(1u<<i));if(ack)continue;
      if(capacity<slot_size(i))return Result::Invalid;memcpy(out,b_,slot_size(i));length=slot_size(i);slot=i;return Result::Ok;}
    return Result::Empty;
  }
  Result watermark(Watermark&w){
    w=Watermark{};w.drops_may_be_lower_bound=true;if(!s_.qualified)return Result::Disabled;Lease l(s_);if(!l)return Result::Busy;if(!ready_)return Result::Invalid;
    w.uncertainty=fault_||cp_.damage_flags;w.checkpoint_generation=cp_.generation;w.state_ack=cp_.state_ack;w.state_ack_crc=cp_.state_ack_crc;
    uint64_t full=uint64_t(cp_.dropped_full)+volatile_full_,io=uint64_t(cp_.dropped_io)+volatile_io_;
    w.dropped_full=full>UINT32_MAX?UINT32_MAX:uint32_t(full);w.dropped_io=io>UINT32_MAX?UINT32_MAX:uint32_t(io);
    for(uint8_t i=0;i<4;++i){Read r=read(i);if(r==Read::Error){w.uncertainty=true;return Result::Uncertain;}if(r!=Read::Ok){if(i==3&&cp_.state_reservations)w.uncertainty=true;continue;}
      if(!(i==0?valid_context(b_):data_valid(i))){w.uncertainty=true;continue;}
      if(i==3)w.coalesced=get32(b_+HEADER+224);
      uint64_t n=sequence(b_);if(n>w.highest_committed)w.highest_committed=n;
      bool ack=(i==3)?n<=cp_.state_ack:(cp_.ack_mask&(1u<<i));if(ack)++w.acknowledged;else++w.pending;}
    return w.uncertainty?Result::Uncertain:Result::Ok;
  }
};
static_assert(sizeof(Journal)<=96,"no full-profile RAM mirror");
} // namespace halo_diag
