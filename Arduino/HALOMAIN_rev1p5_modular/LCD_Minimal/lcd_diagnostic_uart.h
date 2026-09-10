#pragma once
#if defined(HALO_DURABLE_DIAGNOSTICS) && HALO_DURABLE_DIAGNOSTICS
// One bounded ordinary JSON request/reply on the existing UART. No UART receipt
// retires a journal record; only the full checked cloud receipt can do so.
static bool lcd_diag_unhex(const char*s,uint8_t*out,size_t bytes){
  if(!s||strnlen(s,bytes*2+1)!=bytes*2)return false;
  auto digit=[](char c)->int{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
  for(size_t i=0;i<bytes;++i){const int a=digit(s[i*2]),b=digit(s[i*2+1]);if(a<0||b<0)return false;out[i]=uint8_t(a*16+b);}return true;
}
static bool lcd_diag_decode_context(const uint8_t*raw,halo_diag::Context&c){
  if(!halo_diag::valid_context(raw))return false;const uint8_t*p=raw+halo_diag::HEADER;c={};
  memcpy(c.journal,p,16);memcpy(c.campaign,p+16,16);memcpy(c.target_sha,p+32,32);memcpy(c.target_version,p+64,32);memcpy(c.origin,p+96,64);memcpy(c.device_mac,p+160,6);
  c.board=(halo_diag::Board)p[166];c.target_hash_kind=(halo_diag::HashKind)p[167];c.expected_bytes=halo_diag::get32(p+168);c.created_epoch=halo_diag::get64(p+172);memcpy(c.owner_binding_sha,p+180,32);return true;
}
static bool lcd_diag_uart(JsonDocument&doc){
  if(strcmp(doc["type"]|"","OTA_DIAG"))return false;
  const char*nonce=doc["nonce"]|"",*op=doc["op"]|"";uint8_t challenge[8];
  if(!lcd_diag_unhex(nonce,challenge,sizeof(challenge))||strnlen(op,12)>=12||!g_lcd_coord_boot_id)return true;
  const bool info=!strcmp(op,"info");
  if(!info&&(!doc["peer_boot"].is<uint32_t>()||doc["peer_boot"].as<uint32_t>()!=g_lcd_coord_boot_id))return true;
  halo_diag::Result result=halo_diag::Result::Invalid;uint8_t raw[256],slot=0;size_t bytes=0,offset=0,total=0;
  if(info){LcdDiagnosticScope scope;if(scope.active&&lcd_diag_safe(nullptr)&&lcd_nvs_image_valid()&&esp_read_mac(raw,ESP_MAC_ETH)==ESP_OK){bytes=6;result=halo_diag::Result::Ok;}}
#if HALO_LCD_SLEEP_WITNESS
  else if(!strcmp(op,"sleep")){
    result=lcd_sleep_witness_read(raw,sizeof(raw),bytes)?halo_diag::Result::Ok:halo_diag::Result::Empty;
  }
#endif
  else if(!strcmp(op,"context")){
    halo_diag::Context ctx{};if(lcd_diag_unhex(doc["data"]|"",raw,256)&&lcd_diag_decode_context(raw,ctx))result=lcd_diag_context(ctx);
  }else if(!strcmp(op,"discover"))result=lcd_diag_discovery(raw,bytes);
  else if(!strcmp(op,"read")){
    if(doc["slot"].is<unsigned>()&&doc["slot"].as<unsigned>()<4&&doc["offset"].is<unsigned>()&&(doc["offset"].as<unsigned>()==0||doc["offset"].as<unsigned>()==128)){slot=doc["slot"].as<unsigned>();offset=doc["offset"].as<unsigned>();result=lcd_diag_read(slot,raw,sizeof(raw),bytes);}
  }else if(!strcmp(op,"next"))result=lcd_diag_next(raw,sizeof(raw),bytes,slot);
  else if(!strcmp(op,"ack")){
    const unsigned n=doc["bytes"]|0u;
    if(n&&n<=256&&doc["slot"].is<unsigned>()&&doc["slot"].as<unsigned>()>=1&&doc["slot"].as<unsigned>()<=3&&doc["status"].is<int>()&&lcd_diag_unhex(doc["data"]|"",raw,n)){
      slot=doc["slot"].as<unsigned>();result=lcd_diag_ack(slot,doc["status"].as<int>(),(const char*)raw,n);
    }
  }
  if(!strcmp(op,"seal")){
    if(doc["bytes"].is<unsigned>()&&doc["bytes"].as<unsigned>()==20&&lcd_diag_unhex(doc["data"]|"",raw,20))
      result=lcd_diag_seal(raw,halo_diag::get32(raw+16));
  }else if(!strcmp(op,"handoff")){
    if(doc["bytes"].is<unsigned>()&&doc["bytes"].as<unsigned>()==4&&lcd_diag_unhex(doc["data"]|"",raw,4)){
      const uint32_t epoch=halo_diag::get32(raw);result=lcd_diag_handoff(epoch,raw,sizeof(raw),bytes);
    }
  }else if(!strcmp(op,"h_ack")){
    const unsigned n=doc["bytes"]|0u;
    if(n&&n<=256&&doc["status"].is<int>()&&lcd_diag_unhex(doc["data"]|"",raw,n))
      result=lcd_diag_handoff_ack(doc["status"].as<int>(),(const char*)raw,n);
  }else if(!strcmp(op,"retire")){
    if(doc["bytes"].is<unsigned>()&&doc["bytes"].as<unsigned>()==0&&doc["data"].is<const char*>()&&!doc["data"].as<const char*>()[0])
      result=lcd_diag_retire();
  }
  // Sense's existing RX frame is512 bytes. Never enlarge it for diagnostics.
  // Each record read carries128 bytes; final whole-record CRC rejects a torn
  // coalesced state if the two requests observe different generations.
  total=bytes;if(bytes>128){if(offset>=bytes){bytes=0;result=halo_diag::Result::Invalid;}else{memmove(raw,raw+offset,bytes-offset);bytes-=offset;if(bytes>128)bytes=128;}}
  char hex[257];halo_diag::hex_into(hex,raw,bytes);
  JsonDocument out;out["ver"]=PROTOCOL_VERSION;out["type"]="OTA_DIAG_REPLY";out["msg_id"]=get_next_msg_id();out["ts"]=millis();
  out["nonce"]=nonce;out["op"]=op;out["peer_boot"]=g_lcd_coord_boot_id;out["fw"]=kFirmwareVersion;out["result"]=unsigned(result);out["slot"]=slot;out["offset"]=offset;out["total"]=total;out["data"]=hex;
#if HALO_LCD_SLEEP_WITNESS
  if(!strcmp(op,"sleep") && result==halo_diag::Result::Ok) {
    out["sw_reset"]=halo_lcd_sleep_witness::kDeepSleepReset;
    out["sw_wake"]=halo_lcd_sleep_witness::kTimerWake;
    out["sw_quality"]=unsigned(halo_lcd_sleep_witness::Quality::TimerPredecessor);
  }
#endif
  if(info)out["qualification"]=unsigned(g_lcd_diag_adapter.last_qualification());
  char wire[512];const size_t needed=measureJson(out);if(out.overflowed()||needed>=sizeof(wire)||serializeJson(out,wire,sizeof(wire))!=needed)return true;
  uart_send_json(wire);return true;
}
#endif
