"""Actual manual clock/time functions with deterministic network boundaries.

No firmware/network/hardware access. The native tests retain the production
callbacks, DNS guards, deadlines, retry admission and OTA terminal ordering.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def definition(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        if text.startswith('//', end):
            end = text.index('\n', end)
            continue
        if text.startswith('/*', end):
            end = text.index('*/', end) + 2
            continue
        if text[end] in ('"', "'"):
            quote = text[end]
            end += 1
            while text[end] != quote:
                end += 2 if text[end] == '\\' else 1
            end += 1
            continue
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def time_harness():
    source = (ROOT / 'Sense_Minimal/sense_time.h').read_text()
    state = source[source.index('static const uint32_t SENSE_NTP_ATTEMPT_MS'):
                   source.index('// ── Time cache functions')]
    signatures = [
        'static void sense_ntp_accept_sync(',
        'static void sense_ntp_on_sync(',
        'static void sense_ntp_on_manual_sync(',
        'static void sense_ntp_on_dns(',
        'static void sense_ntp_stop_locked(',
        'static bool sense_ntp_attempt_pending(',
        'static bool sense_time_has_fresh_sync(',
        'static void sense_ntp_request_manual_retry(',
        'static void sense_ntp_try_manual_retry_locked(',
        'static void sense_ntp_begin(',
        'extern "C" void halo_sntp_dns_acquire(',
        'extern "C" void halo_sntp_dns_release(',
        'static void sense_ntp_service(',
        'static void sense_ntp_quiesce_for_sleep(',
    ]
    prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <sys/time.h>
static uint32_t now_ms=1000,policy_budget=15000;
static unsigned dns_requests,starts,stops,cache_writes,core_depth;
static bool dns_available=true;
static uint32_t millis(){return now_ms;}
static std::recursive_mutex g_time_mutex;
static const time_t TIME_VALID_MIN_EPOCH=1700000000;
static const char*g_tz_current="PST8PDT,M3.2.0,M11.1.0";
static std::vector<std::string> selected_servers;
static void(*sync_callback)(timeval*)=nullptr;
struct ip_addr_t{uint32_t addr=0;};
using err_t=int;
#define IP_IS_V4(p) true
#define ip_2_ip4(p) (p)
#define ip4_addr_get_u32(p) ((p)->addr)
#define ERR_OK 0
#define ERR_INPROGRESS 1
#define LWIP_DNS_ADDRTYPE_IPV4 0
#define SNTP_SYNC_MODE_IMMED 0
#define CONFIG_LWIP_SNTP_MAX_SERVERS 3
#define HALO_SENSE_PROD_WRAPPER 1
#define HALO_DURABLE_OTA_POLICY 1
#define LOCK_TCPIP_CORE() (++core_depth)
#define UNLOCK_TCPIP_CORE() (--core_depth)
static uint32_t halo_policy_ntp_budget(uint32_t){return policy_budget;}
static err_t dns_gethostbyname_addrtype(const char* name,ip_addr_t*ip,
 void(*)(const char*,const ip_addr_t*,void*),void*,int){
 assert(core_depth==1);++dns_requests;
 if(!dns_available)return ERR_INPROGRESS;
 ip->addr=!strcmp(name,"pool.ntp.org")?1:!strcmp(name,"time.nist.gov")?2:3;
 return ERR_OK;
}
struct FakeString{
 uint32_t ip;void toCharArray(char*out,size_t n){snprintf(out,n,"10.0.0.%u",ip);}
};
struct IPAddress{uint32_t ip;explicit IPAddress(uint32_t v):ip(v){}FakeString toString(){return {ip};}};
static void sntp_stop(){assert(core_depth==1);++stops;}
static void esp_sntp_set_sync_mode(int){}
static void esp_sntp_set_time_sync_notification_cb(void(*cb)(timeval*)){sync_callback=cb;}
static void configTime(int,int,const char*a,const char*b,const char*c){
 ++starts;selected_servers={a,b,c};
 for(const auto&s:selected_servers)assert(s.rfind("10.0.0.",0)==0);
}
static void fake_setenv(const char*,const char*,int){}
static void fake_tzset(){}
#define setenv fake_setenv
#define tzset fake_tzset
static void time_cache_store(time_t epoch,bool authoritative){assert(epoch>=TIME_VALID_MIN_EPOCH&&authoritative);++cache_writes;}
static struct{template<class...A>void printf(const char*,A...){}void println(const char*){}}Serial;
'''
    cases = r'''
static void reset(uint32_t budget=15000){
 now_ms=1000;policy_budget=budget;dns_requests=starts=stops=cache_writes=core_depth=0;
 dns_available=true;selected_servers.clear();sync_callback=nullptr;
 g_ntp_attempt_budget_ms=15000;g_ntp_attempt_started=g_ntp_attempt_finished=false;
 g_ntp_running=g_ntp_sleep_quiesced=false;g_ntp_attempt_start_ms=0;g_ntp_dns_users=0;
 g_ntp_accept_until_ms=0;g_ntp_received_epoch=0;g_ntp_fresh_this_boot=false;
 g_ntp_manual_retry_requested=false;g_ntp_manual_retry_used=false;g_ntp_resolve_until_ms=0;
 for(auto&s:g_ntp_servers){s.requested=false;s.ipv4=0;s.done=false;}
}
static void expire(){now_ms=g_ntp_attempt_start_ms+g_ntp_attempt_budget_ms;sense_ntp_service();}
int main(){
 // An ordinary boot does not gain a second attempt without a manual action.
 reset();sense_ntp_begin();assert(starts==1&&dns_requests==3&&sense_ntp_attempt_pending());
 expire();sense_ntp_begin();assert(g_ntp_attempt_finished&&starts==1&&!sense_time_has_fresh_sync());
 // Manual requested while pending: original budget/DNS deadline unchanged;
 // expiry automatically opens one bounded numeric-only alternate-server retry.
 reset();sense_ntp_begin();const auto dns_deadline=g_ntp_resolve_until_ms.load();
 now_ms+=5000;sense_ntp_request_manual_retry();sense_ntp_begin();
 assert(g_ntp_attempt_budget_ms==15000&&g_ntp_resolve_until_ms==dns_deadline&&starts==1);
 expire();sense_ntp_begin();const auto retry_deadline=g_ntp_accept_until_ms.load();
 assert(g_ntp_manual_retry_used&&g_ntp_attempt_budget_ms==40000&&starts==2&&dns_requests==3);
 assert(g_ntp_resolve_until_ms==0&&selected_servers[0]=="10.0.0.2"&&sense_ntp_attempt_pending());
 timeval tv{};tv.tv_sec=1800000000;
 sense_ntp_on_sync(&tv);assert(g_ntp_received_epoch==0); // stale old callback
 ip_addr_t late{};late.addr=99;sense_ntp_on_dns("late",&late,&g_ntp_servers[1]);
 assert(g_ntp_servers[1].ipv4==2); // no reopened DNS callback generation
 now_ms+=2000;sync_callback(&tv);sense_ntp_service();
 assert(sense_time_has_fresh_sync()&&cache_writes==1&&g_ntp_accept_until_ms==0);
 // Manual requested AFTER the ordinary boot window has already expired.
 reset();sense_ntp_begin();expire();now_ms+=20000;sense_ntp_request_manual_retry();sense_ntp_begin();
 assert(starts==2&&g_ntp_manual_retry_used&&g_ntp_attempt_budget_ms==40000);
 expire();sync_callback(&tv);sense_ntp_service();assert(!sense_time_has_fresh_sync());
 for(unsigned i=0;i<4;++i){sense_ntp_request_manual_retry();sense_ntp_begin();}
 assert(starts==2&&dns_requests==3&&g_ntp_attempt_finished); // repeated taps cannot renew it
 // A retained durable campaign already has55s. Its budget/deadline is unchanged.
 reset(55000);sense_ntp_begin();sense_ntp_request_manual_retry();expire();sense_ntp_begin();
 assert(starts==1&&g_ntp_attempt_budget_ms==55000&&!g_ntp_manual_retry_used);
 // DNS guards stop synchronously and cannot cause a new deadline on resume.
 reset();sense_ntp_begin();sense_ntp_request_manual_retry();expire();
 halo_sntp_dns_acquire();halo_sntp_dns_acquire();sense_ntp_begin();
 assert(starts==1&&g_ntp_manual_retry_used&&sense_ntp_attempt_pending());
 const auto fixed_start=g_ntp_attempt_start_ms;
 now_ms+=1000;halo_sntp_dns_release();sense_ntp_begin();assert(starts==1);
 halo_sntp_dns_release();sense_ntp_begin();assert(starts==2&&g_ntp_attempt_start_ms==fixed_start);
 assert(g_ntp_accept_until_ms==fixed_start+40000&&dns_requests==3);
 halo_sntp_dns_acquire();sync_callback(&tv);assert(g_ntp_received_epoch==0);
 now_ms+=500;halo_sntp_dns_release();sense_ntp_begin();
 assert(g_ntp_accept_until_ms==fixed_start+40000&&dns_requests==3);
 // No cached numeric address: don't create a new hostname/DNS generation.
 reset();dns_available=false;sense_ntp_begin();expire();sense_ntp_request_manual_retry();sense_ntp_begin();
 assert(starts==0&&dns_requests==3&&!g_ntp_manual_retry_used&&g_ntp_attempt_finished);
 // Sleep quiescence wins, including after a manual request was registered.
 reset();sense_ntp_begin();sense_ntp_request_manual_retry();sense_ntp_quiesce_for_sleep();
 expire();sense_ntp_begin();sense_ntp_on_sync(&tv);sense_ntp_on_manual_sync(&tv);
 assert(starts==1&&g_ntp_received_epoch==0&&!sense_time_has_fresh_sync());
 // A timely reply is preserved if normal-loop service arrives after deadline.
 reset();sense_ntp_begin();now_ms+=14000;sense_ntp_on_sync(&tv);now_ms+=2000;sense_ntp_service();
 sense_ntp_request_manual_retry();sense_ntp_begin();assert(sense_time_has_fresh_sync()&&starts==1);
 // Monotonic32-bit wrap retains both finite windows.
 reset();now_ms=UINT32_MAX-5000;sense_ntp_begin();sense_ntp_request_manual_retry();
 now_ms+=14999;assert(sense_ntp_attempt_pending());now_ms+=1;sense_ntp_service();sense_ntp_begin();
 assert(g_ntp_manual_retry_used&&sense_ntp_attempt_pending());
 now_ms+=40000;sense_ntp_service();assert(!sense_ntp_attempt_pending()&&g_ntp_attempt_finished);
 puts("PASS manual clock: ordinary/durable windows, during/after timeout, cached alternate server, one retry, callback generations, DNS guards, sleep, late service and wrap");
}
'''
    return '\n'.join([prefix, state, *[definition(source, s) for s in signatures], cases])


def terminal_harness():
    wrapper = (ROOT / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino').read_text()
    prefix = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
static bool fresh,pending,manual,g_peer_episode_finished,g_ota_check_done,g_ota_check_requested,g_boot_ota_pending;
static unsigned retries,kicks,clears,finishes,begins;
static bool expire_after_kick,retry_ready;
static std::string last_result="none",wire_result;
static bool sense_time_has_fresh_sync(){return fresh;}
static bool halo_ota_manual_override_active(){return manual;}
static void sense_ntp_request_manual_retry(){++retries;}
static void halo_prod_kick_time_sync(const char*){++kicks;}
static bool sense_ntp_attempt_pending(){
 if(expire_after_kick){pending=false;expire_after_kick=false;retry_ready=true;}
 return pending;
}
static void sense_ntp_service(){}
static void sense_ntp_begin(){++begins;if(retry_ready){retry_ready=false;pending=true;}}
#define LOG_ERROR(...) ((void)0)
static void diag_record_error_persistent(const char*,int,const char*){}
static void ota_set_last_result(const char*r){last_result=r;}
static void ota_peer_cancel(const char*){wire_result=last_result;}
struct OtaIntent{static void clearForceAndCheck(){++clears;}};
static void manual_ota_override_clear(const char*){manual=false;}
static void boot_ota_finish(const char*){++finishes;}
'''
    cases = r'''
int main(){
 fresh=true;manual=true;assert(ota_clock_ready_before_work()&&retries==0&&wire_result.empty());
 fresh=false;pending=true;assert(!ota_clock_ready_before_work()&&retries==1&&wire_result.empty()&&clears==0);
 // Initial expiry falls after kick's begin but before pending is observed.
 // The second service/begin must grant recovery instead of returning failure.
 expire_after_kick=true;assert(!ota_clock_ready_before_work());
 assert(pending&&begins==1&&wire_result.empty()&&clears==0);
 pending=false;assert(!ota_clock_ready_before_work());
 assert(wire_result=="clock_unconfirmed"&&last_result==wire_result);
 assert(wire_result.find("defer")==std::string::npos); // LCD must not promise a new scheduled retry
 assert(g_peer_episode_finished&&g_ota_check_done&&!g_ota_check_requested&&!manual&&clears==1);
 const auto before=retries;g_boot_ota_pending=true;assert(!ota_clock_ready_before_work());
 assert(retries==before&&finishes==1); // automatic check does not add manual opportunity
 puts("PASS actual OTA preflight: fresh mandatory, pending nonterminal, accurate failure before unlock, no false scheduled-retry result");
}
'''
    return '\n'.join([prefix, definition(wrapper, 'static bool ota_clock_ready_before_work()'), cases])


class ManualOtaClockTests(unittest.TestCase):
    def compile_run(self, text):
        with tempfile.TemporaryDirectory(prefix='halo-manual-clock-') as directory:
            path = Path(directory)
            cpp, binary = path/'check.cpp', path/'check'
            cpp.write_text(text)
            subprocess.run([shutil.which('c++'), '-std=c++17', str(cpp), '-o', str(binary)], check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=5)

    def test_actual_clock_recovery(self):
        self.compile_run(time_harness())

    def test_actual_terminal_ordering(self):
        self.compile_run(terminal_harness())


if __name__ == '__main__':
    unittest.main()
