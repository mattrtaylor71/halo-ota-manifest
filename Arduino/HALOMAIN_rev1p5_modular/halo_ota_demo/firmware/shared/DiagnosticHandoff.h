#pragma once
#include "DiagnosticEnvelope.h"
namespace halo_diag {
// A current-time, receipt-acknowledged map makes even epoch0 source events
// discoverable after retirement. fw/build remain ACTUAL captured source values;
// diag_export/diag_handoff explicitly distinguish this from a current VALID
// attestation. Collector follows each d3-UUID-sequence-CRC through request_id.
inline bool prepare_handoff_envelope(const uint8_t*ctx,const uint8_t*source,const Handoff&h,EnvelopeIdentity&id){
  if(h.epoch<1577836800u||h.source_slot<1||h.source_slot>3||!prepare_envelope(ctx,source,h.source_slot,id)||
     memcmp(h.journal,id.ack.journal,16)||h.context_crc!=id.context_crc||h.sequence[h.source_slot-1]!=id.ack.sequence||h.crc[h.source_slot-1]!=id.ack.crc)return false;
  id.epoch=h.epoch;memset(id.request,0,sizeof(id.request));memcpy(id.request,"h4-",3);hex_into(id.request+3,h.journal,16);id.request[35]='-';
  uint8_t number[4];const uint32_t fingerprint=handoff_fingerprint(h);for(unsigned i=0;i<4;++i)number[i]=uint8_t(fingerprint>>(24-8*i));hex_into(id.request+36,number,4);return true;
}
inline bool write_handoff_envelope(const uint8_t*ctx,const uint8_t*source,const Handoff&h,const EnvelopeIdentity&id,void*arg,bool(*sink)(void*,const char*,size_t),size_t&length){
  length=0;EnvelopeIdentity actual;if(!prepare_handoff_envelope(ctx,source,h,actual)||actual.epoch!=id.epoch||actual.context_crc!=id.context_crc||actual.slot!=id.slot||actual.board!=id.board||
    memcmp(actual.ack.journal,id.ack.journal,16)||actual.ack.sequence!=id.ack.sequence||actual.ack.crc!=id.ack.crc||strcmp(actual.device,id.device)||strcmp(actual.build,id.build)||strcmp(actual.fw,id.fw)||strcmp(actual.request,id.request))return false;
  uint8_t map[HANDOFF_BYTES];encode_handoff(map,h);EnvelopeWriter w(arg,sink);
  w.text("{\"device_id\":");w.string(id.device);w.text(",\"device_type\":");w.string(id.board==1?"sense":"lcd");w.text(",\"fw\":");w.string(id.fw);w.text(",\"build\":");w.string(id.build);
  w.text(",\"report_type\":\"heartbeat\",\"diag_export\":1,\"diag_handoff\":1,\"diag_schema\":3,\"diag_handoff_schema\":4,\"request_id\":");w.string(id.request);w.text(",\"ts_epoch\":");w.number(id.epoch);
  w.text(",\"diag_slot\":");w.number(id.slot);w.text(",\"diag_sequence\":");w.number(id.ack.sequence);w.text(",\"diag_crc\":");w.number(id.ack.crc);
  w.text(",\"diag_drops_lower_bound\":true,\"diag_handoff_b64\":");w.base64(map,sizeof(map));w.text(",\"diag_context_b64\":");w.base64(ctx,256);w.text(",\"diag_record_b64\":");w.base64(source,256);w.ch('}');return w.finish(length);
}
inline Result acknowledge_handoff_envelope(Journal&journal,const Handoff&h,const EnvelopeIdentity&id,const uint8_t sha1[20],int status,const char*reply,size_t n){
  char expected[MAX_EVENT_KEY];if(id.epoch!=h.epoch||memcmp(id.ack.journal,h.journal,16)||id.context_crc!=h.context_crc||!event_key(expected,sizeof(expected),id.epoch,sha1)||!receipt_accepted(status,reply,n,expected))return Result::Invalid;
  return journal.acknowledge_handoff(h);
}
} // namespace halo_diag
