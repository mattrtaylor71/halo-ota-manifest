#pragma once
#include "../halo_ota_demo/firmware/shared/DiagnosticHandoff.h"

// Typed operations only: the existing UART owner validates/decodes messages
// then calls here. No reader/task, wake/hold policy, GPIO, LVGL or SD operation.
// Store and scratch must outlive this object, or construct it per operation.
class LcdDiagnostics {
  halo_diag::Journal journal_;uint8_t mac_[6];bool accepted_=false;
 public:
  LcdDiagnostics(halo_diag::Store&s,uint8_t (&scratch)[halo_diag::MAX_RECORD],const uint8_t mac[6]):journal_(s,scratch){memcpy(mac_,mac,6);}
  halo_diag::Result open(){return journal_.open();}
  halo_diag::Result close_orphan(const halo_diag::State&s){accepted_=false;return journal_.close_orphan(s);}
  bool capture_enabled()const{return accepted_&&journal_.capture_enabled();}
  void close_capture(){accepted_=false;}
  bool orphan_closing()const{return journal_.orphan_closing();}
  bool sealed()const{return journal_.sealed();}
  halo_diag::Result seal(){accepted_=false;return journal_.seal();}
  halo_diag::Result accept_context(const halo_diag::Context&ctx){
    using namespace halo_diag;accepted_=false;if(ctx.board!=Board::Lcd||memcmp(ctx.device_mac,mac_,6))return Result::Invalid;uint8_t raw[MAX_RECORD];if(!encode_context(raw,ctx))return Result::Invalid;
    Result r=journal_.open();if(r==Result::Empty||r==Result::Incomplete){r=journal_.initialize(ctx);accepted_=r==Result::Ok;return r;}if(r!=Result::Ok)return r;
    size_t n=0;r=journal_.read_retained(0,raw,sizeof(raw),n);if(r!=Result::Ok)return r;
    // Journal UUID/created time are journal-local, chosen freshly for each
    // offered context. Replayed matching campaign accepts its existing UUID.
    const uint8_t*p=raw+HEADER;const bool same=!memcmp(p+16,ctx.campaign,16)&&!memcmp(p+32,ctx.target_sha,32)&&!strcmp((const char*)p+64,ctx.target_version)&&!strcmp((const char*)p+96,ctx.origin)&&!memcmp(p+160,ctx.device_mac,6)&&p[166]==uint8_t(ctx.board)&&p[167]==uint8_t(ctx.target_hash_kind)&&get32(p+168)==ctx.expected_bytes&&!memcmp(p+180,ctx.owner_binding_sha,32);
    if(!same){journal_.seal();return Result::Busy;}
    accepted_=journal_.capture_enabled();return accepted_?Result::Already:Result::Busy;
  }
  halo_diag::Result prepare_handoff(uint32_t epoch){accepted_=false;return journal_.prepare_handoff(epoch);}
  halo_diag::Result read_handoff(halo_diag::Handoff&h){return journal_.read_handoff(h);}
  halo_diag::Result retire_if_ready(){accepted_=false;return journal_.retire_if_ready();}
  halo_diag::Result cloud_handoff_ack(int status,const char*receipt,size_t bytes,bool(*hash)(void*,const uint8_t*,size_t,uint8_t[20]),void*arg){
    using namespace halo_diag;if(!hash)return Result::Invalid;Handoff h;Result r=journal_.read_handoff(h);if(r!=Result::Ok)return r;
    uint8_t ctx[256],source[256];size_t n=0;r=journal_.read_retained(0,ctx,sizeof(ctx),n);if(r!=Result::Ok)return r;r=journal_.read_retained(h.source_slot,source,sizeof(source),n);if(r!=Result::Ok)return r;
    EnvelopeIdentity id;if(!prepare_handoff_envelope(ctx,source,h,id))return Result::Invalid;char material[MAX_KEY_MATERIAL];uint8_t digest[20];if(!envelope_key_material(material,sizeof(material),id)||!hash(arg,(const uint8_t*)material,strlen(material),digest))return Result::Invalid;
    return acknowledge_handoff_envelope(journal_,h,id,digest,status,receipt,bytes);
  }
  halo_diag::Result failure(const halo_diag::Failure&f){return capture_enabled()?journal_.failure(f):halo_diag::Result::Busy;}
  // Only the already captured, same-context volatile failure may drain after
  // a context conflict. Runtime forbids seal/retire while it remains pending.
  halo_diag::Result flush_retained_failure(const halo_diag::Failure&f){return journal_.failure(f);}
  halo_diag::Result state(const halo_diag::State&s){return capture_enabled()?journal_.state(s):halo_diag::Result::Busy;}
  halo_diag::Result watermark(halo_diag::Watermark&w){return journal_.watermark(w);}
  halo_diag::Result read_record(uint8_t slot,uint8_t*out,size_t cap,size_t&n){return journal_.read_retained(slot,out,cap,n);}
  halo_diag::Result next_record(uint8_t*out,size_t cap,size_t&n,uint8_t&slot){return journal_.next_export(out,cap,n,slot);}
  // hash computes SHA1 over exact key material using the existing implementation.
  // The caller supplies the actual bounded server receipt, never an invented
  // UART "received" message. A mismatched/old record stays pending on LCD.
  halo_diag::Result cloud_ack(uint8_t slot,int http_status,const char*receipt,size_t bytes,
      bool(*hash)(void*,const uint8_t*,size_t,uint8_t[20]),void*arg){
    using namespace halo_diag;if(!hash||slot<1||slot>3)return Result::Invalid;
    uint8_t ctx[MAX_RECORD],record[MAX_RECORD];size_t n=0;Result r=journal_.read_retained(0,ctx,sizeof(ctx),n);if(r!=Result::Ok)return r;
    r=journal_.read_retained(slot,record,sizeof(record),n);if(r!=Result::Ok)return r;
    EnvelopeIdentity id;if(!prepare_envelope(ctx,record,slot,id))return Result::Invalid;
    char material[MAX_KEY_MATERIAL];uint8_t digest[20];if(!envelope_key_material(material,sizeof(material),id)||!hash(arg,(const uint8_t*)material,strlen(material),digest))return Result::Invalid;
    return acknowledge_envelope(journal_,id,digest,http_status,receipt,bytes);
  }
};
