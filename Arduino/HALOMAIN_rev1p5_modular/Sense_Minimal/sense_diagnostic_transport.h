#pragma once
#include "../halo_ota_demo/firmware/shared/DiagnosticHandoff.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

// Include after the existing report TLS configurator, HaloNtpDnsGuard and
// ScopedTlsDmaReserve declarations. Reuses their current policy unchanged.
namespace halo_diag {
enum class PostResult:uint8_t { Disabled,Busy,Invalid,BeginFailed,HttpRejected,ReceiptSize,Truncated,Deadline,Received,RecoverySkipped };
struct PostLease {void*arg;bool(*live)(void*);uint32_t deadline_ms;uint32_t io_timeout_ms;const char*owner_id;};
struct PostReceipt : sense_post::Response {
  static_assert(MAX_RECEIPT_BYTES==sense_post::BODY_MAX,"Exact receipt capacity");
  uint32_t free_before,largest_before,min_before,free_after,largest_after,min_after;
};
inline uint32_t post_remaining(const PostLease&lease){
  if(!lease.live||!lease.live(lease.arg))return 0;
  int32_t left=int32_t(lease.deadline_ms-millis());return left>0?uint32_t(left):0;
}
inline uint16_t post_io_timeout(const PostLease&lease){uint32_t n=post_remaining(lease);if(n>lease.io_timeout_ms)n=lease.io_timeout_ms;if(n>65535)n=65535;return uint16_t(n);}
struct PostMeasure {
  PostReceipt&r;uint32_t start;
  explicit PostMeasure(PostReceipt&out):r(out),start(millis()){
    r=PostReceipt{};r.free_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    r.largest_before=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    r.min_before=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
  }
  ~PostMeasure(){r.elapsed_ms=millis()-start;r.free_after=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);r.largest_after=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);r.min_after=heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);}
};
struct PostBuffer {uint8_t*data;size_t used;static bool append(void*arg,const char*p,size_t n){PostBuffer&b=*static_cast<PostBuffer*>(arg);if(n>MAX_ENVELOPE_BYTES-b.used)return false;memcpy(b.data+b.used,p,n);b.used+=n;return true;}};
// No ACK/storage operation here. Caller validates the actual receipt against
// frozen envelope identity only after Received; a transport ACK is insufficient.
// Shared helper bounds cooperative request/cleanup. Existing watchdog retains
// custody if an SDK, mutex, allocator or TCPIP task itself stalls.
inline PostResult post_diagnostic(const uint8_t*ctx,const uint8_t*record,const EnvelopeIdentity&id,PostLease lease,PostReceipt&receipt,const Handoff*handoff=nullptr){
  PostMeasure measure(receipt);
  if(!OTA_REPORT_HTTP_URL[0]||strncmp(OTA_REPORT_HTTP_URL,"https://",8))return PostResult::Disabled;
  if(!lease.io_timeout_ms||!lease.live||!post_remaining(lease)||!wifi_is_connected())return PostResult::Busy;
  // Select the smaller report allowance once; later checks never rearm it.
  if(lease.io_timeout_ms>OTA_REPORT_HTTP_TIMEOUT_MS)lease.io_timeout_ms=OTA_REPORT_HTTP_TIMEOUT_MS;
  if(!lease.io_timeout_ms)return PostResult::Busy;
  const uint32_t guard_deadline=lease.deadline_ms;
  const uint32_t now=millis();const int32_t left=int32_t(lease.deadline_ms-now);
  if(left<=0)return PostResult::Deadline;
  if(uint32_t(left)>lease.io_timeout_ms)lease.deadline_ms=now+lease.io_timeout_ms;
  if(lease.owner_id&&lease.owner_id[0]&&!key_component(lease.owner_id,64))return PostResult::Invalid;
  uint8_t request[MAX_ENVELOPE_BYTES];PostBuffer buffer{request,0};size_t length=0;
  const bool encoded=handoff
      ? write_handoff_envelope(ctx,record,*handoff,id,&buffer,PostBuffer::append,length)
      : write_envelope(ctx,record,id,&buffer,PostBuffer::append,length);
  if(!encoded||length!=buffer.used)return PostResult::Invalid;
  if(!post_remaining(lease))return PostResult::Deadline;
  sense_idle_network::Guard recovery((handoff?halo_idle_net::Op::Handoff:halo_idle_net::Op::Diagnostic),guard_deadline);
  if(!recovery.active())return PostResult::RecoverySkipped;
  HaloNtpDnsGuard dns_guard;
  ScopedTlsDmaReserve reserve_guard(true);
  const sense_post::Request request_post{OTA_REPORT_HTTP_URL,id.device,lease.owner_id,id.request,request,length,true};
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
  // The helper proved response completion inside the original2s request and
  // cleanup inside its separate reserve. ACK still requires this actual owner.
  if(!lease.live(lease.arg)||!sense_post::remaining(guard_deadline))return failure(PostResult::Deadline);
  if(receipt.status!=200)return failure(PostResult::HttpRejected);
  if(!response.length||response.length>MAX_RECEIPT_BYTES)return failure(PostResult::ReceiptSize);
  // Response storage is already the exact caller-owned receipt; no second copy.
  return PostResult::Received;
}
} // namespace halo_diag
