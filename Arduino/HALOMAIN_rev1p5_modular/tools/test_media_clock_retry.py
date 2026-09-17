#!/usr/bin/env python3
"""Compose actual media-clock admission, NTP generations, sleep and upload gate.

Fake DNS/SNTP/queue boundaries; no device or network calls. Reuses the existing
clock fixture, not a second model of the deadline/callback implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_manual_ota_clock import ROOT, definition, time_harness


def harness(root):
    clock = (root / 'Sense_Minimal/sense_time.h').read_text()
    main = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    text = time_harness(clock).split('int main(){', 1)[0]
    media = 'static void sense_ntp_request_media_retry(' in clock
    if media:
        text += definition(clock, 'static void sense_ntp_request_media_retry(')
        text = text.replace('now_ms=1000;policy_budget=budget;',
                            'g_ntp_media_retry_requested=false;now_ms=1000;policy_budget=budget;')
    else:
        text += '\nstatic void sense_ntp_request_media_retry(){}\n'
    owner = definition(main, 'static void service_boot_wifi_connect(')
    # Execute the unchanged actual clock sleep guard and actual worker custody
    # gate as well as the full owner function, without compiling unrelated UI.
    sleep = definition(main, 'static bool sense_can_sleep_now(')
    sleep = sleep[:sleep.index('  // An SD-spool transfer')] + '  return true;\n}\n'
    begin = main.index('      if (!wifi_is_connected() || !sense_time_has_fresh_sync()) {')
    gate = main[begin:main.index('      if (job.is_voice)', begin)]
    text += r'''
#include <cstdlib>
struct UploadJob {bool from_voice_sd=false,from_image_sd=false,from_persisted=false;void*image_buf=nullptr;};
static UploadJob queue_head;
static bool queued=true,wifi=true,guardian_force_sleep=false,upload_inflight=false;
static void* upload_queue=reinterpret_cast<void*>(1);
static unsigned peeks,persisted,tls_admissions,wifi_services,policy_services;
static const int pdTRUE=1;
static int xQueuePeek(void*,UploadJob*out,int wait){assert(wait==0);++peeks;if(!queued)return 0;*out=queue_head;return pdTRUE;}
static bool wifi_is_connected(){return wifi;}
static void wifi_service_events(){++wifi_services;}
static void service_wifi_maintenance(unsigned long){}
static void halo_policy_service_boot(){++policy_services;}
static void upload_persist_handle_failure(const UploadJob&,const char*why){assert(!strcmp(why,"network_or_clock_pending"));++persisted;}
''' + owner + '\n' + sleep + r'''
static void worker_gate(){UploadJob job;job.image_buf=malloc(1);upload_inflight=true;
 for(unsigned once=0;once<1;++once){
''' + gate + r'''
 ++tls_admissions;free(job.image_buf);upload_inflight=false;
 }}
static unsigned checks;
#define CHECK(value) do{++checks;if(!(value)){fprintf(stderr,"FAIL line%d: %s starts=%u budget=%u now=%u\n",__LINE__,#value,starts,g_ntp_attempt_budget_ms,now_ms);return 1;}}while(0)
static void scenario(uint32_t budget=15000){reset(budget);queue_head={};queued=true;wifi=true;guardian_force_sleep=false;
 upload_queue=reinterpret_cast<void*>(1);peeks=persisted=tls_admissions=wifi_services=policy_services=0;}
static void service(){service_boot_wifi_connect(now_ms);}
int main(){
 timeval tv{};tv.tv_sec=1800000000;
 // Actual owner sees fresh FIFO before begin, preserves primary, then starts
 // secondary in the SAME expiry iteration before an ordinary sleep decision.
 scenario();service();CHECK(starts==1&&g_ntp_attempt_budget_ms==15000);
 const auto primary_dns=g_ntp_resolve_until_ms.load();
 now_ms+=14999;service();CHECK(starts==1&&g_ntp_resolve_until_ms==primary_dns);
 ++now_ms;service();CHECK(starts==2&&g_ntp_attempt_budget_ms==20000&&g_ntp_manual_retry_used);
 CHECK(selected_servers[0]=="10.0.0.2"&&sense_ntp_attempt_pending());
 CHECK(!g_ntp_manual_retry_requested&&!g_ntp_scheduled_retry_requested);
 const auto fixed_start=g_ntp_attempt_start_ms;const auto fixed_until=g_ntp_accept_until_ms.load();
 const char* why=nullptr;CHECK(!sense_can_sleep_now(&why)&&!strcmp(why,"clock_sync_pending"));
 for(unsigned n=0;n<20;++n){now_ms+=100;service();CHECK(g_ntp_attempt_start_ms==fixed_start&&g_ntp_accept_until_ms==fixed_until);}
 sense_ntp_on_sync(&tv);CHECK(g_ntp_received_epoch==0);
 ip_addr_t late{};late.addr=99;sense_ntp_on_dns("old",&late,&g_ntp_servers[1]);CHECK(g_ntp_servers[1].ipv4==2);
 // High-water/competing worker never blocks waiting for clock or starts TLS.
 worker_gate();CHECK(persisted==1&&tls_admissions==0&&!upload_inflight);
 const auto before=now_ms;service();CHECK(now_ms==before); // owner service is nonblocking
 sync_callback(&tv);service();CHECK(sense_time_has_fresh_sync()&&cache_writes==1&&sense_can_sleep_now(nullptr));
 worker_gate();CHECK(tls_admissions==1&&persisted==1);
 // Failure is finite. A later tap/capture/manual/calendar cannot regenerate.
 scenario();service();now_ms+=15000;service();now_ms+=20000;service();
 CHECK(!sense_ntp_attempt_pending()&&!sense_time_has_fresh_sync()&&sense_can_sleep_now(nullptr));
 for(unsigned n=0;n<5;++n){sense_ntp_request_manual_retry();sense_ntp_request_scheduled_retry(now_ms+40000);service();}
 CHECK(starts==2&&g_ntp_attempt_budget_ms==20000);worker_gate();CHECK(persisted==1&&tls_admissions==0);
 // Already-fresh primary reply needs no retry, even at the expiry boundary.
 scenario();service();now_ms+=14999;sync_callback(&tv);now_ms+=2;service();
 CHECK(starts==1&&sense_time_has_fresh_sync()&&!g_ntp_manual_retry_used);
 // Empty/saved FIFO and disconnected owner do not gain media attempts.
 for(unsigned kind=0;kind<5;++kind){scenario();queued=kind!=0;queue_head.from_voice_sd=kind==1;
  queue_head.from_image_sd=kind==2;queue_head.from_persisted=kind==3;if(kind==4)upload_queue=nullptr;
  service();now_ms+=15000;service();CHECK(starts==1&&!g_ntp_manual_retry_used);}
 scenario();wifi=false;service();CHECK(starts==0&&peeks==0);wifi=true;service();CHECK(starts==1);
 // Fresh capture arriving after the primary timed out still has one chance.
 scenario();queued=false;service();now_ms+=15000;service();now_ms+=80000;queued=true;service();
 CHECK(starts==2&&g_ntp_attempt_budget_ms==20000&&g_ntp_attempt_start_ms==now_ms);
 // Existing qualified manual/calendar budgets retain precedence at admission.
 scenario();service();sense_ntp_request_manual_retry();now_ms+=15000;service();
 CHECK(starts==2&&g_ntp_attempt_budget_ms==40000);
 scenario();service();sense_ntp_request_scheduled_retry(now_ms+15000+31000);now_ms+=15000;service();
 CHECK(starts==2&&g_ntp_attempt_budget_ms==31000);
 scenario();service();sense_ntp_request_scheduled_retry(now_ms+15000+100000);now_ms+=15000;service();
 CHECK(starts==2&&g_ntp_attempt_budget_ms==40000);
 // An expired calendar deadline doesn't revive calendar work or deny media's
 // independent shorter window. Once media starts, later manual cannot extend.
 scenario();service();sense_ntp_request_scheduled_retry(now_ms+1000);now_ms+=15000;service();
 CHECK(g_ntp_attempt_budget_ms==20000);const auto media_start=g_ntp_attempt_start_ms;
 sense_ntp_request_manual_retry();sense_ntp_request_scheduled_retry(now_ms+40000);service();
 CHECK(g_ntp_attempt_start_ms==media_start&&g_ntp_attempt_budget_ms==20000);
 // Existing extended durable-OTA clock opportunity never gets another20s.
 scenario(55000);service();now_ms+=55000;service();CHECK(starts==1&&!g_ntp_manual_retry_used&&g_ntp_attempt_budget_ms==55000);
 // Missing DNS gets isolated slots; stale primary callback cannot fill retry.
 scenario();dns_available=false;service();CHECK(starts==0&&dns_requests==3);
 now_ms+=15000;service();CHECK(dns_requests==6&&g_ntp_manual_retry_used&&sense_ntp_attempt_pending());
 dns_reply(0,88);CHECK(g_ntp_servers[0].ipv4==0);dns_reply(4,2);service();CHECK(starts==1&&selected_servers[0]=="10.0.0.2");
 sync_callback(&tv);service();CHECK(sense_time_has_fresh_sync());
 // DNS guard holds and late service never renew secondary DNS or SNTP time.
 scenario();service();halo_sntp_dns_acquire();now_ms+=15000;service();
 CHECK(starts==1&&g_ntp_manual_retry_used&&g_ntp_attempt_budget_ms==20000);
 now_ms+=1000;halo_sntp_dns_release();service();CHECK(starts==2&&g_ntp_attempt_start_ms==16000);
 now_ms+=19000;service();CHECK(!sense_ntp_attempt_pending());
 // Sleep quiesce, guardian exception, and monotonic wrap retain boundaries.
 scenario();service();sense_ntp_quiesce_for_sleep();now_ms+=15000;service();
 CHECK(starts==1&&!g_ntp_manual_retry_used&&!sense_ntp_attempt_pending());
 scenario();service();guardian_force_sleep=true;CHECK(sense_can_sleep_now(nullptr));
 scenario();now_ms=UINT32_MAX-5000;service();now_ms+=15000;service();
 CHECK(starts==2&&sense_ntp_attempt_pending());now_ms+=19999;service();CHECK(sense_ntp_attempt_pending());
 ++now_ms;service();CHECK(!sense_ntp_attempt_pending()&&!sense_time_has_fresh_sync());
 printf("PASS %u actual media clock/owner/sleep/worker assertions\n",checks);return 0;
}
'''
    return text


def execute(root, out, sanitize):
    out.mkdir(parents=True, exist_ok=True)
    cpp = out / 'media_clock.cpp'
    cpp.write_text(harness(root))
    cmd = [shutil.which('clang++') or 'c++', '-std=c++17', '-O1', '-g', str(cpp), '-o', str(out / 'media_clock')]
    if sanitize:
        cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    build = subprocess.run(cmd, text=True, capture_output=True)
    (out / 'build.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        return {'compiled': False, 'exit_code': build.returncode, 'output': build.stderr}
    run = subprocess.run([str(out / 'media_clock')], text=True, capture_output=True, timeout=20)
    (out / 'run.log').write_text(run.stdout + run.stderr)
    return {'compiled': True, 'exit_code': run.returncode, 'output': run.stdout + run.stderr}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--out', type=Path)
    p.add_argument('--negative-source-root', type=Path)
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-media-clock-') as tmp:
        out = a.out or Path(tmp)
        r = {'current': execute(a.source_root, out / 'current', a.sanitize)}
        r['source_sha256'] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in
                              [a.source_root / 'Sense_Minimal/sense_time.h', a.source_root / 'Sense_Minimal/Sense_Minimal.ino']}
        if a.negative_source_root:
            r['negative'] = execute(a.negative_source_root, out / 'negative', False)
        r['pass'] = r['current']['compiled'] and r['current']['exit_code'] == 0
        if 'negative' in r:
            r['pass'] &= r['negative']['compiled'] and r['negative']['exit_code'] != 0
        (out / 'RESULT.json').write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps(r, indent=2))
        return 0 if r['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
