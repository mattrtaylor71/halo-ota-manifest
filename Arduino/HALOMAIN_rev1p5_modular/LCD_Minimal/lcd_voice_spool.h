#pragma once

// Voice-only SD transport. Included after lcd_ota_uart.h and before UART RX.
// Legacy photo spooling remains disabled. Existing generic binary/sleep flags
// also cover this transport so diagnostics, JSON and sleep cannot interrupt it.
#include "../halo_common/VoiceSpoolStore.h"
#ifndef LCD_VOICE_SPOOL_ENABLED
#define LCD_VOICE_SPOOL_ENABLED 1
#endif
#ifndef LCD_VOICE_SPOOL_DIR
#define LCD_VOICE_SPOOL_DIR "/sdcard/voice-spool-v1"
#endif

static halo_voice::Store g_voice_store(LCD_VOICE_SPOOL_DIR);
static bool g_voice_rx = false, g_voice_tx_pending = false;
static bool g_voice_tx_abort_pending = false;
static bool g_voice_waiting_abort = false;
static const char* g_voice_replay_result = "replay_failed";
static bool g_voice_replay_failed = true;
static uint32_t g_voice_started_ms = 0, g_voice_last_frame_ms = 0;
static halo_voice::Meta g_voice_transfer_meta, g_voice_last_meta;
static bool g_voice_have_last = false;
static UartOtaProtocol* g_voice_proto = nullptr;
static uint32_t g_voice_sd_committed = 0, g_voice_sd_failed = 0;
static uint32_t g_voice_sd_invalid = 0, g_voice_sd_incomplete = 0;
static constexpr uint32_t LCD_VOICE_XFER_MS = 90000;
static constexpr uint32_t LCD_VOICE_IDLE_MS = 4000;
static uint32_t g_voice_sd_work_started_ms = 0, g_voice_sd_work_budget_ms = 12000;
static bool lcd_voice_sd_budget() {
  return (uint32_t)(millis()-g_voice_sd_work_started_ms)<g_voice_sd_work_budget_ms;
}

static bool lcd_voice_sd_mount() {
#if !LCD_VOICE_SPOOL_ENABLED
  return false;
#else
  // Caller owns an outer lease through all raw POSIX operations. Unlike the
  // legacy lcd_sd_init(), this does not write/mount/scan the photo namespace.
  if(lcd_ota_in_progress_for_sd_guard())return false;
  return sd_card_Init()==ESP_OK && sd_card_is_mounted();
#endif
}
static bool lcd_voice_proto_ready() {
  // Reset the incremental parser at a newly negotiated idle-to-binary handoff;
  // a failed prior frame must not carry parser state into the next recording.
  delete g_voice_proto;g_voice_proto=new UartOtaProtocol(&senseSerial);
  if(!g_voice_proto||!g_voice_proto->valid())return false;
  g_voice_proto->quiet=true;return true;
}
static bool lcd_voice_reply(JsonDocument& d) {
  d["ver"]=PROTOCOL_VERSION;d["msg_id"]=get_next_msg_id();d["ts"]=(uint32_t)millis();
  d["voice_schema"]=halo_voice::kSchema;
  if(d.overflowed()||measureJson(d)>=1024){Serial.println("[VOICE_SD] reply_overflow");return false;}
  // Deliberately bypass ordinary-JSON suppression for the negotiated control
  // record; this function is called only by the exclusive UART task.
  serializeJson(d,senseSerial);senseSerial.print('\n');senseSerial.flush();return true;
}
static bool lcd_voice_copy(JsonVariantConst v,char* out,size_t cap,bool required=true) {
  if(!v.is<const char*>())return false;
  const char* s=v.as<const char*>();
  if(!s||strnlen(s,cap)>=cap||!halo_voice::bounded_text(s,cap,required))return false;
  strcpy(out,s);return true;
}
static bool lcd_voice_u32(JsonVariantConst v,uint32_t* out) {
  if(!v.is<uint32_t>())return false;*out=v.as<uint32_t>();return true;
}
static bool lcd_voice_parse_meta(JsonObjectConst d,halo_voice::Meta* m) {
  if(!m)return false;halo_voice::Meta v;
  const char* kind=d["kind"]|"";const char* fmt=d["pcm_fmt"]|"";
  if(strcmp(kind,"voice")||strcmp(fmt,"s16le_mono")||
     !lcd_voice_u32(d["voice_schema"],&v.schema)||!lcd_voice_u32(d["rate"],&v.rate)||
     !lcd_voice_u32(d["job_id"],&v.job_id)||!lcd_voice_u32(d["len"],&v.len)||
     !lcd_voice_u32(d["crc32"],&v.crc32)||!lcd_voice_u32(d["epoch"],&v.epoch)||
     !lcd_voice_u32(d["retries"],&v.retries)||
     !lcd_voice_copy(d["owner_id"],v.owner_id,sizeof(v.owner_id))||
     !lcd_voice_copy(d["device_id"],v.device_id,sizeof(v.device_id))||
     !lcd_voice_copy(d["session_id"],v.session_id,sizeof(v.session_id))||
     !lcd_voice_copy(d["request_id"],v.request_id,sizeof(v.request_id))||!halo_voice::valid(v))return false;
  *m=v;return true;
}
static void lcd_voice_put_meta(JsonObject d,const halo_voice::Meta& m) {
  d["voice_schema"]=m.schema;d["kind"]="voice";d["pcm_fmt"]="s16le_mono";d["rate"]=m.rate;
  d["owner_id"]=m.owner_id;d["device_id"]=m.device_id;d["session_id"]=m.session_id;
  d["request_id"]=m.request_id;d["job_id"]=m.job_id;d["len"]=m.len;d["crc32"]=m.crc32;
  d["epoch"]=m.epoch;d["retries"]=m.retries;
}
static void lcd_voice_echo(JsonDocument& d,const halo_voice::Meta& m) {
  d["request_id"]=m.request_id;d["job_id"]=m.job_id;d["len"]=m.len;d["crc32"]=m.crc32;
}
static bool lcd_voice_binding(JsonObjectConst d,char owner[64],char device[32],char request[33],bool need_request) {
  uint32_t schema=0;
  return lcd_voice_u32(d["voice_schema"],&schema)&&schema==halo_voice::kSchema&&
    lcd_voice_copy(d["owner_id"],owner,64)&&lcd_voice_copy(d["device_id"],device,32)&&
    (!need_request||(lcd_voice_copy(d["request_id"],request,33)&&halo_voice::request_valid(request)));
}
static bool lcd_voice_link_idle() {
  return !g_lcd_sleep_commit_gate.load()&&lcd_nvs_image_valid()&&
    !lcd_ota_in_progress_for_sd_guard()&&!g_img_rx_active&&!g_img_rx_binary_mode&&
    !g_spool_tx_pending&&!g_spool_tx_active&&!g_suppress_uart_json_tx;
}
static void lcd_voice_hold(bool receive,const halo_voice::Meta& m) {
  g_voice_transfer_meta=m;g_voice_last_meta=m;g_voice_have_last=true;
  g_voice_started_ms=g_voice_sd_work_started_ms;g_voice_last_frame_ms=millis();
  g_voice_tx_abort_pending=false;
  g_voice_rx=receive;g_voice_tx_pending=!receive;
  g_img_rx_active=receive;g_img_rx_binary_mode=receive;
  g_spool_tx_active=!receive;g_spool_tx_pending=!receive;
  g_suppress_uart_json_tx=true;
  if(g_sleep_transition)g_sleep_transition=false;
  // Keep panel/LVGL changes with their existing UI owner. Normal inactivity
  // may darken the screen while these flags keep the transport awake.
}
static void lcd_voice_release(const char* reason,bool failed) {
  g_voice_store.abort();g_voice_rx=false;g_voice_tx_pending=false;
  g_img_rx_active=false;g_img_rx_binary_mode=false;g_spool_tx_active=false;g_spool_tx_pending=false;
  g_suppress_uart_json_tx=false;
  g_voice_waiting_abort=false;
  lcd_media_release();
  if(failed)g_voice_sd_failed++;
  Serial.printf("[VOICE_SD] result=%s job=%lu bytes=%lu failures=%lu\n",reason,
    (unsigned long)g_voice_transfer_meta.job_id,(unsigned long)g_voice_store.received(),(unsigned long)g_voice_sd_failed);
}
static bool lcd_voice_expired() {
  return (uint32_t)(millis()-g_voice_started_ms)>=LCD_VOICE_XFER_MS||
    (uint32_t)(millis()-g_voice_last_frame_ms)>=LCD_VOICE_IDLE_MS;
}
static void lcd_voice_quarantine_tick() {
  if(g_voice_waiting_abort && g_spool_tx_active &&
     (uint32_t)(millis()-g_voice_started_ms)>=LCD_VOICE_XFER_MS) {
    // The FILE was already closed before waiting for ABORT. Do not hold the
    // LCD awake forever for lost proof: retire only the original transfer's
    // sleep custody. JSON suppression and media admission stay quarantined;
    // a bound late ABORT or normal sleep/reboot is required for recovery.
    g_spool_tx_active=false;
    Serial.println("[VOICE_SD] cleanup_deadline custody=retired json_quarantine=1 retained=1");
  }
}
static bool lcd_voice_abort_ack_last() {
  StaticJsonDocument<320> r;r["type"]="VOICE_XFER_ABORT_ACK";
  lcd_voice_echo(r,g_voice_last_meta);r["ok"]=1;r["json_ready"]=true;return lcd_voice_reply(r);
}
static bool lcd_voice_abort(JsonObjectConst d) {
  uint32_t schema=0,job=0,len=0,crc=0;
  const char* request=d["request_id"]|"";
  const bool match=g_voice_have_last&&lcd_voice_u32(d["voice_schema"],&schema)&&schema==halo_voice::kSchema&&
    !strcmp(request,g_voice_last_meta.request_id)&&lcd_voice_u32(d["job_id"],&job)&&job==g_voice_last_meta.job_id&&
    lcd_voice_u32(d["len"],&len)&&len==g_voice_last_meta.len&&lcd_voice_u32(d["crc32"],&crc)&&crc==g_voice_last_meta.crc32;
  if(!match)return false;
  // A foreground yield has already closed its FILE. Keep ordinary TX and UI
  // admission quarantined until this exact terminal proof is serialized.
  if(g_voice_waiting_abort) {
    if(lcd_voice_abort_ack_last())lcd_voice_release(g_voice_replay_result,g_voice_replay_failed);
    return true;
  }
  // Never use a stale voice control to clear OTA or another transfer's owner.
  // The send path still owns an open-stream pin: it closes that file before
  // releasing sleep/JSON ownership and acknowledging the abort.
  if(g_voice_tx_pending){g_voice_tx_abort_pending=true;return true;}
  if(g_voice_rx)lcd_voice_release("peer_abort",true);
  else if(!lcd_voice_link_idle())return false;
  lcd_voice_abort_ack_last();return true;
}

static bool lcd_voice_uart(JsonDocument& doc) {
  const char* type=doc["type"]|"";JsonObjectConst d=doc.as<JsonObjectConst>();
  if(!strcmp(type,"VOICE_XFER_ABORT")){lcd_voice_abort(d);return true;}
  const bool begin=!strcmp(type,"VOICE_XFER_BEGIN"),list=!strcmp(type,"VOICE_SPOOL_LIST_REQ"),
    fetch=!strcmp(type,"VOICE_SPOOL_FETCH"),del=!strcmp(type,"VOICE_SPOOL_DELETE"),attempt=!strcmp(type,"VOICE_SPOOL_ATTEMPT");
  if(!begin&&!list&&!fetch&&!del&&!attempt)return false;
  LcdMaintenanceStorageGuard admission_guard;
  StaticJsonDocument<2048> r;r["type"]=begin?"VOICE_XFER_READY":list?"VOICE_SPOOL_LIST":fetch?"VOICE_SPOOL_FETCH_READY":attempt?"VOICE_SPOOL_ATTEMPT_ACK":"VOICE_SPOOL_DELETE_ACK";
  halo_voice::Meta m;char owner[64]={},device[32]={},request[33]={};
  halo_voice::Result result=halo_voice::Result::Invalid;
  const bool parsed=begin?lcd_voice_parse_meta(d,&m):lcd_voice_binding(d,owner,device,request,!list);
  if(begin&&parsed)lcd_voice_echo(r,m);else if(request[0])r["request_id"]=request;
  if(!parsed){r["ok"]=0;r["reason"]="invalid";lcd_voice_reply(r);return true;}
  if(!lcd_voice_link_idle() || !lcd_media_try_claim(fetch||list,lcd_media_deferred_intent_pending())) {
    r["ok"]=0;r["reason"]="busy";r["json_ready"]=lcd_voice_link_idle();lcd_voice_reply(r);return true;
  }
  // Prevent either sleep route from entering while mounting/validating storage.
  g_img_rx_active=true;g_suppress_uart_json_tx=true;
  g_voice_sd_work_started_ms=millis();g_voice_sd_work_budget_ms=12000;
  {
    SdCardLease lease;
    if(!lease||!lcd_voice_sd_mount())result=halo_voice::Result::Io;
    else {
      g_voice_store.set_progress(lcd_freeze_wdt_feed);
      g_voice_store.set_budget(lcd_voice_sd_budget);
      if(begin)result=g_voice_store.begin(m);
      else if(list){halo_voice::Stats stats;char after[33]={};
        bool cursor_ok=!d.containsKey("after_request_id")||
          (lcd_voice_copy(d["after_request_id"],after,sizeof(after))&&halo_voice::request_valid(after));
        result=cursor_ok?g_voice_store.list(owner,device,&m,&stats,after):halo_voice::Result::Invalid;
        r["count"]=stats.count;r["invalid"]=stats.invalid;r["incomplete"]=stats.incomplete;
        g_voice_sd_invalid=stats.invalid;g_voice_sd_incomplete=stats.incomplete;
        r["request_id"]=(result==halo_voice::Result::Ok)?m.request_id:"";
        if(result==halo_voice::Result::Ok)r["epoch"]=m.epoch;}
      else if(fetch)result=g_voice_store.lookup(request,owner,device,&m);
      else {uint32_t len=0,crc=0,epoch=0;
        if(lcd_voice_u32(d["len"],&len)&&lcd_voice_u32(d["crc32"],&crc)){
          if(attempt&&lcd_voice_u32(d["epoch"],&epoch))result=g_voice_store.mark_attempt(request,owner,device,len,crc,epoch,&m);
          else if(del)result=g_voice_store.erase(request,owner,device,len,crc);
        }
      }
    }
  }
  bool ok=result==halo_voice::Result::Ok||result==halo_voice::Result::AlreadyStored||(list&&result==halo_voice::Result::Empty);
  const bool foreground_cancel=fetch && lcd_media_replay_cancelled();
  if(foreground_cancel)ok=false;
  if((begin&&result==halo_voice::Result::Ok)||(fetch&&ok)) {
    if(!lcd_voice_proto_ready()){g_voice_store.abort();ok=false;result=halo_voice::Result::Io;}
    else lcd_voice_hold(begin,m);
  }
  r["ok"]=ok?1:0;r["reason"]=foreground_cancel?"busy":halo_voice::result_name(result);
  if(begin)r["stored"]=result==halo_voice::Result::AlreadyStored?1:0;
  if(fetch&&ok){lcd_voice_echo(r,m);lcd_voice_put_meta(r.createNestedObject("meta"),m);}
  if(attempt&&ok){lcd_voice_echo(r,m);r["epoch"]=m.epoch;}
  if(begin||fetch)r["json_ready"]=!g_voice_rx&&!g_voice_tx_pending;
  if(!lcd_voice_reply(r)&&(g_voice_rx||g_voice_tx_pending))lcd_voice_release("reply_overflow",true);
  if(!g_voice_rx&&!g_voice_tx_pending) {
    g_img_rx_active=false;g_suppress_uart_json_tx=false;lcd_media_release();
  }
  Serial.printf("[VOICE_SD] control=%s result=%s job=%lu\n",type,halo_voice::result_name(result),(unsigned long)m.job_id);
  return true;
}

static bool lcd_voice_receive_loop() {
  if(!g_voice_rx)return false;
  lcd_freeze_wdt_feed();
  if(lcd_voice_expired()){lcd_voice_release("receive_timeout",true);return false;}
  uint8_t type=0;uint16_t seq=0;static uint8_t bytes[MAX_FRAME_SIZE];size_t n=sizeof(bytes);static char json[512];
  const auto event=g_voice_proto->recv_event(&type,&seq,bytes,&n,json,sizeof(json),100);
  if(event==UartOtaProtocol::TIMEOUT)return true;
  if(event==UartOtaProtocol::JSON){StaticJsonDocument<512>d;if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","VOICE_XFER_ABORT"))lcd_voice_abort(d.as<JsonObjectConst>());return g_voice_rx;}
  if(lcd_voice_expired()){lcd_voice_release("receive_deadline",true);return false;}
  if(type==MSG_IMG_NACK){lcd_voice_release("peer_nack",true);return false;}
  halo_voice::Result result=halo_voice::Result::Invalid;
  {
    SdCardLease lease;
    g_voice_sd_work_started_ms=millis();g_voice_sd_work_budget_ms=3000;
    if(!lease||!sd_card_is_mounted())result=halo_voice::Result::Io;
    else if(type==MSG_IMG_CHUNK)result=g_voice_store.append(seq,bytes,n);
    else if(type==MSG_IMG_END&&n==0)result=g_voice_store.finish(seq);
  }
  const bool ok=result==halo_voice::Result::Ok||result==halo_voice::Result::DuplicateChunk;
  if(!ok){g_voice_proto->send_frame(MSG_IMG_NACK,seq,nullptr,0);lcd_voice_release(halo_voice::result_name(result),true);return false;}
  if(lcd_voice_expired()){g_voice_proto->send_frame(MSG_IMG_NACK,seq,nullptr,0);lcd_voice_release("receive_deadline",true);return false;}
  g_voice_last_frame_ms=millis();
  if(type==MSG_IMG_END){uint8_t proof[40];uint8_t* p=proof;halo_voice::put32(p,g_voice_transfer_meta.len);halo_voice::put32(p,g_voice_transfer_meta.crc32);memcpy(p,g_voice_transfer_meta.request_id,32);
    g_voice_sd_committed++;g_voice_proto->send_frame(MSG_IMG_ACK,seq,proof,sizeof(proof));lcd_voice_release("committed",false);return false;}
  g_voice_proto->send_frame(MSG_IMG_ACK,seq,nullptr,0);return true;
}

// Every replay outcome joins the peer's existing bound ABORT after closing
// the FILE. Even a final END ACK proves payload receipt, not that the Sense has
// left its raw RX lease. No SD slot is erased. Missing proof keeps quarantine.
static void lcd_voice_finish_replay(uint16_t seq,const char* result,bool failed,bool nack,bool peer_abort=false) {
  g_voice_replay_result=result;g_voice_replay_failed=failed;
  g_voice_waiting_abort=true;g_voice_tx_pending=false;g_spool_tx_pending=false;
  if(peer_abort) {
    if(lcd_voice_abort_ack_last())lcd_voice_release(result,failed);
    return;
  }
  if(nack)g_voice_proto->send_frame(MSG_IMG_NACK,seq,nullptr,0);
  const uint32_t started=millis();
  static uint8_t bytes[MAX_FRAME_SIZE];static char json[512];
  while(g_voice_waiting_abort && (uint32_t)(millis()-started)<2500 &&
        (uint32_t)(millis()-g_voice_started_ms)<LCD_VOICE_XFER_MS) {
    uint8_t type=0;uint16_t got_seq=0;size_t n=sizeof(bytes);
    const auto event=g_voice_proto->recv_event(&type,&got_seq,bytes,&n,json,sizeof(json),100);
    lcd_freeze_wdt_feed();
    if(event==UartOtaProtocol::JSON) {
      StaticJsonDocument<512>d;
      if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","VOICE_XFER_ABORT"))
        lcd_voice_abort(d.as<JsonObjectConst>());
    }
  }
  Serial.printf("[VOICE_SD] cleanup=%s json_ready=%u retained=1\n",result,g_voice_waiting_abort?0:1);
}

static void lcd_voice_send_file() {
  if(!g_voice_tx_pending)return;
  // Keep pending true while streaming: it identifies the voice owner for ABORT.
  vTaskDelay(pdMS_TO_TICKS(120));
  const uint32_t drain_started=millis();
  const unsigned drain_limit=(unsigned)senseSerial.available();
  for(unsigned i=0;i<drain_limit&&i<1024&&(uint32_t)(millis()-drain_started)<20;++i){
    if(!senseSerial.available())break;senseSerial.read();
  }
  char path[halo_voice::kPathBytes];g_voice_store.payload_path(g_voice_transfer_meta.request_id,path,sizeof(path));
  FILE* f=nullptr;bool ok=false;bool foreground_yield=false;
  {SdCardLease lease;if(lease&&sd_card_is_mounted())ok=sd_open_file_for_read(path,&f)==ESP_OK&&f;}
  uint16_t seq=0;uint32_t sent=0;static uint8_t bytes[MAX_CHUNK_SIZE],reply[MAX_FRAME_SIZE];static char json[512];
  while(ok&&sent<g_voice_transfer_meta.len){
    if(lcd_media_replay_cancelled()){foreground_yield=true;ok=false;break;}
    lcd_freeze_wdt_feed();if(lcd_voice_expired()){ok=false;break;}
    size_t n=0;if(sd_read_chunk(f,bytes,sizeof(bytes),&n)!=ESP_OK||n==0||n>g_voice_transfer_meta.len-sent){ok=false;break;}
    if(!g_voice_proto->send_frame(MSG_IMG_CHUNK,seq,bytes,n)){ok=false;break;}
    bool ack=false;
    while(!lcd_voice_expired()){
      if(lcd_media_replay_cancelled()){foreground_yield=true;break;}
      uint8_t type=0;uint16_t rseq=0;size_t rn=sizeof(reply);
      auto event=g_voice_proto->recv_event(&type,&rseq,reply,&rn,json,sizeof(json),100);
      lcd_freeze_wdt_feed();
      if(event==UartOtaProtocol::TIMEOUT)continue;
      if(event==UartOtaProtocol::JSON){StaticJsonDocument<512>d;if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","VOICE_XFER_ABORT"))lcd_voice_abort(d.as<JsonObjectConst>());break;}
      ack=type==MSG_IMG_ACK&&rseq==seq&&rn==0;break;
    }
    if(!ack||!g_voice_tx_pending){ok=false;break;}
    sent+=(uint32_t)n;seq++;g_voice_last_frame_ms=millis();
  }
  if(f&&sd_close_file(f)!=ESP_OK)ok=false;
  if(g_voice_tx_abort_pending){
    g_voice_tx_abort_pending=false;lcd_voice_finish_replay(seq,"peer_abort",true,false,true);return;
  }
  if(foreground_yield||lcd_media_replay_cancelled()){lcd_voice_finish_replay(seq,"foreground_yield",false,true);return;}
  if(ok){uint8_t proof[40];uint8_t* p=proof;halo_voice::put32(p,g_voice_transfer_meta.len);halo_voice::put32(p,g_voice_transfer_meta.crc32);memcpy(p,g_voice_transfer_meta.request_id,32);
    ok=g_voice_proto->send_frame(MSG_IMG_END,seq,proof,sizeof(proof));
    bool ack=false;
    while(ok&&!lcd_voice_expired()){
      uint8_t type=0;uint16_t rseq=0;size_t rn=sizeof(reply);
      auto event=g_voice_proto->recv_event(&type,&rseq,reply,&rn,json,sizeof(json),100);lcd_freeze_wdt_feed();
      if(event==UartOtaProtocol::TIMEOUT)continue;
      if(event==UartOtaProtocol::FRAME)ack=type==MSG_IMG_ACK&&rseq==seq&&rn==0;
      else{StaticJsonDocument<512>d;if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","VOICE_XFER_ABORT"))lcd_voice_abort(d.as<JsonObjectConst>());}
      break;
    }ok=ok&&ack;
  }
  if(g_voice_tx_abort_pending){
    g_voice_tx_abort_pending=false;lcd_voice_finish_replay(seq,"peer_abort",true,false,true);return;
  }
  if(g_voice_tx_pending)lcd_voice_finish_replay(seq,ok?"replay_sent":"replay_failed",!ok,!ok);
}

static void lcd_voice_queue_diagnostic() {
  LcdMaintenanceStorageGuard admission_guard;
  if(!lcd_voice_link_idle()||!lcd_media_try_claim(false,lcd_media_deferred_intent_pending())){
    Serial.printf("[VOICE_QUEUE] schema=1 enabled=%d mounted=unknown result=busy\n",LCD_VOICE_SPOOL_ENABLED);return;
  }
  g_img_rx_active=true;g_suppress_uart_json_tx=true;
  g_voice_sd_work_started_ms=millis();g_voice_sd_work_budget_ms=12000;
  halo_voice::Stats stats;halo_voice::Result result=halo_voice::Result::Io;bool mounted=false;
  {
    SdCardLease lease;
    mounted=lease&&lcd_voice_sd_mount();
    if(mounted){g_voice_store.set_progress(lcd_freeze_wdt_feed);g_voice_store.set_budget(lcd_voice_sd_budget);result=g_voice_store.inventory(&stats);}
  }
  g_img_rx_active=false;g_suppress_uart_json_tx=false;
  lcd_media_release();
  Serial.printf("[VOICE_QUEUE] schema=1 enabled=%d mounted=%u result=%s pending=%lu incomplete=%lu corrupt=%lu elapsed_ms=%lu committed_this_boot=%lu failed_this_boot=%lu\n",
    LCD_VOICE_SPOOL_ENABLED,mounted?1:0,halo_voice::result_name(result),(unsigned long)stats.count,
    (unsigned long)stats.incomplete,(unsigned long)stats.invalid,(unsigned long)(millis()-g_voice_sd_work_started_ms),
    (unsigned long)g_voice_sd_committed,(unsigned long)g_voice_sd_failed);
}
