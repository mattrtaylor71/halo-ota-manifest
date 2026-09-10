// Host-only declared test-state preparation; never linked into firmware.
#include "../halo_ota_demo/firmware/shared/DurableOtaPolicy.h"
#include "../halo_ota_demo/firmware/shared/NightlySchedule.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <climits>

using namespace durable_ota;
static bool number(const char* s,uint32_t& value) {
  if(!s||!*s)return false;
  for(const char*p=s;*p;++p)if(*p<'0'||*p>'9')return false;
  errno=0;char*end=nullptr;unsigned long long n=strtoull(s,&end,10);
  if(errno||*end||n>UINT32_MAX)return false;value=uint32_t(n);return true;
}
static bool version(const char*s,uint32_t(&v)[3]) {
  if(!s||!*s)return false;
  for(int i=0;i<3;++i){char part[12]={};unsigned n=0;
    while(*s&&*s!='.'){if(n>=10)return false;part[n++]=*s++;}
    if(!number(part,v[i]))return false;
    if(i<2){if(*s++!='.')return false;}else if(*s)return false;
  }return true;
}
static bool newer(const char*a,const char*b) {
  uint32_t x[3],y[3];if(!version(a,x)||!version(b,y))return false;
  for(int i=0;i<3;++i)if(x[i]!=y[i])return x[i]>y[i];return false;
}
static int fail(){std::fputs("policy fixture refused\n",stderr);return 2;}
int main(int argc,char**argv) {
  if(argc==4&&!std::strcmp(argv[1],"calendar")){
    uint32_t now=0;if(!number(argv[2],now)||now<kMinimumEpoch||setenv("TZ",argv[3],1))return fail();
    tzset();uint32_t delta=halo_seconds_until_maintenance(time_t(now));
    if(!delta||uint64_t(now)+delta>UINT32_MAX)return fail();
    std::printf("%u\n",now+delta);return 0;
  }
  uint8_t bytes[kRecordBytes];Record r{};
  if(argc<2||std::fread(bytes,1,sizeof(bytes),stdin)!=sizeof(bytes)||std::fgetc(stdin)!=EOF||!decode(bytes,sizeof(bytes),r))return fail();
  if(!std::strcmp(argv[1],"inspect")&&argc==2){
    std::printf("{\"generation\":%u,\"phase\":%u,\"budget_day\":%u,\"budget_granted\":%u,\"high_water\":%u,\"not_before\":%u,\"work_remaining_ms\":%u,\"reserved_work_ms\":%u,\"network_windows\":%u,\"day_attempts\":%u}\n",r.generation,unsigned(r.phase),r.budget_day,r.budget_granted,r.high_water,r.not_before,r.work_remaining_ms,r.reserved_work_ms,unsigned(r.network_windows),unsigned(r.day_attempts));return 0;
  }
  uint32_t now=0,due=0;
  if(argc!=5||std::strcmp(argv[1],"exhausted-yesterday")||!number(argv[2],now)||!number(argv[3],due))return fail();
  if(now<kMinimumEpoch+86400UL||uint64_t(due)<=uint64_t(now)+60||uint64_t(due)>uint64_t(now)+26*3600||due/86400UL!=now/86400UL)return fail();
  if(r.phase!=Phase::DEFERRED||active_phase(r)||r.bench.state!=BenchState::NONE||r.one_shot.phase!=OneShotPhase::NONE||r.generation==UINT32_MAX||!newer(r.target.version,argv[4])||!newer(r.target.peer_version,argv[4]))return fail();
  // This is a NEW synthetic prior-state case, never an in-flight debt refund.
  const uint32_t end_yesterday=(now/86400UL)*86400UL-1;
  r.generation++;r.created=end_yesterday-1200;r.high_water=end_yesterday;
  r.budget_granted=r.created;r.budget_day=r.created/86400UL;
  r.fast_start=r.fast_due=r.fast_expiry=0;r.not_before=due;
  r.work_remaining_ms=r.reserved_work_ms=r.active_started=r.active_deadline=0;
  r.arm_epoch=r.arm_peer_boot=0;std::memset(r.arm_id,0,sizeof(r.arm_id));
  r.deferred_path=true;r.fast_opportunities=2;r.network_windows=network_cap(r);r.day_attempts=apply_cap(r);
  r.begins[0]=r.begins[1]=begin_cap(r);r.attempt_begins[0]=r.attempt_begins[1]=2;
  if(r.attempt_ordinal<2)r.attempt_ordinal=2;
  Record rolled{},manual{};
  if(!shape(r)||!encode(r,bytes)||!rollover(r,Clock{due,true,true},rolled)||rolled.work_remaining_ms!=kDailyWorkMs||rollover(r,Clock{due,true,false},manual))return fail();
  return std::fwrite(bytes,1,sizeof(bytes),stdout)==sizeof(bytes)?0:2;
}
