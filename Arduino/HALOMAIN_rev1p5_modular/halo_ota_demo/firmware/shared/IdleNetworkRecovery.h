#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace halo_idle_net {
static constexpr uint32_t PERIOD_MS=5000, SETUP_MARGIN_MS=500, REQUIRED_MS=PERIOD_MS+SETUP_MARGIN_MS;
static constexpr uint32_t COOLDOWN_S=21600, PROBE_COOLDOWN_S=180, MIN_EPOCH=1700000000u, MAX_EPOCH=4102444800u;
enum class Op:uint8_t {Ordinary=1,Diagnostic=2,Handoff=3,Admission=4,Probe=5};
enum class State:uint32_t {Empty=0,Active=0x41565731u,Returned=0x52545731u,Hold=0x48445731u};
enum class Why:uint8_t {None,Ready,Deadline,Busy,Cold,Corrupt,Cooldown,Clock,AddFailed,DeleteFailed};
struct alignas(4) Retained {uint32_t words[8];};
static_assert(sizeof(Retained)==32,"RTC recovery budget");
struct Record {uint32_t boot=0,epoch=0,until=0;Op op=Op::Ordinary;uint8_t reset=0;State state=State::Empty;uint32_t cooldown=COOLDOWN_S;};
inline uint32_t crc(const uint8_t*p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(unsigned b=0;b<8;++b)c=(c>>1)^(0xedb88320u&uint32_t(-int32_t(c&1)));}return ~c;}
inline bool epoch_ok(uint32_t n){return n>=MIN_EPOCH&&n<=MAX_EPOCH;}
inline State state(const Retained&r){return State(__atomic_load_n(&r.words[6],__ATOMIC_ACQUIRE));}
inline void mark(Retained&r,State s){__atomic_store_n(&r.words[6],uint32_t(s),__ATOMIC_RELEASE);}
inline void clear(Retained&r){mark(r,State::Empty);}
inline bool decode(const Retained&r,Record&o){
  const State s=state(r);if(s!=State::Active&&s!=State::Returned&&s!=State::Hold)return false;
  if(r.words[0]!=0x3144574eu||!r.words[1]||r.words[7]!=crc(reinterpret_cast<const uint8_t*>(r.words),24))return false;
  if(r.words[4]&0xffff0000u)return false;const uint8_t op=uint8_t(r.words[4]);
  if(op<1||op>5||(r.words[2]&&!epoch_ok(r.words[2]))||(r.words[3]&&!epoch_ok(r.words[3])))return false;
  if(r.words[5]&&(op!=uint8_t(Op::Probe)||r.words[5]!=PROBE_COOLDOWN_S))return false;
  o={r.words[1],r.words[2],r.words[3],Op(op),uint8_t(r.words[4]>>8),s,r.words[5]?r.words[5]:COOLDOWN_S};return true;
}
inline void encode(Retained&r,const Record&o){
  clear(r);r.words[0]=0x3144574eu;r.words[1]=o.boot;r.words[2]=o.epoch;r.words[3]=o.until;
  r.words[4]=uint32_t(o.op)|(uint32_t(o.reset)<<8);r.words[5]=o.cooldown==COOLDOWN_S?0:o.cooldown;
  r.words[7]=crc(reinterpret_cast<const uint8_t*>(r.words),24);mark(r,o.state);
}
struct Session {
  bool initialized=false,blocked_boot=false;uint32_t boot=0;Why why=Why::None;
  void begin(Retained&r,uint32_t current,uint8_t reset,bool allow_short_probe=false){
    if(initialized)return;initialized=true;boot=current;
    // Power-on/brownout/power-glitch/unknown cannot attest preserved RTC bytes.
    if(reset==0||reset==1||reset==9||reset==14){clear(r);why=Why::Cold;return;}
    Record old;if(!decode(r,old)){const bool residue=state(r)!=State::Empty;clear(r);why=Why::Corrupt;blocked_boot=residue;return;}
    if(old.boot==current){clear(r);why=Why::Corrupt;blocked_boot=true;return;}
    // A shipping/off reader never inherits a shortened bench-probe interval.
    if(old.cooldown!=COOLDOWN_S&&!allow_short_probe){old.cooldown=COOLDOWN_S;old.until=0;encode(r,old);}
    const bool wdt=reset==4||reset==5||reset==6||reset==7||reset==15;
    if(old.state==State::Active||(old.state==State::Returned&&wdt)){
      old.state=State::Hold;old.until=0;old.reset=reset;encode(r,old);blocked_boot=true;why=Why::Cooldown;
    }else if(old.state==State::Returned){clear(r);}
  }
  bool permit(Retained&r,bool fresh,uint32_t now){
    Record old;if(!decode(r,old))return !blocked_boot;
    if(old.state==State::Returned&&old.boot==boot)return true;
    if(old.state!=State::Hold)return false;
    if(!fresh||!epoch_ok(now)){why=Why::Clock;return false;}
    if(!old.until){old.until=now>MAX_EPOCH-old.cooldown?MAX_EPOCH:now+old.cooldown;old.epoch=now;encode(r,old);}
    if(blocked_boot||now<old.epoch||now<old.until||old.until==MAX_EPOCH){why=Why::Cooldown;return false;}
    clear(r);return true;
  }
};
// The adapter acknowledges enrolment once, then never feeds during guarded
// work/cleanup. Global timeout/panic/idle subscriptions are unchanged. A failed
// acknowledgement must clean up the exact user or retain ACTIVE and restart.
struct Port {void*arg;uint32_t(*millis)(void*);bool(*safe)(void*);bool(*add)(void*,void**);bool(*remove)(void*,void*);void(*restart)(void*);};
class Guard {
  Port p;Retained&r;void*user=nullptr;bool owned=false;
public:
  Why why=Why::None;
  Guard(Port port,Retained&rtc,Session&s,Op op,uint32_t deadline,bool fresh,uint32_t epoch,bool short_probe=false):p(port),r(rtc){
    if(!s.initialized||!s.boot||!p.safe(p.arg)){why=Why::Busy;return;}
    if(int32_t(deadline-p.millis(p.arg))<int32_t(REQUIRED_MS)){why=Why::Deadline;return;}
    if(!s.permit(r,fresh,epoch)){why=s.why;return;}
    // Stable header precedes subscription. Only the aligned control word changes
    // while the WDT can expire; a partial CRC rewrite cannot lose its tombstone.
    encode(r,{s.boot,fresh&&epoch_ok(epoch)?epoch:0,0,op,0,State::Active,op==Op::Probe&&short_probe?PROBE_COOLDOWN_S:COOLDOWN_S});
    if(!p.add(p.arg,&user)){clear(r);why=Why::AddFailed;return;}
    owned=true;why=(int32_t(deadline-p.millis(p.arg))<int32_t(PERIOD_MS)||!p.safe(p.arg))?Why::Deadline:Why::Ready;
  }
  Guard(const Guard&)=delete;Guard&operator=(const Guard&)=delete;
  bool active()const{return owned&&why==Why::Ready;}
  ~Guard(){
    if(!owned)return;
    // The ISR may have snapshotted failure before delete obtains its lock. Keep
    // this atomic RETURNED tombstone even after a successful removal.
    mark(r,State::Returned);
    if(!p.remove(p.arg,user)){why=Why::DeleteFailed;mark(r,State::Active);p.restart(p.arg);}
  }
};
}
