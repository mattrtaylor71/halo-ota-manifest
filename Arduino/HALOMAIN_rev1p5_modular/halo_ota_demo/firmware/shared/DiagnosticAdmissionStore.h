#pragma once
#include "DiagnosticAdmission.h"

namespace halo_admission {
enum class Read:uint8_t {Missing,Present,Error};
enum class Result:uint8_t {Stored,Already,Missing,Occupied,Declined,Corrupt,Io,Invalid,Acked};
// All callbacks execute under the SAME nonblocking existing NVS writer lease.
// authorize_capture must re-read and compare exact policy/context plus the
// prior-boot unACKed State condition. It grants no policy work. write performs
// one checked set/commit; erase is only the one dedicated key. uncertain is
// diagnostic-local, never the policy/reclaim uncertainty latch.
struct Port {
  void*arg;
  bool(*take)(void*);void(*give)(void*);
  bool(*live)(void*);
  bool(*authorize_capture)(void*,const Record&);
  Read(*read)(void*,uint8_t*,size_t*);
  bool(*capacity)(void*,size_t);
  bool(*write)(void*,const uint8_t*,size_t);
  bool(*erase)(void*);
  void(*uncertain)(void*);
};
struct Lease {
  Port&p;bool owned;
  explicit Lease(Port&x):p(x),owned(p.take&&p.take(p.arg)){}
  ~Lease(){if(owned)p.give(p.arg);}
};
class Store {
  Port p_;
  bool ready()const{return p_.take&&p_.give&&p_.live&&p_.authorize_capture&&p_.read&&p_.capacity&&p_.write&&p_.erase&&p_.uncertain;}
  Result io(){p_.uncertain(p_.arg);return Result::Io;}
public:
  explicit Store(Port p):p_(p){}
  Result capture(const Record&r){
    uint8_t expected[BYTES];if(!encode(r,expected))return Result::Invalid;
    if(!ready())return Result::Declined;Lease lease(p_);
    if(!lease.owned||!p_.live(p_.arg)||!p_.authorize_capture(p_.arg,r))return Result::Declined;
    uint8_t existing[BYTES];size_t n=0;const Read found=p_.read(p_.arg,existing,&n);
    if(found==Read::Error)return io();
    if(found==Read::Present){Record old;if(!decode(existing,n,old))return Result::Corrupt;
      return memcmp(existing,expected,BYTES)?Result::Occupied:Result::Already;}
    // Revalidate immediately before the only mutation, using the original
    // deadline. No replacement of an occupied record, including corrupt bytes.
    if(n||!p_.live(p_.arg)||!p_.authorize_capture(p_.arg,r)||
       !p_.capacity(p_.arg,BYTES)||!p_.live(p_.arg))return Result::Declined;
    if(!p_.write(p_.arg,expected,BYTES))return io();
    n=0;if(p_.read(p_.arg,existing,&n)!=Read::Present||n!=BYTES||memcmp(existing,expected,BYTES))return io();
    // Late successful storage is real; do not invent rollback or retry it.
    return Result::Stored;
  }
  Result read(uint8_t(&out)[BYTES]){
    if(!ready())return Result::Declined;Lease lease(p_);if(!lease.owned||!p_.live(p_.arg))return Result::Declined;
    size_t n=0;const Read found=p_.read(p_.arg,out,&n);if(found==Read::Error)return io();
    if(found==Read::Missing)return n?Result::Corrupt:Result::Missing;
    Record r;return decode(out,n,r)?Result::Already:Result::Corrupt;
  }
  // validator must validate the actual backend receipt for this exact retained
  //64B source and its full Context. A bare HTTP200/transport ACK is not enough.
  // The receipt/envelope factory is a separate required integration boundary.
  Result acknowledge(const uint8_t(&source)[BYTES],void*receipt,
                     bool(*validator)(void*,const uint8_t*,size_t)){
    Record r;if(!decode(source,BYTES,r)||!validator)return Result::Invalid;
    if(!ready())return Result::Declined;Lease lease(p_);if(!lease.owned||!p_.live(p_.arg))return Result::Declined;
    uint8_t current[BYTES];size_t n=0;const Read found=p_.read(p_.arg,current,&n);
    if(found==Read::Error)return io();if(found==Read::Missing)return n?Result::Corrupt:Result::Missing;
    Record old;if(!decode(current,n,old))return Result::Corrupt;
    if(memcmp(source,current,BYTES))return Result::Occupied;
    if(!validator(receipt,current,BYTES)||!p_.live(p_.arg))return Result::Declined;
    if(!p_.erase(p_.arg))return io();
    n=0;const Read after=p_.read(p_.arg,current,&n);
    if(after!=Read::Missing||n)return io();return Result::Acked;
  }
};
} // namespace halo_admission
