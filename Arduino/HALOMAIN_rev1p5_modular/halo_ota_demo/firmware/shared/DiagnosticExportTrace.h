#pragma once
#include "DiagnosticEnvelope.h"
namespace halo_diag {
// Last synchronous exporter decision in this boot; never policy authority.
// Identity, HTTP and ACK fields are absent until actually observed.
struct ExportTrace {
 enum class Why:uint8_t {NotCalled,PreSleepBudget,Deadline,PendingBoot,Busy,Wifi,LocalUnavailable,NoItem,Owner,Envelope,PostReturn,Ack,PeerQuery};
 uint64_t seq=0;uint32_t crc=0,left=0;int32_t http=0;uint8_t journal[16]{};
 Why why=Why::NotCalled;uint8_t slot=0,post=0,ack=0;
 bool attempted=false,record=false,post_seen=false,http_seen=false,handoff=false;
 static const char* name(Why w){static const char*const names[]={"not_called","pre_sleep_budget","deadline","pending_boot","busy","wifi","local_unavailable","no_item","owner","envelope","post_return","ack","peer_query"};return names[unsigned(w)];}
 void note(Why w,uint32_t now,uint32_t deadline){why=w;const int32_t n=int32_t(deadline-now);left=n>0?uint32_t(n):0;}
 void select(const uint8_t*context,const uint8_t*raw,uint8_t s,bool is_handoff){
  memcpy(journal,context+HEADER,16);seq=sequence(raw);crc=record_crc(raw,s);slot=s;handoff=is_handoff;record=true;post_seen=http_seen=false;ack=0;
 }
 void returned(uint8_t p,bool has_http,int32_t status){post=p;post_seen=true;http_seen=has_http;http=status;}
 void append(JsonObject&out)const{
  out["diag_export_decision"]=name(why);out["diag_export_attempted"]=attempted;out["diag_export_left_ms"]=left;
  if(record){char id[33];hex_into(id,journal,16);out["diag_export_journal"]=id;out["diag_export_seq"]=seq;out["diag_export_crc"]=crc;out["diag_export_slot"]=slot;out["diag_export_handoff"]=handoff;}
  if(post_seen)out["diag_export_result"]=post;
  if(http_seen)out["diag_export_http"]=http;
  if(ack)out["diag_export_ack"]=(ack==1?"committed":"not_committed");
 }
};
static_assert(sizeof(ExportTrace)<=64,"bounded volatile export trace");
}
