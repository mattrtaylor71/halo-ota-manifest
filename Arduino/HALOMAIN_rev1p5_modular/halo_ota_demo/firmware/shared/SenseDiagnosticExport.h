#pragma once
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
#include <mbedtls/sha1.h>
#include "DiagnosticExportTrace.h"
static bool g_diag_export_attempted=false;
static halo_diag::ExportTrace g_diag_export_trace;
using DiagExportWhy=halo_diag::ExportTrace::Why;
static void sense_diag_export_note(DiagExportWhy why,uint32_t deadline){g_diag_export_trace.note(why,millis(),deadline);}
static void sense_diag_export_budget_skipped(uint32_t deadline){if(!g_diag_export_attempted)sense_diag_export_note(DiagExportWhy::PreSleepBudget,deadline);}
// Included after the frozen post_diagnostic transport and ordinary UART helper.
// This batch is optional, after OTA release, inside the existing pre-sleep
// deadline. It does not acquire a new wake/OTA lease or become policy authority.
static uint8_t g_diag_peer_discovery[88]{};
static bool g_diag_peer_discovery_valid=false;
static uint32_t g_diag_peer_discovery_boot=0,g_diag_peer_discovery_ms=0,g_diag_peer_discovery_epoch=0;
static bool sense_diag_export_idle(void*){
  return !g_ota_check_in_progress&&!g_ota_check_requested&&!g_boot_ota_pending&&
    !g_ota_apply_in_progress&&!g_lcd_ota_task_running&&!g_lcd_ota_proxy_owns_uart&&
    !g_spool_owns_uart&&!g_img_spool_tx_active&&!halo_primary_user_work_busy()&&
    !current_job.active&&!upload_inflight&&!http_inflight&&!foreground_active&&!voice_recording_active&&!halo_provisioning_active();
}
static uint32_t sense_diag_remaining(uint32_t deadline){const int32_t n=int32_t(deadline-millis());return n>0?uint32_t(n):0;}
static bool sense_diag_record_owner(const uint8_t*context,char(&owner)[64]){
  halo_diag::Context ctx{};owner[0]=0;if(!sense_diag_context_decode(context,ctx))return false;
  if(!halo_diag::nonzero(ctx.owner_binding_sha,32))return true;
  if(!ProvisioningState::loadOwnerId(owner,sizeof(owner))||!owner[0])return false;
  uint8_t binding[32];sense_diag_sha(owner,strlen(owner),ctx.device_mac,6,binding);return !memcmp(binding,ctx.owner_binding_sha,32);
}
static bool sense_diag_post_record(const uint8_t*context,const uint8_t*record,uint8_t slot,uint32_t deadline,halo_diag::EnvelopeIdentity&id,halo_diag::PostReceipt&receipt,const halo_diag::Handoff*handoff=nullptr){
  char owner[64]{};if(!sense_diag_remaining(deadline)){sense_diag_export_note(DiagExportWhy::Deadline,deadline);return false;}
  if(!sense_diag_export_idle(nullptr)){sense_diag_export_note(g_boot_ota_pending?DiagExportWhy::PendingBoot:DiagExportWhy::Busy,deadline);return false;}
  if(!sense_diag_record_owner(context,owner)){sense_diag_export_note(DiagExportWhy::Owner,deadline);return false;}
  if(handoff?!halo_diag::prepare_handoff_envelope(context,record,*handoff,id):!halo_diag::prepare_envelope(context,record,slot,id)){sense_diag_export_note(DiagExportWhy::Envelope,deadline);return false;}
  g_diag_export_trace.select(context,record,slot,handoff!=nullptr);
  const halo_diag::PostLease lease{nullptr,sense_diag_export_idle,deadline,OTA_REPORT_HTTP_TIMEOUT_MS,owner};
  const auto result=halo_diag::post_diagnostic(context,record,id,lease,receipt,handoff);
  g_diag_export_trace.returned(uint8_t(result),receipt.http_observed,receipt.status);sense_diag_export_note(DiagExportWhy::PostReturn,deadline);
  Serial.printf("[OTA_DIAG] export result=%u http=%d elapsed=%lu heap=%lu/%lu largest=%lu/%lu\n",unsigned(result),receipt.status,(unsigned long)receipt.elapsed_ms,(unsigned long)receipt.free_before,(unsigned long)receipt.free_after,(unsigned long)receipt.largest_before,(unsigned long)receipt.largest_after);
  return result==halo_diag::PostResult::Received&&sense_diag_remaining(deadline)&&sense_diag_export_idle(nullptr);
}
static bool sense_diag_local_ack(const halo_diag::EnvelopeIdentity&id,const halo_diag::PostReceipt&receipt,uint32_t deadline){
  const uint32_t left=sense_diag_remaining(deadline);if(!left)return false;char material[halo_diag::MAX_KEY_MATERIAL];uint8_t hash[20];
  if(!halo_diag::envelope_key_material(material,sizeof(material),id)||mbedtls_sha1((const uint8_t*)material,strlen(material),hash)!=0)return false;
  SenseDiagnosticScope scope(left);if(!scope.active||!sense_diag_safe(nullptr))return false;
  const auto r=halo_diag::acknowledge_envelope(g_diag_journal,id,hash,receipt.status,receipt.body,receipt.length);
  return r==halo_diag::Result::Ok||r==halo_diag::Result::Already;
}
static bool sense_diag_local_handoff_ack(const halo_diag::Handoff&handoff,const halo_diag::EnvelopeIdentity&id,const halo_diag::PostReceipt&receipt,uint32_t deadline){
  const uint32_t left=sense_diag_remaining(deadline);if(!left)return false;
  char material[halo_diag::MAX_KEY_MATERIAL];uint8_t hash[20];
  if(!halo_diag::envelope_key_material(material,sizeof(material),id)||mbedtls_sha1((const uint8_t*)material,strlen(material),hash)!=0)return false;
  SenseDiagnosticScope scope(left);if(!scope.active||!sense_diag_safe(nullptr))return false;
  const auto result=halo_diag::acknowledge_handoff_envelope(g_diag_journal,handoff,id,hash,receipt.status,receipt.body,receipt.length);
  return result==halo_diag::Result::Ok||result==halo_diag::Result::Already;
}
// Each phase owns only the data it needs. No frame containing later peer
// replies is live while a local record is inside TLS.
static __attribute__((noinline)) bool sense_diag_export_local(uint32_t deadline){
  sense_diag_flush_manifest(sense_diag_remaining(deadline));
  uint8_t context[256],record[256];size_t n=0;
  bool local_ready=false,local_context=false;
  {
    SenseDiagnosticScope scope(sense_diag_remaining(deadline));
    local_context=scope.active&&sense_diag_ensure_open()&&g_diag_journal.read_retained(0,context,sizeof(context),n)==halo_diag::Result::Ok;
    local_ready=local_context&&g_diag_ready;
  }
#if HALO_DURABLE_OTA_POLICY
  // A checked RESOLVED commit can precede a power loss before its optional RAM
  // close hook. Re-derive closure from the loaded authority on every cold drain.
  {halo_diag::Context retained;
  if(local_context&&sense_diag_context_decode(context,retained)&&
     halo_policy_diagnostic_close_retained(retained.campaign,retained.origin))
    sense_diag_close_campaign(retained.campaign,retained.origin);}
#endif
  sense_diag_service_seal(sense_diag_remaining(deadline));
  local_ready=local_context&&g_diag_ready; // orphan closure may have created its explicit current State
  // Each board owns its evidence. A local journal fault cannot prevent a
  // healthy LCD from forwarding its retained records within this same budget.
  // At most three immutable/coalesced records per board. No retry loop: a failed
  // POST/receipt/ACK leaves the exact source pending for a later normal wake.
  for(unsigned count=0;local_ready&&count<3&&sense_diag_remaining(deadline)>=500&&sense_diag_export_idle(nullptr);++count){
    uint8_t slot=0;halo_diag::Result next;
    {SenseDiagnosticScope scope(sense_diag_remaining(deadline));if(!scope.active)break;next=g_diag_journal.next_export(record,sizeof(record),n,slot);}
    if(next!=halo_diag::Result::Ok){sense_diag_export_note(next==halo_diag::Result::Empty?DiagExportWhy::NoItem:DiagExportWhy::LocalUnavailable,deadline);break;}
    halo_diag::EnvelopeIdentity id;halo_diag::PostReceipt receipt;
    if(!sense_diag_post_record(context,record,slot,deadline,id,receipt))return false;
    const bool acked=sense_diag_local_ack(id,receipt,deadline);g_diag_export_trace.ack=acked?1:2;sense_diag_export_note(DiagExportWhy::Ack,deadline);if(!acked)break;
  }
  // At most one additional handoff POST for this board. It is permitted only
  // for a closed journal, after every retained source has its durable cloud ACK.
  if(local_ready&&sense_diag_remaining(deadline)>=500&&sense_diag_export_idle(nullptr)){
    if(sense_diag_retire(sense_diag_remaining(deadline))==halo_diag::Result::Empty)local_ready=false;
    halo_diag::Handoff handoff{};bool have=false;
    if(local_ready){SenseDiagnosticScope scope(sense_diag_remaining(deadline));
      if(scope.active&&g_diag_ready&&g_diag_journal.sealed()){
        auto ready=g_diag_journal.read_handoff(handoff);
        if(ready==halo_diag::Result::Busy&&sense_time_has_fresh_sync()){
          ready=g_diag_journal.prepare_handoff(uint32_t(sense_now_epoch()));
          if(ready==halo_diag::Result::Ok||ready==halo_diag::Result::Already)ready=g_diag_journal.read_handoff(handoff);
        }
        have=ready==halo_diag::Result::Ok&&g_diag_journal.read_retained(handoff.source_slot,record,sizeof(record),n)==halo_diag::Result::Ok;
      }
    }
    if(have){halo_diag::EnvelopeIdentity id;halo_diag::PostReceipt receipt;
      if(!sense_diag_post_record(context,record,handoff.source_slot,deadline,id,receipt,&handoff))return false;
      const bool acked=sense_diag_local_handoff_ack(handoff,id,receipt,deadline);g_diag_export_trace.ack=acked?1:2;sense_diag_export_note(DiagExportWhy::Ack,deadline);if(acked)
        (void)sense_diag_retire(sense_diag_remaining(deadline));
    }
  }
  return true;
}
struct SenseDiagPeerIdentity {uint32_t boot=0;char fw[12]{};uint8_t mac[6]{},journal[16]{};};
// The one reply buffer is reused only after its boot/build/data have been copied
// to caller-owned identity/context. No callback retains this temporary storage.
static __attribute__((noinline)) bool sense_diag_peer_begin(uint32_t deadline,SenseDiagPeerIdentity&peer,uint8_t(&context)[256]){
  SenseDiagnosticReply reply;
  if(!sense_diag_exchange("info",0,nullptr,0,nullptr,0,0,deadline,reply)||reply.result!=halo_diag::Result::Ok||reply.size!=6)return false;
  peer.boot=reply.boot;memcpy(peer.fw,reply.fw,sizeof(peer.fw));memcpy(peer.mac,reply.bytes,6);
  if(sense_diag_exchange("discover",peer.boot,peer.fw,0,nullptr,0,0,deadline,reply)&&reply.result==halo_diag::Result::Ok&&reply.size==88){memcpy(g_diag_peer_discovery,reply.bytes,88);g_diag_peer_discovery_valid=true;g_diag_peer_discovery_boot=peer.boot;g_diag_peer_discovery_ms=millis();g_diag_peer_discovery_epoch=sense_now_epoch();}
  if(!sense_diag_read_record(peer.boot,peer.fw,0,deadline,reply)||reply.result!=halo_diag::Result::Ok||reply.size!=256)return false;
  halo_diag::Context decoded;if(!sense_diag_context_decode(reply.bytes,decoded)||decoded.board!=halo_diag::Board::Lcd||memcmp(decoded.device_mac,peer.mac,6))return false;
  memcpy(context,reply.bytes,256);memcpy(peer.journal,decoded.journal,16);
#if HALO_DURABLE_OTA_POLICY
  if(halo_policy_diagnostic_close_retained(decoded.campaign,decoded.origin)){
    uint8_t binding[20];memcpy(binding,decoded.journal,16);halo_diag::put32(binding+16,halo_diag::record_crc(context,0));
    (void)sense_diag_exchange("seal",peer.boot,peer.fw,0,binding,sizeof(binding),0,deadline,reply);
  }
#endif
  return !(sense_diag_exchange("retire",peer.boot,peer.fw,0,nullptr,0,0,deadline,reply)&&reply.result==halo_diag::Result::Empty);
}
// Receipt validation and UART ACK happen after POST returns, so their key and
// reply temporaries never occupy the TLS call chain. LCD still validates/owns ACK.
static __attribute__((noinline)) bool sense_diag_peer_ack(const SenseDiagPeerIdentity&peer,uint8_t slot,const char*op,const halo_diag::EnvelopeIdentity&id,const halo_diag::PostReceipt&receipt,uint32_t deadline){
  char material[halo_diag::MAX_KEY_MATERIAL],key[halo_diag::MAX_EVENT_KEY];uint8_t hash[20];
  if(!halo_diag::envelope_key_material(material,sizeof(material),id)||mbedtls_sha1((const uint8_t*)material,strlen(material),hash)!=0||!halo_diag::event_key(key,sizeof(key),id.epoch,hash)||!halo_diag::receipt_accepted(receipt.status,receipt.body,receipt.length,key))return false;
  SenseDiagnosticReply ack;
  const bool acked=sense_diag_exchange(op,peer.boot,peer.fw,slot,(const uint8_t*)receipt.body,receipt.length,receipt.status,deadline,ack)&&(ack.result==halo_diag::Result::Ok||ack.result==halo_diag::Result::Already);g_diag_export_trace.ack=acked?1:2;sense_diag_export_note(DiagExportWhy::Ack,deadline);return acked;
}
enum class SenseDiagPeerStep:uint8_t {Stop,Empty,More};
static __attribute__((noinline)) SenseDiagPeerStep sense_diag_peer_record(const SenseDiagPeerIdentity&peer,const uint8_t*context,uint32_t deadline){
  SenseDiagnosticReply next;if(!sense_diag_exchange("next",peer.boot,peer.fw,0,nullptr,0,0,deadline,next))return SenseDiagPeerStep::Stop;
  if(next.result==halo_diag::Result::Empty)return SenseDiagPeerStep::Empty;if(next.result!=halo_diag::Result::Ok||next.slot<1||next.slot>3)return SenseDiagPeerStep::Stop;
  const uint8_t slot=next.slot;if(!sense_diag_read_record(peer.boot,peer.fw,slot,deadline,next)||next.result!=halo_diag::Result::Ok)return SenseDiagPeerStep::Stop;
  halo_diag::EnvelopeIdentity id;halo_diag::PostReceipt receipt;
  if(!sense_diag_post_record(context,next.bytes,next.slot,deadline,id,receipt))return SenseDiagPeerStep::Stop;
  return sense_diag_peer_ack(peer,next.slot,"ack",id,receipt,deadline)?SenseDiagPeerStep::More:SenseDiagPeerStep::Stop;
}
static __attribute__((noinline)) void sense_diag_peer_handoff(const SenseDiagPeerIdentity&peer,const uint8_t*context,uint32_t deadline){
  uint8_t epoch[4];halo_diag::put32(epoch,sense_time_has_fresh_sync()?uint32_t(sense_now_epoch()):0);
  SenseDiagnosticReply source;
  if(!sense_diag_exchange("handoff",peer.boot,peer.fw,0,epoch,sizeof(epoch),0,deadline,source)||source.result!=halo_diag::Result::Ok||source.size!=halo_diag::HANDOFF_BYTES)return;
  halo_diag::Handoff handoff{};
  if(!halo_diag::decode_handoff(source.bytes,source.size,handoff)||memcmp(handoff.journal,peer.journal,16)||handoff.context_crc!=halo_diag::record_crc(context,0))return;
  if(!sense_diag_read_record(peer.boot,peer.fw,handoff.source_slot,deadline,source)||source.result!=halo_diag::Result::Ok)return;
  halo_diag::EnvelopeIdentity id;halo_diag::PostReceipt receipt;
  if(!sense_diag_post_record(context,source.bytes,handoff.source_slot,deadline,id,receipt,&handoff))return;
  if(!sense_diag_peer_ack(peer,handoff.source_slot,"h_ack",id,receipt,deadline))return;
  (void)sense_diag_exchange("retire",peer.boot,peer.fw,0,nullptr,0,0,deadline,source);
}
static __attribute__((noinline)) void sense_diag_export_peer(uint32_t deadline){
  if(sense_diag_remaining(deadline)<500||!sense_diag_bus_clear()||!sense_diag_export_idle(nullptr))return;
  sense_diag_export_note(DiagExportWhy::PeerQuery,deadline);
  SenseDiagPeerIdentity peer;uint8_t context[256];
  if(!sense_diag_peer_begin(deadline,peer,context))return;
  for(unsigned count=0;count<3&&sense_diag_remaining(deadline)>=500&&sense_diag_export_idle(nullptr);++count){
    const auto step=sense_diag_peer_record(peer,context,deadline);
    if(step==SenseDiagPeerStep::Stop)return;if(step==SenseDiagPeerStep::Empty)break;
  }
  if(sense_diag_remaining(deadline)<500||!sense_diag_export_idle(nullptr))return;
  sense_diag_peer_handoff(peer,context,deadline);
}
static __attribute__((noinline)) void sense_diag_export_retained(uint32_t deadline){
  if(g_diag_export_attempted)return;
  if(sense_diag_remaining(deadline)<500){sense_diag_export_note(DiagExportWhy::Deadline,deadline);return;}
  if(!sense_diag_export_idle(nullptr)){sense_diag_export_note(g_boot_ota_pending?DiagExportWhy::PendingBoot:DiagExportWhy::Busy,deadline);return;}
  if(!wifi_is_connected()){sense_diag_export_note(DiagExportWhy::Wifi,deadline);return;}
  g_diag_export_attempted=true;g_diag_export_trace.attempted=true;
  sense_diag_export_note(DiagExportWhy::LocalUnavailable,deadline);
  if(!sense_diag_export_local(deadline))return;
  sense_diag_export_peer(deadline);
}

static void sense_diag_report_discovery(JsonObject&payload){
  g_diag_export_trace.append(payload);
  payload["diag_schema"]=3;payload["diag_discovery_schema"]=1;payload["diag_s_ready"]=g_diag_ready?1:0;payload["diag_s_uncertain"]=g_diag_uncertain?1:0;
  payload["diag_s_qualification"]=unsigned(g_diag_adapter.last_qualification());
  if(g_diag_peer_qualification_boot){payload["diag_l_qualification"]=g_diag_peer_qualification;payload["diag_l_qualification_boot"]=g_diag_peer_qualification_boot;payload["diag_l_qualification_ms"]=g_diag_peer_qualification_ms;}
  if(g_diag_peer_discovery_valid){char hex[177];halo_diag::hex_into(hex,g_diag_peer_discovery,88);payload["diag_l_discovery"]=hex;payload["diag_l_peer_boot"]=g_diag_peer_discovery_boot;payload["diag_l_read_ms"]=g_diag_peer_discovery_ms;payload["diag_l_read_epoch"]=g_diag_peer_discovery_epoch;}
  SenseDiagnosticScope scope(250);if(!scope.active||!g_diag_ready||!sense_diag_safe(nullptr))return;
  halo_diag::Watermark w;uint8_t record[256],out[88]{};size_t n=0;
  if(g_diag_journal.watermark(w)!=halo_diag::Result::Ok||g_diag_journal.read_retained(0,record,sizeof(record),n)!=halo_diag::Result::Ok)return;
  memcpy(out,record+halo_diag::HEADER,16);
  for(uint8_t slot=1;slot<=3;++slot){auto r=g_diag_journal.read_retained(slot,record,sizeof(record),n);if(r==halo_diag::Result::Empty)continue;if(r!=halo_diag::Result::Ok)return;halo_diag::put64(out+16+(slot-1)*12,halo_diag::sequence(record));halo_diag::put32(out+24+(slot-1)*12,halo_diag::record_crc(record,slot));}
  halo_diag::put64(out+52,w.state_ack);halo_diag::put32(out+60,w.state_ack_crc);halo_diag::put64(out+64,w.highest_committed);halo_diag::put32(out+72,w.uncertainty?1:0);halo_diag::put32(out+76,w.dropped_full);halo_diag::put32(out+80,w.dropped_io);halo_diag::put32(out+84,w.coalesced);
  char hex[177];halo_diag::hex_into(hex,out,sizeof(out));payload["diag_s_discovery"]=hex;
}

#endif
