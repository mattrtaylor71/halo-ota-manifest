#pragma once
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
#include "DiagnosticHandoff.h"
#if HALO_DURABLE_OTA_POLICY
static bool halo_policy_diagnostic_peer_identity(const OtaManifest&,uint8_t(&)[16],char(&)[64]);
static bool halo_policy_diagnostic_close_retained(const uint8_t*,const char*);
#endif
// The existing Sense main-task UART pump is the only dispatcher. This borrowed
// reply pointer exists only during the synchronous exchange; no new reader/task.
struct SenseDiagnosticReply {char nonce[17]{},op[12]{},fw[12]{};uint32_t boot=0;halo_diag::Result result=halo_diag::Result::Invalid;uint8_t slot=0,bytes[256]{};size_t size=0,offset=0,total=0;bool received=false;
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_OTA_POLICY
uint8_t sw_reset=0,sw_wake=0,sw_quality=0;
#endif
};
static SenseDiagnosticReply* g_diag_uart_reply=nullptr;
static uint8_t g_diag_peer_qualification=0;
static uint32_t g_diag_peer_qualification_boot=0,g_diag_peer_qualification_ms=0;
static bool sense_diag_unhex(const char*s,uint8_t*out,size_t n){
  if(!s||strnlen(s,n*2+1)!=n*2)return false;auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
  for(size_t i=0;i<n;++i){int a=digit(s[2*i]),b=digit(s[2*i+1]);if(a<0||b<0)return false;out[i]=uint8_t(a*16+b);}return true;
}
static bool sense_diag_accept_uart(JsonDocument&doc){
  if(strcmp(doc["type"]|"","OTA_DIAG_REPLY"))return false;
  auto*r=g_diag_uart_reply;if(!r||r->received||strcmp(doc["nonce"]|"",r->nonce)||strcmp(doc["op"]|"",r->op))return true;
  if(!doc["peer_boot"].is<uint32_t>()||!doc["peer_boot"].as<uint32_t>()||(r->boot&&doc["peer_boot"].as<uint32_t>()!=r->boot))return true;
  const char*fw=doc["fw"]|"",*hex=doc["data"]|"";size_t n=strnlen(hex,514);
  if(!halo_diag::text_ok(fw,sizeof(r->fw))||(r->fw[0]&&strcmp(fw,r->fw))||n>512||(n&1)||!doc["result"].is<unsigned>()||doc["result"].as<unsigned>()>unsigned(halo_diag::Result::Incomplete)||!doc["slot"].is<unsigned>()||doc["slot"].as<unsigned>()>3)return true;
  if(n>256||!doc["offset"].is<unsigned>()||doc["offset"].as<unsigned>()>128||!doc["total"].is<unsigned>()||doc["total"].as<unsigned>()>256)return true;
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_OTA_POLICY
  if(!strcmp(r->op,"sleep")&&doc["result"].as<unsigned>()==unsigned(halo_diag::Result::Ok)){
    if(!doc["sw_reset"].is<unsigned>()||doc["sw_reset"].as<unsigned>()!=8||
       !doc["sw_wake"].is<unsigned>()||doc["sw_wake"].as<unsigned>()!=4||
       !doc["sw_quality"].is<unsigned>()||doc["sw_quality"].as<unsigned>()!=2)return true;
    r->sw_reset=8;r->sw_wake=4;r->sw_quality=2;
  }
#endif
  if(!sense_diag_unhex(hex,r->bytes,n/2))return true;r->offset=doc["offset"].as<unsigned>();r->total=doc["total"].as<unsigned>();r->size=n/2;r->slot=doc["slot"].as<unsigned>();r->result=(halo_diag::Result)doc["result"].as<unsigned>();r->boot=doc["peer_boot"].as<uint32_t>();memcpy(r->fw,fw,strlen(fw)+1);r->received=true;
  if(!strcmp(r->op,"info")&&doc["qualification"].is<unsigned>()&&doc["qualification"].as<unsigned>()<=unsigned(halo_diag::Qualification::RetirementIo)){
    g_diag_peer_qualification=doc["qualification"].as<unsigned>();g_diag_peer_qualification_boot=r->boot;g_diag_peer_qualification_ms=millis();
  }
  return true;
}
static bool sense_diag_bus_clear(){
  return sense_lcd_ota_retry_safe()&&!g_ota_apply_in_progress&&!g_lcd_ota_proxy_owns_uart&&!g_lcd_ota_task_running&&!g_spool_owns_uart&&!g_img_spool_tx_active&&
    !halo_primary_user_work_busy()&&!current_job.active&&!upload_inflight&&!http_inflight&&!foreground_active&&!voice_recording_active&&!halo_provisioning_active();
}
static bool sense_diag_exchange(const char*op,uint32_t peer,const char*fw,uint8_t slot,const uint8_t*data,size_t bytes,int status,uint32_t deadline,SenseDiagnosticReply&reply,unsigned offset=0){
  if(g_diag_uart_reply||!op||strnlen(op,12)>=12||bytes>256||(bytes&&!data)||(int32_t)(deadline-millis())<=0||!sense_diag_bus_clear())return false;
  reply={};uint8_t nonce[8];esp_fill_random(nonce,sizeof(nonce));halo_diag::hex_into(reply.nonce,nonce,sizeof(nonce));memcpy(reply.op,op,strlen(op)+1);reply.boot=peer;
  if(fw&&fw[0]){if(!halo_diag::text_ok(fw,sizeof(reply.fw)))return false;memcpy(reply.fw,fw,strlen(fw)+1);}
  char hex[513];halo_diag::hex_into(hex,data,bytes);JsonDocument doc;doc["ver"]=PROTOCOL_VERSION;doc["type"]="OTA_DIAG";doc["msg_id"]=get_next_msg_id();doc["ts"]=millis();doc["nonce"]=reply.nonce;doc["op"]=op;doc["peer_boot"]=peer;doc["slot"]=slot;doc["offset"]=offset;doc["bytes"]=bytes;doc["status"]=status;doc["data"]=hex;
  char wire[896];size_t needed=measureJson(doc);if(doc.overflowed()||needed>=sizeof(wire)||serializeJson(doc,wire,sizeof(wire))!=needed)return false;
  const uint32_t now=millis();if((int32_t)(deadline-now)<=0)return false;const uint32_t wait=(deadline-now)<1500?(deadline-now):1500;
  struct ReplyOwner {explicit ReplyOwner(SenseDiagnosticReply&r){g_diag_uart_reply=&r;}~ReplyOwner(){g_diag_uart_reply=nullptr;}}owner(reply);
  uart_send_json(wire);
  while(!reply.received&&(uint32_t)(millis()-now)<wait&&(int32_t)(deadline-millis())>0&&sense_diag_bus_clear()){pump_uart_rx_once();if(!reply.received)delay(1);}
  return reply.received&&(int32_t)(deadline-millis())>0&&sense_diag_bus_clear();
}
// At most two exact nonce-bound requests under one original caller deadline.
static bool sense_diag_read_record(uint32_t peer,const char*fw,uint8_t slot,uint32_t deadline,SenseDiagnosticReply&record){
  if(!sense_diag_exchange("read",peer,fw,slot,nullptr,0,0,deadline,record,0))return false;
  if(record.result!=halo_diag::Result::Ok)return true;
  if(record.slot!=slot||record.offset!=0||record.total!=256||record.size!=128)return false;
  SenseDiagnosticReply tail;if(!sense_diag_exchange("read",peer,fw,slot,nullptr,0,0,deadline,tail,128)||tail.result!=halo_diag::Result::Ok||tail.slot!=slot||tail.offset!=128||tail.total!=256||tail.size!=128)return false;
  memcpy(record.bytes+128,tail.bytes,128);record.size=256;
  return halo_diag::valid(record.bytes,256,slot);
}
static void sense_diag_lcd_context(const OtaManifest&manifest,const char*current_fw,uint32_t remaining){
  if(remaining<1||!sense_diag_bus_clear())return;
  const uint32_t budget=remaining<3500?remaining:3500,deadline=millis()+budget;
  halo_diag::Context ctx{};uint8_t raw[256],target[32];
#if HALO_DURABLE_OTA_POLICY
  // Policy owns the current campaign independently of either optional journal.
  if(!halo_policy_diagnostic_peer_identity(manifest,ctx.campaign,ctx.origin))return;
#else
  if(!sense_diag_capture_current())return;
  size_t n=0;SenseDiagnosticScope scope(budget);
  if(!scope.active||g_diag_journal.read_retained(0,raw,sizeof(raw),n)!=halo_diag::Result::Ok||
     !sense_diag_context_decode(raw,ctx))return;
#endif
  if(!sense_diag_hex(manifest.sha256,target)||!manifest.size||!halo_diag::text_ok(manifest.version,sizeof(ctx.target_version)))return;
  SenseDiagnosticReply info;if(!sense_diag_exchange("info",0,current_fw,0,nullptr,0,0,deadline,info)||info.result!=halo_diag::Result::Ok||info.size!=6)return;
  ctx.board=halo_diag::Board::Lcd;memcpy(ctx.device_mac,info.bytes,6);memcpy(ctx.target_sha,target,32);
  memset(ctx.target_version,0,sizeof(ctx.target_version));memcpy(ctx.target_version,manifest.version,strlen(manifest.version)+1);
  ctx.target_hash_kind=halo_diag::HashKind::ExpectedBin;ctx.expected_bytes=manifest.size;ctx.created_epoch=sense_now_epoch();
  // Each offer proposes a fresh journal. LCD retains its existing UUID when the
  // whole campaign matches; after retirement it must not reuse the prior UUID.
  esp_fill_random(ctx.journal,16);memset(ctx.owner_binding_sha,0,32);char owner[64]{};
  if(ProvisioningState::loadOwnerId(owner,sizeof(owner))&&owner[0])sense_diag_sha(owner,strlen(owner),ctx.device_mac,6,ctx.owner_binding_sha);
  if(!halo_diag::encode_context(raw,ctx))return;SenseDiagnosticReply accepted;
  const bool replied=sense_diag_exchange("context",info.boot,info.fw,0,raw,sizeof(raw),0,deadline,accepted);
  Serial.printf("[OTA_DIAG] peer_context replied=%u result=%u\n",replied?1:0,replied?unsigned(accepted.result):unsigned(halo_diag::Result::Busy));
}
#endif
