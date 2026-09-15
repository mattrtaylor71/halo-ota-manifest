#pragma once

// Image-only SD transport. Included after lcd_ota_uart.h and before UART RX.
// Legacy photo spooling remains disabled. Existing generic binary/sleep flags
// also cover this transport so diagnostics, JSON and sleep cannot interrupt it.
#include "../halo_common/ImageSpoolStore.h"
#ifndef LCD_IMAGE_SPOOL_ENABLED
#define LCD_IMAGE_SPOOL_ENABLED 1
#endif
#ifndef LCD_IMAGE_SPOOL_DIR
#define LCD_IMAGE_SPOOL_DIR "/sdcard/image-spool-v1"
#endif

static halo_image::Store g_image_store(LCD_IMAGE_SPOOL_DIR);
static bool g_image_rx = false, g_image_tx_pending = false;
static bool g_image_tx_abort_pending = false;
static bool g_image_waiting_abort = false;
static const char* g_image_replay_result = "replay_failed";
static bool g_image_replay_failed = true;
static uint32_t g_image_started_ms = 0, g_image_last_frame_ms = 0;
static halo_image::Meta g_image_transfer_meta, g_image_last_meta;
static bool g_image_have_last = false;
static UartOtaProtocol* g_image_proto = nullptr;
static uint32_t g_image_sd_committed = 0, g_image_sd_failed = 0;
static uint32_t g_image_sd_invalid = 0, g_image_sd_incomplete = 0;
static constexpr uint32_t LCD_IMAGE_XFER_MS = 90000;
static constexpr uint32_t LCD_IMAGE_IDLE_MS = 4000;
static uint32_t g_image_sd_work_started_ms = 0, g_image_sd_work_budget_ms = 12000;
static bool lcd_image_sd_budget() {
  return (uint32_t)(millis()-g_image_sd_work_started_ms)<g_image_sd_work_budget_ms;
}

static bool lcd_image_sd_mount() {
#if !LCD_IMAGE_SPOOL_ENABLED
  return false;
#else
  // Caller owns an outer lease through all raw POSIX operations. Unlike the
  // legacy lcd_sd_init(), this does not write/mount/scan the photo namespace.
  if(lcd_ota_in_progress_for_sd_guard())return false;
  return sd_card_Init()==ESP_OK && sd_card_is_mounted();
#endif
}
static bool lcd_image_proto_ready() {
  // Reset the incremental parser at a newly negotiated idle-to-binary handoff;
  // a failed prior frame must not carry parser state into the next image.
  delete g_image_proto;g_image_proto=new UartOtaProtocol(&senseSerial);
  if(!g_image_proto||!g_image_proto->valid())return false;
  g_image_proto->quiet=true;return true;
}
static bool lcd_image_reply(JsonDocument& d) {
  d["ver"]=PROTOCOL_VERSION;d["msg_id"]=get_next_msg_id();d["ts"]=(uint32_t)millis();
  d["image_schema"]=halo_image::kSchema;
  if(d.overflowed()||measureJson(d)>=1024){Serial.println("[IMAGE_SD] reply_overflow");return false;}
  // Deliberately bypass ordinary-JSON suppression for the negotiated control
  // record; this function is called only by the exclusive UART task.
  serializeJson(d,senseSerial);senseSerial.print('\n');senseSerial.flush();return true;
}
static bool lcd_image_copy(JsonVariantConst v,char* out,size_t cap,bool required=true) {
  if(!v.is<const char*>())return false;
  const char* s=v.as<const char*>();
  if(!s||strnlen(s,cap)>=cap||!halo_image::bounded_text(s,cap,required))return false;
  strcpy(out,s);return true;
}
static bool lcd_image_u32(JsonVariantConst v,uint32_t* out) {
  if(!v.is<uint32_t>())return false;*out=v.as<uint32_t>();return true;
}
static bool lcd_image_i32(JsonVariantConst v,int32_t* out) {
  if(!v.is<int32_t>())return false;*out=v.as<int32_t>();return true;
}
static bool lcd_image_parse_meta(JsonObjectConst d,halo_image::Meta* m) {
  if(!m)return false;halo_image::Meta v;
  const char* kind=d["kind"]|"";const char* fmt=d["content_type"]|"";
  JsonObjectConst c=d["cam"].as<JsonObjectConst>();
  if(strcmp(kind,"image")||strcmp(fmt,"image/jpeg")||c.isNull()||
     !lcd_image_u32(d["image_schema"],&v.schema)||!lcd_image_u32(d["job_id"],&v.job_id)||
     !lcd_image_u32(d["len"],&v.len)||!lcd_image_u32(d["crc32"],&v.crc32)||
     !lcd_image_u32(d["epoch"],&v.epoch)||!lcd_image_u32(d["retries"],&v.retries)||
     !lcd_image_u32(d["qty"],&v.quantity)||!d["add_list"].is<bool>()||
     !lcd_image_copy(d["owner_id"],v.owner_id,sizeof(v.owner_id))||
     !lcd_image_copy(d["device_id"],v.device_id,sizeof(v.device_id))||
     !lcd_image_copy(d["request_id"],v.request_id,sizeof(v.request_id))||
     !lcd_image_copy(d["checksum_sha256"],v.checksum_sha256,sizeof(v.checksum_sha256))||
     !lcd_image_copy(d["mode"],v.mode,sizeof(v.mode))||!lcd_image_copy(d["expiry"],v.expiry,sizeof(v.expiry),false)||
     !lcd_image_u32(c["p"],&v.camera.profile)||!lcd_image_u32(c["f"],&v.camera.flash_enabled)||
     !lcd_image_u32(c["q"],&v.camera.jpeg_quality)||!lcd_image_u32(c["w"],&v.camera.actual_width)||
     !lcd_image_u32(c["h"],&v.camera.actual_height)||!lcd_image_u32(c["fs"],&v.camera.configured_framesize)||
     !lcd_image_i32(c["l"],&v.camera.scene_luma)||!lcd_image_i32(c["g"],&v.camera.scene_green_ratio)||
     !lcd_image_u32(c["x"],&v.camera.xclk_hz))return false;
  v.add_to_shopping_list=d["add_list"].as<bool>()?1:0;
  if(!halo_image::valid(v))return false;*m=v;return true;
}
static void lcd_image_put_meta(JsonObject d,const halo_image::Meta& m) {
  d["image_schema"]=m.schema;d["kind"]="image";d["content_type"]="image/jpeg";
  d["owner_id"]=m.owner_id;d["device_id"]=m.device_id;d["request_id"]=m.request_id;
  d["checksum_sha256"]=m.checksum_sha256;
  d["job_id"]=m.job_id;d["len"]=m.len;d["crc32"]=m.crc32;d["epoch"]=m.epoch;d["retries"]=m.retries;
  d["mode"]=m.mode;d["expiry"]=m.expiry;d["qty"]=m.quantity;d["add_list"]=m.add_to_shopping_list!=0;
  JsonObject c=d.createNestedObject("cam");
  c["p"]=m.camera.profile;c["f"]=m.camera.flash_enabled;c["q"]=m.camera.jpeg_quality;
  c["w"]=m.camera.actual_width;c["h"]=m.camera.actual_height;c["fs"]=m.camera.configured_framesize;
  c["l"]=m.camera.scene_luma;c["g"]=m.camera.scene_green_ratio;c["x"]=m.camera.xclk_hz;
}
static void lcd_image_echo(JsonDocument& d,const halo_image::Meta& m) {
  d["request_id"]=m.request_id;d["job_id"]=m.job_id;d["len"]=m.len;d["crc32"]=m.crc32;
}
static bool lcd_image_binding(JsonObjectConst d,char owner[64],char device[32],char request[33],bool need_request) {
  uint32_t schema=0;
  return lcd_image_u32(d["image_schema"],&schema)&&schema==halo_image::kSchema&&
    lcd_image_copy(d["owner_id"],owner,64)&&lcd_image_copy(d["device_id"],device,32)&&
    (!need_request||(lcd_image_copy(d["request_id"],request,33)&&halo_image::request_valid(request)));
}
static bool lcd_image_link_idle() {
  return !g_lcd_sleep_commit_gate.load()&&lcd_nvs_image_valid()&&
    !lcd_ota_in_progress_for_sd_guard()&&!g_img_rx_active&&!g_img_rx_binary_mode&&
    !g_spool_tx_pending&&!g_spool_tx_active&&!g_suppress_uart_json_tx;
}
static void lcd_image_hold(bool receive,const halo_image::Meta& m) {
  g_image_transfer_meta=m;g_image_last_meta=m;g_image_have_last=true;
  g_image_started_ms=g_image_sd_work_started_ms;g_image_last_frame_ms=millis();
  g_image_tx_abort_pending=false;
  g_image_rx=receive;g_image_tx_pending=!receive;
  g_img_rx_active=receive;g_img_rx_binary_mode=receive;
  g_spool_tx_active=!receive;g_spool_tx_pending=!receive;
  g_suppress_uart_json_tx=true;
  if(g_sleep_transition)g_sleep_transition=false;
  // Keep panel/LVGL changes with their existing UI owner. Normal inactivity
  // may darken the screen while these flags keep the transport awake.
}
static void lcd_image_release(const char* reason,bool failed) {
  g_image_store.abort();g_image_rx=false;g_image_tx_pending=false;
  g_img_rx_active=false;g_img_rx_binary_mode=false;g_spool_tx_active=false;g_spool_tx_pending=false;
  g_suppress_uart_json_tx=false;
  g_image_waiting_abort=false;
  lcd_media_release();
  if(failed)g_image_sd_failed++;
  Serial.printf("[IMAGE_SD] result=%s job=%lu bytes=%lu failures=%lu\n",reason,
    (unsigned long)g_image_transfer_meta.job_id,(unsigned long)g_image_store.received(),(unsigned long)g_image_sd_failed);
}
static bool lcd_image_expired() {
  return (uint32_t)(millis()-g_image_started_ms)>=LCD_IMAGE_XFER_MS||
    (uint32_t)(millis()-g_image_last_frame_ms)>=LCD_IMAGE_IDLE_MS;
}
static void lcd_image_quarantine_tick() {
  if(g_image_waiting_abort && g_spool_tx_active &&
     (uint32_t)(millis()-g_image_started_ms)>=LCD_IMAGE_XFER_MS) {
    // The FILE was already closed before waiting for ABORT. Do not hold the
    // LCD awake forever for lost proof: retire only the original transfer's
    // sleep custody. JSON suppression and media admission stay quarantined;
    // a bound late ABORT or normal sleep/reboot is required for recovery.
    g_spool_tx_active=false;
    Serial.println("[IMAGE_SD] cleanup_deadline custody=retired json_quarantine=1 retained=1");
  }
}
static bool lcd_image_abort_ack_last() {
  StaticJsonDocument<320> r;r["type"]="IMAGE_XFER_ABORT_ACK";
  lcd_image_echo(r,g_image_last_meta);r["ok"]=1;r["json_ready"]=true;return lcd_image_reply(r);
}
static bool lcd_image_abort(JsonObjectConst d) {
  uint32_t schema=0,job=0,len=0,crc=0;
  const char* request=d["request_id"]|"";
  const bool match=g_image_have_last&&lcd_image_u32(d["image_schema"],&schema)&&schema==halo_image::kSchema&&
    !strcmp(request,g_image_last_meta.request_id)&&lcd_image_u32(d["job_id"],&job)&&job==g_image_last_meta.job_id&&
    lcd_image_u32(d["len"],&len)&&len==g_image_last_meta.len&&lcd_image_u32(d["crc32"],&crc)&&crc==g_image_last_meta.crc32;
  if(!match)return false;
  // A foreground yield has already closed its FILE. Keep ordinary TX and UI
  // admission quarantined until this exact terminal proof is serialized.
  if(g_image_waiting_abort) {
    if(lcd_image_abort_ack_last())lcd_image_release(g_image_replay_result,g_image_replay_failed);
    return true;
  }
  // Never use a stale image control to clear OTA or another transfer's owner.
  // The send path still owns an open-stream pin: it closes that file before
  // releasing sleep/JSON ownership and acknowledging the abort.
  if(g_image_tx_pending){g_image_tx_abort_pending=true;return true;}
  if(g_image_rx)lcd_image_release("peer_abort",true);
  else if(!lcd_image_link_idle())return false;
  lcd_image_abort_ack_last();return true;
}

static bool lcd_image_uart(JsonDocument& doc) {
  const char* type=doc["type"]|"";JsonObjectConst d=doc.as<JsonObjectConst>();
  if(!strcmp(type,"IMAGE_XFER_ABORT")){lcd_image_abort(d);return true;}
  const bool begin=!strcmp(type,"IMAGE_XFER_BEGIN"),list=!strcmp(type,"IMAGE_SPOOL_LIST_REQ"),
    fetch=!strcmp(type,"IMAGE_SPOOL_FETCH"),del=!strcmp(type,"IMAGE_SPOOL_DELETE"),attempt=!strcmp(type,"IMAGE_SPOOL_ATTEMPT");
  if(!begin&&!list&&!fetch&&!del&&!attempt)return false;
  LcdMaintenanceStorageGuard admission_guard;
  StaticJsonDocument<2048> r;r["type"]=begin?"IMAGE_XFER_READY":list?"IMAGE_SPOOL_LIST":fetch?"IMAGE_SPOOL_FETCH_READY":attempt?"IMAGE_SPOOL_ATTEMPT_ACK":"IMAGE_SPOOL_DELETE_ACK";
  halo_image::Meta m;char owner[64]={},device[32]={},request[33]={};
  halo_image::Result result=halo_image::Result::Invalid;
  const bool parsed=begin?lcd_image_parse_meta(d,&m):lcd_image_binding(d,owner,device,request,!list);
  if(begin&&parsed)lcd_image_echo(r,m);else if(request[0])r["request_id"]=request;
  if(!parsed){r["ok"]=0;r["reason"]="invalid";lcd_image_reply(r);return true;}
  if(!lcd_image_link_idle() || !lcd_media_try_claim(fetch||list,lcd_media_deferred_intent_pending())) {
    r["ok"]=0;r["reason"]="busy";r["json_ready"]=lcd_image_link_idle();lcd_image_reply(r);return true;
  }
  // Prevent either sleep route from entering while mounting/validating storage.
  g_img_rx_active=true;g_suppress_uart_json_tx=true;
  g_image_sd_work_started_ms=millis();g_image_sd_work_budget_ms=12000;
  {
    SdCardLease lease;
    if(!lease||!lcd_image_sd_mount())result=halo_image::Result::Io;
    else {
      g_image_store.set_progress(lcd_freeze_wdt_feed);
      g_image_store.set_budget(lcd_image_sd_budget);
      if(begin)result=g_image_store.begin(m);
      else if(list){halo_image::Stats stats;char after[33]={};
        bool cursor_ok=!d.containsKey("after_request_id")||
          (lcd_image_copy(d["after_request_id"],after,sizeof(after))&&halo_image::request_valid(after));
        result=cursor_ok?g_image_store.list(owner,device,&m,&stats,after):halo_image::Result::Invalid;
        r["count"]=stats.count;r["invalid"]=stats.invalid;r["incomplete"]=stats.incomplete;
        g_image_sd_invalid=stats.invalid;g_image_sd_incomplete=stats.incomplete;
        r["request_id"]=(result==halo_image::Result::Ok)?m.request_id:"";
        if(result==halo_image::Result::Ok)r["epoch"]=m.epoch;}
      else if(fetch)result=g_image_store.lookup(request,owner,device,&m);
      else {uint32_t len=0,crc=0,epoch=0;
        if(lcd_image_u32(d["len"],&len)&&lcd_image_u32(d["crc32"],&crc)){
          if(attempt&&lcd_image_u32(d["epoch"],&epoch))result=g_image_store.mark_attempt(request,owner,device,len,crc,epoch,&m);
          else if(del)result=g_image_store.erase(request,owner,device,len,crc);
        }
      }
    }
  }
  bool ok=result==halo_image::Result::Ok||result==halo_image::Result::AlreadyStored||(list&&result==halo_image::Result::Empty);
  const bool foreground_cancel=fetch && lcd_media_replay_cancelled();
  if(foreground_cancel)ok=false;
  if((begin&&result==halo_image::Result::Ok)||(fetch&&ok)) {
    if(!lcd_image_proto_ready()){g_image_store.abort();ok=false;result=halo_image::Result::Io;}
    else lcd_image_hold(begin,m);
  }
  r["ok"]=ok?1:0;r["reason"]=foreground_cancel?"busy":halo_image::result_name(result);
  if(begin)r["stored"]=result==halo_image::Result::AlreadyStored?1:0;
  if(fetch&&ok){lcd_image_echo(r,m);lcd_image_put_meta(r.createNestedObject("meta"),m);}
  if(attempt&&ok){lcd_image_echo(r,m);r["epoch"]=m.epoch;}
  if(begin||fetch)r["json_ready"]=!g_image_rx&&!g_image_tx_pending;
  if(!lcd_image_reply(r)&&(g_image_rx||g_image_tx_pending))lcd_image_release("reply_overflow",true);
  if(!g_image_rx&&!g_image_tx_pending) {
    g_img_rx_active=false;g_suppress_uart_json_tx=false;lcd_media_release();
  }
  Serial.printf("[IMAGE_SD] control=%s result=%s job=%lu\n",type,halo_image::result_name(result),(unsigned long)m.job_id);
  return true;
}

static bool lcd_image_receive_loop() {
  if(!g_image_rx)return false;
  lcd_freeze_wdt_feed();
  if(lcd_image_expired()){lcd_image_release("receive_timeout",true);return false;}
  uint8_t type=0;uint16_t seq=0;static uint8_t bytes[MAX_FRAME_SIZE];size_t n=sizeof(bytes);static char json[512];
  const auto event=g_image_proto->recv_event(&type,&seq,bytes,&n,json,sizeof(json),100);
  if(event==UartOtaProtocol::TIMEOUT)return true;
  if(event==UartOtaProtocol::JSON){StaticJsonDocument<512>d;if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","IMAGE_XFER_ABORT"))lcd_image_abort(d.as<JsonObjectConst>());return g_image_rx;}
  if(lcd_image_expired()){lcd_image_release("receive_deadline",true);return false;}
  if(type==MSG_IMG_NACK){lcd_image_release("peer_nack",true);return false;}
  halo_image::Result result=halo_image::Result::Invalid;
  {
    SdCardLease lease;
    g_image_sd_work_started_ms=millis();g_image_sd_work_budget_ms=3000;
    if(!lease||!sd_card_is_mounted())result=halo_image::Result::Io;
    else if(type==MSG_IMG_CHUNK)result=g_image_store.append(seq,bytes,n);
    else if(type==MSG_IMG_END&&n==0)result=g_image_store.finish(seq);
  }
  const bool ok=result==halo_image::Result::Ok||result==halo_image::Result::DuplicateChunk;
  if(!ok){g_image_proto->send_frame(MSG_IMG_NACK,seq,nullptr,0);lcd_image_release(halo_image::result_name(result),true);return false;}
  if(lcd_image_expired()){g_image_proto->send_frame(MSG_IMG_NACK,seq,nullptr,0);lcd_image_release("receive_deadline",true);return false;}
  g_image_last_frame_ms=millis();
  if(type==MSG_IMG_END){uint8_t proof[40];uint8_t* p=proof;halo_image::put32(p,g_image_transfer_meta.len);halo_image::put32(p,g_image_transfer_meta.crc32);memcpy(p,g_image_transfer_meta.request_id,32);
    g_image_sd_committed++;g_image_proto->send_frame(MSG_IMG_ACK,seq,proof,sizeof(proof));lcd_image_release("committed",false);return false;}
  g_image_proto->send_frame(MSG_IMG_ACK,seq,nullptr,0);return true;
}

// Every replay outcome joins the peer's existing bound ABORT after closing
// the FILE. Even a final END ACK proves payload receipt, not that the Sense has
// left its raw RX lease. No SD slot is erased. Missing proof keeps quarantine.
static void lcd_image_finish_replay(uint16_t seq,const char* result,bool failed,bool nack,bool peer_abort=false) {
  g_image_replay_result=result;g_image_replay_failed=failed;
  g_image_waiting_abort=true;g_image_tx_pending=false;g_spool_tx_pending=false;
  if(peer_abort) {
    if(lcd_image_abort_ack_last())lcd_image_release(result,failed);
    return;
  }
  if(nack)g_image_proto->send_frame(MSG_IMG_NACK,seq,nullptr,0);
  const uint32_t started=millis();
  static uint8_t bytes[MAX_FRAME_SIZE];static char json[512];
  while(g_image_waiting_abort && (uint32_t)(millis()-started)<2500 &&
        (uint32_t)(millis()-g_image_started_ms)<LCD_IMAGE_XFER_MS) {
    uint8_t type=0;uint16_t got_seq=0;size_t n=sizeof(bytes);
    const auto event=g_image_proto->recv_event(&type,&got_seq,bytes,&n,json,sizeof(json),100);
    lcd_freeze_wdt_feed();
    if(event==UartOtaProtocol::JSON) {
      StaticJsonDocument<512>d;
      if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","IMAGE_XFER_ABORT"))
        lcd_image_abort(d.as<JsonObjectConst>());
    }
  }
  Serial.printf("[IMAGE_SD] cleanup=%s json_ready=%u retained=1\n",result,g_image_waiting_abort?0:1);
}

static void lcd_image_send_file() {
  if(!g_image_tx_pending)return;
  // Keep pending true while streaming: it identifies the image owner for ABORT.
  vTaskDelay(pdMS_TO_TICKS(120));
  const uint32_t drain_started=millis();
  const unsigned drain_limit=(unsigned)senseSerial.available();
  for(unsigned i=0;i<drain_limit&&i<1024&&(uint32_t)(millis()-drain_started)<20;++i){
    if(!senseSerial.available())break;senseSerial.read();
  }
  char path[halo_image::kPathBytes];g_image_store.payload_path(g_image_transfer_meta.request_id,path,sizeof(path));
  FILE* f=nullptr;bool ok=false;bool foreground_yield=false;
  {SdCardLease lease;if(lease&&sd_card_is_mounted())ok=sd_open_file_for_read(path,&f)==ESP_OK&&f;}
  uint16_t seq=0;uint32_t sent=0;static uint8_t bytes[MAX_CHUNK_SIZE],reply[MAX_FRAME_SIZE];static char json[512];
  while(ok&&sent<g_image_transfer_meta.len){
    if(lcd_media_replay_cancelled()){foreground_yield=true;ok=false;break;}
    lcd_freeze_wdt_feed();if(lcd_image_expired()){ok=false;break;}
    size_t n=0;if(sd_read_chunk(f,bytes,sizeof(bytes),&n)!=ESP_OK||n==0||n>g_image_transfer_meta.len-sent){ok=false;break;}
    if(!g_image_proto->send_frame(MSG_IMG_CHUNK,seq,bytes,n)){ok=false;break;}
    bool ack=false;
    while(!lcd_image_expired()){
      if(lcd_media_replay_cancelled()){foreground_yield=true;break;}
      uint8_t type=0;uint16_t rseq=0;size_t rn=sizeof(reply);
      auto event=g_image_proto->recv_event(&type,&rseq,reply,&rn,json,sizeof(json),100);
      lcd_freeze_wdt_feed();
      if(event==UartOtaProtocol::TIMEOUT)continue;
      if(event==UartOtaProtocol::JSON){StaticJsonDocument<512>d;if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","IMAGE_XFER_ABORT"))lcd_image_abort(d.as<JsonObjectConst>());break;}
      ack=type==MSG_IMG_ACK&&rseq==seq&&rn==0;break;
    }
    if(!ack||!g_image_tx_pending){ok=false;break;}
    sent+=(uint32_t)n;seq++;g_image_last_frame_ms=millis();
  }
  if(f&&sd_close_file(f)!=ESP_OK)ok=false;
  if(g_image_tx_abort_pending){
    g_image_tx_abort_pending=false;lcd_image_finish_replay(seq,"peer_abort",true,false,true);return;
  }
  if(foreground_yield||lcd_media_replay_cancelled()){lcd_image_finish_replay(seq,"foreground_yield",false,true);return;}
  if(ok){uint8_t proof[40];uint8_t* p=proof;halo_image::put32(p,g_image_transfer_meta.len);halo_image::put32(p,g_image_transfer_meta.crc32);memcpy(p,g_image_transfer_meta.request_id,32);
    ok=g_image_proto->send_frame(MSG_IMG_END,seq,proof,sizeof(proof));
    bool ack=false;
    while(ok&&!lcd_image_expired()){
      uint8_t type=0;uint16_t rseq=0;size_t rn=sizeof(reply);
      auto event=g_image_proto->recv_event(&type,&rseq,reply,&rn,json,sizeof(json),100);lcd_freeze_wdt_feed();
      if(event==UartOtaProtocol::TIMEOUT)continue;
      if(event==UartOtaProtocol::FRAME)ack=type==MSG_IMG_ACK&&rseq==seq&&rn==0;
      else{StaticJsonDocument<512>d;if(!deserializeJson(d,json)&&!strcmp(d["type"]|"","IMAGE_XFER_ABORT"))lcd_image_abort(d.as<JsonObjectConst>());}
      break;
    }ok=ok&&ack;
  }
  if(g_image_tx_abort_pending){
    g_image_tx_abort_pending=false;lcd_image_finish_replay(seq,"peer_abort",true,false,true);return;
  }
  if(g_image_tx_pending)lcd_image_finish_replay(seq,ok?"replay_sent":"replay_failed",!ok,!ok);
}

static void lcd_image_queue_diagnostic() {
  LcdMaintenanceStorageGuard admission_guard;
  if(!lcd_image_link_idle()||!lcd_media_try_claim(false,lcd_media_deferred_intent_pending())){
    Serial.printf("[IMAGE_QUEUE] schema=1 enabled=%d mounted=unknown result=busy\n",LCD_IMAGE_SPOOL_ENABLED);return;
  }
  g_img_rx_active=true;g_suppress_uart_json_tx=true;
  g_image_sd_work_started_ms=millis();g_image_sd_work_budget_ms=12000;
  halo_image::Stats stats;halo_image::Result result=halo_image::Result::Io;bool mounted=false;
  {
    SdCardLease lease;
    mounted=lease&&lcd_image_sd_mount();
    if(mounted){g_image_store.set_progress(lcd_freeze_wdt_feed);g_image_store.set_budget(lcd_image_sd_budget);result=g_image_store.inventory(&stats);}
  }
  g_img_rx_active=false;g_suppress_uart_json_tx=false;
  lcd_media_release();
  Serial.printf("[IMAGE_QUEUE] schema=1 enabled=%d mounted=%u result=%s pending=%lu incomplete=%lu corrupt=%lu elapsed_ms=%lu committed_this_boot=%lu failed_this_boot=%lu\n",
    LCD_IMAGE_SPOOL_ENABLED,mounted?1:0,halo_image::result_name(result),(unsigned long)stats.count,
    (unsigned long)stats.incomplete,(unsigned long)stats.invalid,(unsigned long)(millis()-g_image_sd_work_started_ms),
    (unsigned long)g_image_sd_committed,(unsigned long)g_image_sd_failed);
}
