#pragma once
#include "DurableOtaOneShot.h"
#ifndef HALO_DURABLE_OTA_POLICY
#define HALO_DURABLE_OTA_POLICY 0
#endif
// Include after SenseDurablePolicyState and the existing main-task query,
// send_maint_window, pump, safety and version helpers. No UART reader/task.
namespace sense_one_shot {
enum class Result:uint8_t { Disabled, Invalid, Busy, Admission, Storage, Clock, Query, Peer, Ack, Deadline, Armed, TimerRecorded, TimerNotReady };
struct Request { durable_ota::Target target{}; uint8_t campaign[16]{}; uint32_t delay_s{600}; };
inline bool unhex(const char*s,uint8_t*out,size_t n){
  if(!s||strnlen(s,2*n+1)!=2*n)return false;
  for(size_t i=0;i<n;++i){unsigned v=0;for(unsigned j=0;j<2;++j){char c=s[2*i+j];int d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;if(d<0)return false;v=v*16+unsigned(d);}out[i]=uint8_t(v);}return true;
}
template<size_t N> inline bool copy_text(char(&out)[N],const char*s){
  if(!s||!s[0]||strnlen(s,N)>=N)return false;memset(out,0,N);memcpy(out,s,strlen(s));return durable_ota::text(out,N);
}
inline bool parse(JsonObjectConst doc,Request& out){
  if(!HALO_OTA_ONE_SHOT||!HALO_DURABLE_OTA_POLICY||!doc||doc.size()!=8||!doc["ver"].is<uint32_t>()||doc["ver"].as<uint32_t>()!=PROTOCOL_VERSION||!doc["msg_id"].is<uint32_t>()||!doc["ts"].is<uint32_t>()||!doc["type"].is<const char*>()||strcmp(doc["type"].as<const char*>(),"OTA_ONE_SHOT")||!doc["delay_s"].is<uint32_t>()||(HALO_OTA_BENCH_PROFILE?(doc["delay_s"].as<uint32_t>()<120||doc["delay_s"].as<uint32_t>()>900):doc["delay_s"].as<uint32_t>()!=600)||!doc["campaign"].is<const char*>()||!doc["sense"].is<JsonObjectConst>()||!doc["lcd"].is<JsonObjectConst>())return false;
  const auto s=doc["sense"].as<JsonObjectConst>(),l=doc["lcd"].as<JsonObjectConst>();
  if(s.size()!=4||l.size()!=3||!s["version"].is<const char*>()||!s["url"].is<const char*>()||!s["sha256"].is<const char*>()||!s["bytes"].is<uint32_t>()||!l["version"].is<const char*>()||!l["sha256"].is<const char*>()||!l["bytes"].is<uint32_t>())return false;
  out={};out.delay_s=doc["delay_s"].as<uint32_t>();
  if(!unhex(doc["campaign"].as<const char*>(),out.campaign,16)||!durable_ota::nonzero(out.campaign,16)||!copy_text(out.target.version,s["version"].as<const char*>())||!copy_text(out.target.url,s["url"].as<const char*>())||!copy_text(out.target.peer_version,l["version"].as<const char*>())||!unhex(s["sha256"].as<const char*>(),out.target.sha256,32)||!unhex(l["sha256"].as<const char*>(),out.target.peer_sha256,32))return false;
  out.target.bytes=s["bytes"].as<uint32_t>();out.target.peer_bytes=l["bytes"].as<uint32_t>();
  return out.target.peer_bytes&&durable_ota::nonzero(out.target.peer_sha256,32)&&!strcmp(out.target.version,out.target.peer_version)&&durable_ota::target_valid(out.target);
}
inline bool live(uint32_t start,uint32_t budget){return budget&&budget<=durable_ota::kPreflightMs&&uint32_t(millis()-start)<budget;}
#if HALO_OTA_ONE_SHOT && HALO_DURABLE_OTA_POLICY
inline bool idle(){
  return nvs_capacity_image_valid()&&!xPortInIsrContext()&&!self_retry_user_busy()&&sense_lcd_ota_retry_safe()&&
    !g_ota_apply_in_progress&&!g_lcd_ota_proxy_owns_uart&&!g_lcd_ota_task_running&&!g_spool_owns_uart&&!g_img_spool_tx_active&&
    !halo_primary_user_work_busy()&&!current_job.active&&!upload_inflight&&!http_inflight&&
    !upload_queue_count()&&!scan_ui_inflight&&!dish_scan_inflight;
}
struct Exchange {
  durable_ota::OneShotAck ack{};
  uint32_t start{},budget{},ack_start{},generation{};
  bool waiting=false,received=false,query_owned=false;
  char query[40]{};
};
static Exchange* active_exchange=nullptr;
struct ExchangeScope {
  Exchange& exchange;
  explicit ExchangeScope(Exchange& e):exchange(e){active_exchange=&e;}
  ~ExchangeScope(){
    exchange.waiting=false;
    if(exchange.query_owned&&!strcmp(s_lcd_query_requested_id,exchange.query))s_lcd_query_pending=false;
    active_exchange=nullptr;
  }
};
#endif
// Called synchronously by the existing main-task MAINT_WINDOW_ACK dispatcher.
// Transient parser strings are copied only after every expected field matches.
inline bool accept_ack(uint32_t remaining,uint32_t wake,bool clear,const char*request,const char*status,bool persisted,uint64_t due,uint32_t duration,uint32_t before,uint32_t after,const char*challenge,uint32_t peer_boot){
#if HALO_OTA_ONE_SHOT && HALO_DURABLE_OTA_POLICY
  Exchange* e=active_exchange;const auto*r=sense_policy::current();
  if(!e||!e->waiting||e->received||!r||r->generation!=e->generation||!live(e->start,e->budget)||uint32_t(millis()-e->ack_start)>=5000||!request||!status||!challenge)return false;
  char expected[64]{};
  if(!durable_ota::one_shot_request(*r,expected)||strcmp(request,expected)||strcmp(challenge,r->one_shot.challenge)||!peer_boot||peer_boot!=r->one_shot.peer_boot||peer_boot!=g_lcd_query_peer_boot_id||clear||!persisted||strcmp(status,"stored_verified")||due!=r->one_shot.due||remaining!=r->one_shot.sent_remaining||wake!=remaining||duration||before||after)return false;
  auto&a=e->ack;if(!copy_text(a.request,request)||!copy_text(a.challenge,challenge))return false;
  a.peer_boot=peer_boot;a.start_epoch=uint32_t(due);a.remaining_s=remaining;a.wake_in_s=wake;a.duration_s=duration;a.grace_before_s=before;a.grace_after_s=after;a.clear=clear;a.persisted=persisted;a.stored_verified=true;e->received=true;return true;
#else
  (void)remaining;(void)wake;(void)clear;(void)request;(void)status;(void)persisted;(void)due;(void)duration;(void)before;(void)after;(void)challenge;(void)peer_boot;return false;
#endif
}
inline Result execute(const Request& request,uint32_t original_start,uint32_t original_budget){
#if HALO_OTA_ONE_SHOT && HALO_DURABLE_OTA_POLICY
  if(active_exchange||!idle())return Result::Busy;
  if(!live(original_start,original_budget))return Result::Deadline;
  if(!durable_ota::target_valid(request.target)||!durable_ota::nonzero(request.campaign,16)||!request.target.peer_bytes||!durable_ota::nonzero(request.target.peer_sha256,32)||strcmp(request.target.version,request.target.peer_version))return Result::Invalid;
  auto clock=sense_policy::fresh_clock();
  if(!sense_policy::load_state(original_start,original_budget))return Result::Storage;
  const auto* before=sense_policy::current();
  const uint32_t configured=before?durable_ota::initial_delay(*before):600;
  if(request.delay_s!=configured)return Result::Admission;
  if(!clock.fresh||clock.epoch<durable_ota::kMinimumEpoch||uint64_t(clock.epoch)+configured+durable_ota::kOneShotWindow>UINT32_MAX)return Result::Clock;
  const uint32_t due=clock.epoch+configured,boot=g_coord_sense_boot_id;
  if(!boot)return Result::Admission;
  char origin[64]{};memcpy(origin,"ota_test_",9);static const char hex[]="0123456789abcdef";
  for(unsigned i=0;i<16;++i){origin[9+2*i]=hex[request.campaign[i]>>4];origin[10+2*i]=hex[request.campaign[i]&15];}
  if(before&&durable_ota::bench_active(*before)){
    char session[17]{};for(unsigned i=0;i<8;++i){session[2*i]=hex[before->bench.session[i]>>4];session[2*i+1]=hex[before->bench.session[i]&15];}
    char campaign[33]{};for(unsigned i=0;i<16;++i){campaign[2*i]=hex[request.campaign[i]>>4];campaign[2*i+1]=hex[request.campaign[i]&15];}
    snprintf(origin,sizeof(origin),"bench_%s_%s",session,campaign);
  }
  Exchange exchange{};exchange.start=original_start;exchange.budget=original_budget;ExchangeScope owned(exchange);
  // Atomic central API: persist ONLY charged PREPARING, never pristine FAST.
  if(!sense_policy::prepare_supplied_control(request.target,origin,request.campaign,due,boot,original_start,original_budget,clock))return Result::Admission;
  durable_ota::Record candidate{};
  auto fail=[&](Result reason){
    const auto*r=sense_policy::current();
    if(r&&live(original_start,original_budget)&&durable_ota::one_shot_close(*r,sense_policy::fresh_clock(),sense_policy::next_normal_epoch(),candidate))
      (void)sense_policy::commit_candidate(candidate,original_start,original_budget);
    // Expired/unsafe/uncertain storage retains charged preparation for central
    // reset/next-safe-boundary reconciliation. Failure never refunds it.
    return reason;
  };
  const auto* r=sense_policy::current();if(!r||r->phase!=durable_ota::Phase::ARM_PREPARATION||r->one_shot.phase!=durable_ota::OneShotPhase::PREPARING||r->one_shot.due!=due||r->one_shot.arm_boot!=boot)return Result::Storage;
  auto control_live=[&](){const auto*p=sense_policy::current();return p&&live(original_start,original_budget)&&uint32_t(millis()-original_start)<p->reserved_work_ms&&g_coord_sense_boot_id==boot&&idle();};
  if(!control_live())return fail(Result::Deadline);
  snprintf(exchange.query,sizeof(exchange.query),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
  uint32_t remaining=r->reserved_work_ms-uint32_t(millis()-original_start);
  if(!sense_lcd_ota_query_start(exchange.query,remaining))return fail(Result::Query);
  exchange.query_owned=true;LcdOtaQuerySnapshot peer{};bool ready=false;
  while(control_live()){
    pump_uart_rx_once();
    if(!control_live())break;
    const auto polled=sense_lcd_ota_query_poll(peer);
    if(polled==LCD_QUERY_READY){ready=true;break;}
    if(polled==LCD_QUERY_TIMEOUT)break;
    delay(10);
  }
  if(!control_live())return fail(Result::Deadline);
  r=sense_policy::current();
  if(!ready||!peer.correlated||!peer.peer_boot_id||!peer.boot_ready||strcmp(peer.running_state,"VALID")||!peer.running_part[0]||strcmp(peer.running_part,"?")==0||strcmp(peer.running_part,peer.boot_part)||peer.coord_waiting||peer.coord_lease_ms||peer.coord_owner[0]||peer.part_size<r->target.peer_bytes||compareSemver(peer.fw,r->target.peer_version)>0)return fail(Result::Peer);
  durable_ota::OneShotPeer observed{};observed.boot=peer.peer_boot_id;copy_text(observed.challenge,exchange.query);observed.query_matched=peer.correlated;observed.local_valid=nvs_capacity_image_valid();observed.peer_valid=true;observed.target_verified=true;observed.idle=idle();
  exchange.ack_start=millis();
  if(!durable_ota::one_shot_prepare(*r,sense_policy::fresh_clock(),due,observed,uint32_t(exchange.ack_start-original_start),boot,candidate)||!sense_policy::commit_candidate(candidate,original_start,original_budget))return fail(Result::Storage);
  r=sense_policy::current();exchange.generation=r->generation;
  if(!control_live()||uint32_t(millis()-exchange.ack_start)>=5000)return fail(Result::Deadline);
  MaintenanceWindow window{};window.duration_sec=window.grace_before_sec=window.grace_after_sec=0;window.start_epoch=r->one_shot.due;
  if(!durable_ota::one_shot_request(*r,window.request_id))return fail(Result::Storage);
  exchange.waiting=true;
  // Exactly one send. Persisted pending tuple and ACK clock were fixed above.
  send_maint_window(&window,r->one_shot.sent_remaining,r->one_shot.sent_remaining,false,r->one_shot.challenge,r->one_shot.peer_boot);
  while(control_live()&&uint32_t(millis()-exchange.ack_start)<5000){
    pump_uart_rx_once();
    if(!control_live()||uint32_t(millis()-exchange.ack_start)>=5000)break;
    if(exchange.received){
      r=sense_policy::current();
      if(!r||r->generation!=exchange.generation||!durable_ota::one_shot_confirm(*r,sense_policy::fresh_clock(),exchange.ack,g_lcd_query_peer_boot_id,g_coord_sense_boot_id,uint32_t(millis()-exchange.ack_start),uint32_t(millis()-original_start),idle(),candidate))return fail(Result::Ack);
      if(!sense_policy::commit_candidate(candidate,original_start,original_budget))return Result::Storage;
      if(!live(original_start,original_budget)||uint32_t(millis()-exchange.ack_start)>=5000||!idle()||!sense_policy::fresh_clock().fresh)return fail(Result::Deadline);
      return Result::Armed;
    }
    delay(10);
  }
  return fail(Result::Deadline);
#else
  (void)request;(void)original_start;(void)original_budget;return Result::Disabled;
#endif
}
// Select immediately before the actual final SDK call, not in an earlier
// report/coschedule pass. Caller retains this small proof through that call.
struct TimerProof {durable_ota::Clock selected{};uint32_t generation{},boot{};uint64_t microseconds{};durable_ota::OneShotSelection selection{durable_ota::OneShotSelection::NONE};};
inline TimerProof select_timer(){
  TimerProof proof{};
#if HALO_OTA_ONE_SHOT && HALO_DURABLE_OTA_POLICY
  const auto*r=sense_policy::current();if(!r)return proof;proof.selected=sense_policy::fresh_clock();const auto t=durable_ota::one_shot_timer(*r,proof.selected);proof.selection=t.state;proof.microseconds=t.microseconds;proof.generation=r->generation;proof.boot=g_coord_sense_boot_id;
#endif
  return proof;
}
inline Result note_timer(const TimerProof&proof,uint64_t actual_us,int32_t sdk_result,uint32_t start,uint32_t budget){
#if HALO_OTA_ONE_SHOT && HALO_DURABLE_OTA_POLICY
  const auto*r=sense_policy::current();const auto now=sense_policy::fresh_clock();
  if(!live(start,budget)||!idle()||!r||r->generation!=proof.generation||proof.selection!=durable_ota::OneShotSelection::WAIT||actual_us!=proof.microseconds||proof.boot!=g_coord_sense_boot_id||!now.fresh||now.epoch<proof.selected.epoch)return Result::TimerNotReady;
  durable_ota::Record candidate{};
  if(!durable_ota::one_shot_note_timer(*r,proof.selected,proof.boot,actual_us,sdk_result,candidate)||!sense_policy::commit_candidate(candidate,start,budget))return Result::Storage;
  return Result::TimerRecorded;
#else
  (void)proof;(void)actual_us;(void)sdk_result;(void)start;(void)budget;return Result::Disabled;
#endif
}
inline void append_report(JsonDocument& doc){
#if HALO_OTA_ONE_SHOT && HALO_DURABLE_OTA_POLICY
  const auto*r=sense_policy::current();if(!r||r->one_shot.phase==durable_ota::OneShotPhase::NONE)return;
  char id[64]{};if(!durable_ota::one_shot_request(*r,id))return;
  doc["oneshot_request"]=id;doc["oneshot_origin"]=r->origin;doc["oneshot_phase"]=uint8_t(r->one_shot.phase);doc["oneshot_due"]=r->one_shot.due;doc["oneshot_expiry"]=r->one_shot.expiry;doc["oneshot_lcd_boot"]=r->one_shot.peer_boot;doc["oneshot_ack_epoch"]=r->one_shot.ack_epoch;doc["oneshot_control_debited"]=r->one_shot.control_debited;doc["oneshot_timer_recorded"]=r->one_shot.timer_recorded;doc["oneshot_timer_epoch"]=r->one_shot.timer_epoch;doc["oneshot_timer_us"]=r->one_shot.timer_us;doc["oneshot_timer_sdk"]=r->one_shot.timer_sdk;doc["oneshot_arm_boot"]=r->one_shot.arm_boot;
#else
  (void)doc;
#endif
}
} // namespace sense_one_shot
