#pragma once
#if HALO_DIAGNOSTIC_ADMISSION && HALO_DURABLE_DIAGNOSTICS && HALO_DURABLE_OTA_POLICY
#include "DiagnosticAdmissionAuth.h"
#ifndef HALO_DIAG_B1_URL
#define HALO_DIAG_B1_URL ""
#endif
// A separately reviewed local provider may supply a scoped private-bench key.
// Missing provider/URL disables only this optional later export. No live secret
// or user/legacy credential storage is introduced by this candidate.
extern "C" bool halo_diag_b1_load_credentials(char*,size_t,uint8_t*,size_t) __attribute__((weak));
namespace sense_admission_export {
static bool attempted=false;
struct Local {bool held=false;};
static bool safe(){return !sense_admission::uncertain&&sense_diag_safe(nullptr)&&nvs_capacity_image_valid();}
static halo_admission::NvsGuard guard(Local&local){return {&local,
 [](void*v){auto&l=*static_cast<Local*>(v);if(l.held||!safe()||g_optional_nvs_writer.test_and_set(std::memory_order_acquire))return false;return l.held=true;},
 [](void*v){auto&l=*static_cast<Local*>(v);if(l.held){l.held=false;g_optional_nvs_writer.clear(std::memory_order_release);}},
 [](void*){return safe();},[](void*,const halo_admission::Record&){return false;},
 [](void*){return SIZE_MAX;},[](void*){return false;},[](void*){sense_admission::uncertain=true;}};}
static bool read(uint8_t(&raw)[64]){Local local;halo_admission::NvsAdapter adapter(guard(local),true);halo_admission::Store store(adapter.port());return store.read(raw)==halo_admission::Result::Already;}
static bool context_matches(const uint8_t(&context)[256],const uint8_t(&raw)[64]){halo_admission::EnvelopeIdentity id;return halo_admission::prepare_envelope(context,raw,sense_admission::hash,id);}
static bool retention_pending(){
  // Only exact known evidence protects the optional Context. Corrupt/unknown
  // old-reader residue remains unproved; no OTA or rollback path consults this.
  uint8_t raw[64],context[256];size_t n=0;
  return safe()&&read(raw)&&g_diag_journal.read_retained(0,context,256,n)==halo_diag::Result::Ok&&
    n==256&&context_matches(context,raw);
}
static bool url_ok(){const char*u=HALO_DIAG_B1_URL;const char*path="/v1/private/ota/admission";const size_t n=strlen(u),p=strlen(path);return n>8+p&&n<256&&!strncmp(u,"https://",8)&&!strcmp(u+n-p,path)&&!strchr(u,'?')&&!strchr(u,'#');}
struct Key {char id[33]{};uint8_t bytes[32]{};~Key(){halo_admission::wipe(bytes,sizeof(bytes));}};
static halo_diag::PostResult post(const uint8_t(&context)[256],const uint8_t(&raw)[64],
    const halo_admission::EnvelopeIdentity&id,halo_diag::PostLease lease,halo_diag::PostReceipt&receipt){
  using namespace halo_diag;PostMeasure measure(receipt);
  if(!url_ok()||!halo_diag_b1_load_credentials)return PostResult::Disabled;
  if(!post_remaining(lease)||!lease.io_timeout_ms||!wifi_is_connected())return PostResult::Busy;
  // Private response receipts require verified TLS. Never inherit the existing
  // ordinary-report insecure fallback when no root/bundle is available.
#if !HAS_CRT_BUNDLE
  if((!OTA_REPORT_HTTP_ROOT_CA||!OTA_REPORT_HTTP_ROOT_CA[0])&&(!rootCA||!rootCA[0]))return PostResult::Disabled;
#endif
  Key key;if(!halo_diag_b1_load_credentials(key.id,sizeof(key.id),key.bytes,sizeof(key.bytes))||!halo_admission::key_id_ok(key.id))return PostResult::Disabled;
  if(lease.io_timeout_ms>OTA_REPORT_HTTP_TIMEOUT_MS)lease.io_timeout_ms=OTA_REPORT_HTTP_TIMEOUT_MS;
  const uint32_t guard_deadline=lease.deadline_ms;
  const uint32_t now=millis();const int32_t left=int32_t(lease.deadline_ms-now);if(left<=0)return PostResult::Deadline;
  if(uint32_t(left)>lease.io_timeout_ms)lease.deadline_ms=now+lease.io_timeout_ms;
  uint8_t body[halo_admission::PRIVATE_ENVELOPE_MAX];size_t length=0;char signature[65];
  if(!halo_admission::authenticated_envelope(context,raw,sense_admission::hash,id,key.bytes,body,sizeof(body),length,signature))return PostResult::Invalid;
  halo_admission::wipe(key.bytes,sizeof(key.bytes));
  if(!post_remaining(lease))return PostResult::Deadline;
  sense_idle_network::Guard recovery(halo_idle_net::Op::Admission,guard_deadline);
  if(!recovery.active())return PostResult::RecoverySkipped;
  HaloNtpDnsGuard dns_guard;ScopedTlsDmaReserve reserve_guard(true);
  const sense_post::Request request_post{HALO_DIAG_B1_URL,id.device,nullptr,id.request,body,length,true,key.id,signature};
  const sense_post::Lease post_lease{lease.arg,lease.live,guard_deadline,lease.io_timeout_ms,now};
  sense_post::Response&response=receipt;
  const sense_post::Result result=sense_post::post_once(request_post,post_lease,response);
  // Preserve the prior transport's empty-body contract on every failure while
  // sharing the exact response buffer on success.
  auto failure=[&](PostResult result){memset(receipt.body,0,sizeof(receipt.body));receipt.length=0;return result;};
  receipt.status=response.status;receipt.http_observed=response.http_observed;
  if(result!=sense_post::Result::Received){
    if(result==sense_post::Result::Deadline||result==sense_post::Result::Cleanup)return failure(PostResult::Deadline);
    if(result==sense_post::Result::Busy)return failure(PostResult::Busy);
    if(result==sense_post::Result::Invalid)return failure(PostResult::Invalid);
    if(result==sense_post::Result::UnsupportedTls)return failure(PostResult::Disabled);
    if(result==sense_post::Result::Response)return failure(PostResult::Truncated);
    return failure(PostResult::BeginFailed);
  }
  if(!lease.live(lease.arg)||!sense_post::remaining(guard_deadline))return failure(PostResult::Deadline);
  if(receipt.status!=200)return failure(PostResult::HttpRejected);
  if(!response.length||response.length>MAX_RECEIPT_BYTES)return failure(PostResult::ReceiptSize);
  // Response storage is already the exact caller-owned receipt; no second copy.
  return PostResult::Received;
}
static bool ack(const uint8_t(&context)[256],const uint8_t(&raw)[64],const halo_diag::PostReceipt&receipt,uint32_t deadline){
  const uint32_t left=sense_diag_remaining(deadline);if(!left)return false;
  SenseDiagnosticScope scope(left);if(!scope.active||!safe())return false;
  uint8_t current[256];size_t n=0;if(g_diag_journal.read_retained(0,current,256,n)!=halo_diag::Result::Ok||n!=256||memcmp(context,current,256))return false;
  Local local;halo_admission::NvsAdapter adapter(guard(local),true);halo_admission::Store store(adapter.port());
  return halo_admission::acknowledge_envelope(store,context,raw,sense_admission::hash,
    [](const uint8_t*p,size_t z,uint8_t*out){return mbedtls_sha1(p,z,out)==0;},receipt.status,receipt.body,receipt.length)==halo_admission::Result::Acked;
}
static __attribute__((noinline)) void export_once(uint32_t deadline){
  if(attempted||!url_ok()||!halo_diag_b1_load_credentials||!sense_diag_export_idle(nullptr)||
     sense_diag_remaining(deadline)<500||!wifi_is_connected())return;
  attempted=true;uint8_t raw[64],context[256];size_t n=0;
  {SenseDiagnosticScope scope(sense_diag_remaining(deadline));if(!scope.active||!read(raw)||
    g_diag_journal.read_retained(0,context,256,n)!=halo_diag::Result::Ok||n!=256||!context_matches(context,raw))return;}
  halo_admission::EnvelopeIdentity id;if(!halo_admission::prepare_envelope(context,raw,sense_admission::hash,id))return;
  halo_diag::PostReceipt receipt;const halo_diag::PostLease lease{nullptr,sense_diag_export_idle,deadline,OTA_REPORT_HTTP_TIMEOUT_MS,nullptr};
  const auto result=post(context,raw,id,lease,receipt);
  Serial.printf("[OTA_DIAG_ADMISSION] export=%u http=%d elapsed=%lu heap=%lu/%lu largest=%lu/%lu\n",unsigned(result),receipt.status,(unsigned long)receipt.elapsed_ms,(unsigned long)receipt.free_before,(unsigned long)receipt.free_after,(unsigned long)receipt.largest_before,(unsigned long)receipt.largest_after);
  if(result==halo_diag::PostResult::Received&&sense_diag_export_idle(nullptr)&&ack(context,raw,receipt,deadline))
    (void)sense_diag_retire(sense_diag_remaining(deadline));
}
} // namespace sense_admission_export
static bool halo_diag_admission_retention_pending(){return sense_admission_export::retention_pending();}
static void halo_diag_admission_export(uint32_t deadline){sense_admission_export::export_once(deadline);}
#endif
