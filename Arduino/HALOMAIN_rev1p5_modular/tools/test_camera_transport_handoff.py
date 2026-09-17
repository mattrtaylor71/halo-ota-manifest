#!/usr/bin/env python3
"""Actual camera initialization versus delayed transport/DMA RAII teardown.

Uses actual camera functions, network admission and voice DMA destructor with
scripted timing/allocator boundaries. No device, network, or policy writes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_camera_dma_ownership import ROOT, harness as camera_harness
from test_voice_persistence import definition


def harness(root):
    text = camera_harness(root).split('int main(){', 1)[0]
    old_delay = definition(text, 'static void delay(')
    text = text.replace(old_delay, r'''
static uint32_t handoff_started,http_done_ms,transport_done_ms;
static bool probe_admission;
static void finish_transport_if_due();
static void delay(uint32_t n){
 now_ms+=n;
 if(uint32_t(now_ms-handoff_started)>=http_done_ms)http_inflight=false;
 finish_transport_if_due();
 if(probe_admission){SenseBackupWifiCall newcomer;check(!newcomer,"new network entrants excluded during handoff/capture");}
}
''')
    text = text.replace('++init_count;events.push_back("camera_init");',
                        'check(sense_backup_diag::wifi_calls.load()==0,"camera must wait for complete transport/DMA scope, not HTTP flag");\n ++init_count;events.push_back("camera_init");')
    dma = definition((root / 'Sense_Minimal/sense_voice.h').read_text(), 'struct DmaGuardVoice') + ';'
    text += r'''
struct UnwindingTransport {
 SenseBackupWifiCall lease;
''' + dma + r'''
 DmaGuardVoice restore{true};
};
static UnwindingTransport* transport;
static unsigned cleanup_count;
static void finish_transport_if_due(){
 if(transport&&uint32_t(now_ms-handoff_started)>=transport_done_ms){
  delete transport;transport=nullptr;++cleanup_count;
  check(sense_backup_diag::wifi_calls.load()==0,"outer network scope releases after DMA restore");
 }
}
static void scenario(uint32_t start,uint32_t http_done,uint32_t transport_done,bool probe=false){
 if(transport){delete transport;transport=nullptr;}reset();now_ms=handoff_started=start;
 http_done_ms=http_done;transport_done_ms=transport_done;probe_admission=probe;cleanup_count=0;
 connected_largest=40000; // admission must be safe even when a heap snapshot looks ample
 transport=new UnwindingTransport;check(bool(transport->lease),"old transport admitted");
 http_inflight=http_done!=0;g_camera_dma_reserve=nullptr;
}
int main(){
 // The physical182 sequence: HTTP clears, but the outer call remains alive
 // while diagnostics and its DMA guard unwind. The old code allocates here.
 scenario(1000,40,160);
 check(init_camera(),"capture waits for final transport destructor then succeeds");
 check(cleanup_count==1&&uint32_t(now_ms-handoff_started)>=160,"complete transport cleanup precedes capture");
 check(init_count==1&&g_camera_radio_off_owned.load(),"exclusive admission remains through capture");
 {SenseBackupWifiCall next;check(!next,"no subsequent TLS can steal camera DMA");}
 probe_admission=false;deinit_camera();check(!g_camera_radio_off_owned.load(),"cleanup releases owner");
 // HTTP may have unlocked before init_camera even begins; the full call is
 // still a mandatory drain and reserve restoration cannot race allocation.
 for(uint32_t start:{0U,1000U,UINT32_MAX-100U})
 for(uint32_t final_release:{20U,200U,1500U,8000U,19980U}){
  scenario(start,0,final_release,true);
  check(init_camera(),"late complete scope retained as the same camera request");
  check(cleanup_count==1&&init_count==1,"no speculative first allocation before cleanup");
  check(!g_camera_dma_reserve,"restored reserve handed directly to camera");
  probe_admission=false;deinit_camera();
 }
 // A stalled owner or contended mutex ends finitely without camera allocation
 // or radio teardown. Even caller-side deinit must not reserve DMA against TLS.
 for(bool http_live:{false,true}){
  scenario(1000,http_live?UINT32_MAX:0,UINT32_MAX,true);
  check(!init_camera(),"stalled transport reports finite admission failure");
  check(uint32_t(now_ms-handoff_started)==20000,"drain has one fixed20s budget");
  check(init_count==0&&!radio_off_count&&!g_camera_radio_off_owned.load(),"timeout has no camera/radio side effects");
  probe_admission=false;deinit_camera();check(!g_camera_dma_reserve,"timeout cleanup cannot race TLS reserve");
  delete transport;transport=nullptr;check(g_camera_dma_reserve.load()!=nullptr,"transport eventually restores its reserve");
 }
 reset();handoff_started=now_ms;http_done_ms=transport_done_ms=UINT32_MAX;probe_admission=false;
 mutex_busy=true;check(!init_camera()&&init_count==0,"HTTP mutex contention never falls through to allocate");
 check(uint32_t(now_ms-handoff_started)==20000&&!g_camera_radio_off_owned.load(),"contended lease expires and releases gate");
 // Normal adequate-memory capture leaves Wi-Fi connected, but still excludes
 // new network allocations until its existing deinit restores the reserve.
 reset();handoff_started=now_ms;connected_largest=40000;
 check(init_camera()&&radio_off_count==0,"ordinary capture does not gain a radio reset");
 {SenseBackupWifiCall next;check(!next,"camera initialization/capture owns network admission");}
 deinit_camera();{SenseBackupWifiCall next;check(bool(next),"next network call admitted after complete camera cleanup");}
 printf("PASS %u actual camera/full-transport handoff assertions\n",checks);
}
'''
    return text


def execute(root, out):
    out.mkdir(parents=True, exist_ok=True)
    source = out / 'handoff.cpp'
    source.write_text(harness(root))
    cmd = [shutil.which('clang++') or 'c++', '-std=c++17', '-O1', '-g',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(source), '-o', str(out / 'handoff')]
    build = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    (out / 'build.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        return {'compiled': False, 'exit_code': build.returncode, 'output': build.stderr}
    run = subprocess.run([str(out / 'handoff')], capture_output=True, text=True, timeout=20)
    (out / 'run.log').write_text(run.stdout + run.stderr)
    return {'compiled': True, 'exit_code': run.returncode, 'output': run.stdout + run.stderr}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--out', type=Path)
    p.add_argument('--negative-source-root', type=Path)
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-camera-transport-') as tmp:
        out = a.out or Path(tmp)
        r = {'current': execute(a.source_root, out / 'current')}
        source = a.source_root / 'Sense_Minimal/sense_camera.h'
        r['source_sha256'] = hashlib.sha256(source.read_bytes()).hexdigest()
        if a.negative_source_root:
            r['negative'] = execute(a.negative_source_root, out / 'negative')
        r['pass'] = r['current']['compiled'] and r['current']['exit_code'] == 0
        if 'negative' in r:
            r['pass'] &= r['negative']['compiled'] and r['negative']['exit_code'] != 0
        (out / 'RESULT.json').write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps(r, indent=2))
        return 0 if r['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
