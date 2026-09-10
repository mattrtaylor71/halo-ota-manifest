#pragma once
#include "DiagnosticAdmissionStore.h"
#include "DiagnosticEnvelope.h"

namespace halo_admission {
using Hash256=bool(*)(const uint8_t*,size_t,uint8_t*);
using Hash1=bool(*)(const uint8_t*,size_t,uint8_t*);
struct EnvelopeIdentity {char device[32]{},request[68]{};uint32_t epoch=0;};
inline bool prepare_envelope(const uint8_t(&context)[256],const uint8_t(&record)[64],
                             Hash256 hash,EnvelopeIdentity&out){
  Record r;if(!hash||!decode(record,64,r)||!halo_diag::valid_context(context)||context[halo_diag::HEADER+166]!=1)return false;
  uint8_t digest[32];if(!hash(context,256,digest)||memcmp(digest,r.context_sha,32)||!hash(record,64,digest))return false;
  EnvelopeIdentity id;memcpy(id.device,"halo-",5);const uint8_t*c=context+halo_diag::HEADER;
  halo_diag::hex_into(id.device+5,c+162,2);id.device[9]='-';halo_diag::hex_into(id.device+10,c+164,2);
  memcpy(id.request,"b1-",3);halo_diag::hex_into(id.request+3,digest,32);id.epoch=r.epoch;out=id;return true;
}
inline bool write_envelope(const uint8_t(&context)[256],const uint8_t(&record)[64],Hash256 hash,
    const EnvelopeIdentity&id,void*arg,bool(*sink)(void*,const char*,size_t),size_t&length){
  length=0;EnvelopeIdentity actual;if(!prepare_envelope(context,record,hash,actual)||
    !halo_diag::key_component(id.device,sizeof(id.device))||!halo_diag::key_component(id.request,sizeof(id.request))||
    strcmp(id.device,actual.device)||strcmp(id.request,actual.request)||id.epoch!=actual.epoch)return false;
  halo_diag::EnvelopeWriter w(arg,sink);w.text("{\"device_id\":");w.string(id.device);
  // Required legacy envelope fields explicitly mean unrecorded here. They are
  // neither the later exporting firmware nor an invented historical build.
  w.text(",\"device_type\":\"sense\",\"fw\":\"unrecorded\",\"build\":\"unrecorded\",\"report_type\":\"heartbeat\",\"diag_export\":1,\"diag_schema\":5,\"diag_admission_schema\":1,\"diag_capture_identity\":\"unrecorded\",\"request_id\":");
  w.string(id.request);w.text(",\"ts_epoch\":");w.number(id.epoch);
  w.text(",\"diag_context_b64\":");w.base64(context,256);w.text(",\"diag_admission_b64\":");w.base64(record,64);w.ch('}');return w.finish(length);
}
inline bool key_material(char*out,size_t capacity,const EnvelopeIdentity&id){
  if(!halo_diag::key_component(id.device,sizeof(id.device))||!halo_diag::key_component(id.request,sizeof(id.request)))return false;
  halo_diag::KeyWriter w(out,capacity);w.text(id.device);w.text("|heartbeat|");w.decimal(id.epoch);w.text("|unrecorded|");w.text(id.request);return w.finish(halo_diag::MAX_KEY_MATERIAL);
}
struct ReceiptView {const uint8_t*source;int status;const char*body;size_t length;char expected[halo_diag::MAX_EVENT_KEY];};
inline bool receipt(void*v,const uint8_t*source,size_t n){const auto&r=*(ReceiptView*)v;return n==64&&!memcmp(source,r.source,64)&&halo_diag::receipt_accepted(r.status,r.body,r.length,r.expected);}
inline Result acknowledge_envelope(Store&store,const uint8_t(&context)[256],const uint8_t(&record)[64],
    Hash256 sha256,Hash1 sha1,int status,const char*body,size_t n){
  EnvelopeIdentity id;char material[halo_diag::MAX_KEY_MATERIAL];uint8_t digest[20];ReceiptView view{record,status,body,n,{}};
  if(!sha1||!prepare_envelope(context,record,sha256,id)||!key_material(material,sizeof(material),id)||
     !sha1((const uint8_t*)material,strlen(material),digest)||!halo_diag::event_key(view.expected,sizeof(view.expected),id.epoch,digest))return Result::Invalid;
  return store.acknowledge(record,&view,receipt);
}
} // namespace halo_admission
