#!/usr/bin/env python3
"""Actual init/cleanup against the measured persistent13,812-byte DMA hole.

Host coverage proves mode selection, pre-init readback, sensor gating and
ownership/cleanup. The pinned SDK guard and real JPEG bench tests are separate.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_camera_dma_ownership import ROOT, harness

CASES = r'''
static void fragmented(){
 reset();fragment_without_wifi=true;connected_largest=13812;fail_reserve=true;
}
static void clean(){
 check(!g_camera_psram_dma_allocation.load(),"allocation correction scope never leaks past init");
 check(!camera_allocated&&!grab_worker&&!g_camera_radio_off_owned.load(),"failed init fully cleans camera/worker/network ownership");
 check(!mutex.held,"HTTP lease released");
}
int main(){
 fragmented();check(init_camera(),"measured persistent13812-byte hole captures with PSRAM DMA");
 check(psram_mode&&init_count==1&&mode_sets==1,"one verified fallback before first allocation");
 check(grab_worker&&camera_allocated,"worker starts after accepted OV2640 init");
 check(!g_camera_psram_dma_allocation.load(),"successful init closes allocation scope");
 deinit_camera();clean();
 // The driver retains its mode after deinit. Do not accidentally convert a
 // later adequate-memory capture (or next wake) to the new path.
 fragment_without_wifi=false;connected_largest=40000;radio_mode=WIFI_STA;
 check(init_camera(),"normal capture after fallback succeeds");
 check(!psram_mode&&mode_sets==2&&init_count==2,"fresh init restores internal mode when it fits");
 deinit_camera();clean();
 for(size_t largest:{size_t(16384),size_t(32768),size_t(65536)}){
  reset();radio_mode=WIFI_OFF;fragment_without_wifi=true;connected_largest=largest;
  check(init_camera()&&!psram_mode&&mode_sets==0,"adequate block uses unchanged path");
  deinit_camera();clean();
 }
 fragmented();fail_init_count=1;check(init_camera(),"fallback driver init failure retries boundedly");
 check(init_count==2&&mode_sets==1,"mode remains selected across failed-driver teardown");
 deinit_camera();clean();
 fragmented();fail_init_count=2;check(!init_camera(),"two fallback failures propagate");clean();
 fragmented();psram_bytes=0;check(!init_camera(),"no external RAM never pretends to recover");
 check(!psram_mode&&init_count==2,"original finite internal attempts preserved");clean();
 for(uint16_t pid:{uint16_t(0),uint16_t(0x3660),uint16_t(0x5640)}){
  fragmented();sensor_pid=pid;check(!init_camera(),"unqualified padded-JPEG sensor rejected");
  check(std::find(events.begin(),events.end(),"grab_start")==events.end(),"unknown PID rejected before any worker/frame consumption");clean();
  reset();sensor_pid=pid;connected_largest=40000;
  check(init_camera()&&!psram_mode,"previous normal-DMA sensor support unchanged");deinit_camera();clean();
 }
 fragmented();wrong_mode_readback=true;
 check(!init_camera()&&init_count==0,"INVALID_STATE without requested readback is rejected");clean();
 fragmented();reject_mode=true;
 check(!init_camera()&&init_count==0,"unsupported setter is not ignored");clean();
 fragmented();mode_result=-7;
 check(!camera_prepare_dma_mode(true),"unexpected setter error rejected even with matching mode");
 fragmented();mode_result=ESP_OK;
 check(init_camera(),"successful setter status also accepted with matching readback");deinit_camera();clean();
 fragmented();camera_allocated=true;
 check(!camera_prepare_dma_mode(true)&&mode_sets==0,"live camera cannot be reconfigured by preparation helper");
 reset();http_inflight=true;
 check(!init_camera()&&mode_sets==0&&init_count==0,"busy transport cannot reach fallback");clean();
 printf("PASS %u camera PSRAM fallback assertions\n",checks);
}
'''


def execute(program, folder, name):
    cpp, binary = folder / (name + '.cpp'), folder / name
    cpp.write_text(program)
    subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17',
                    '-fsanitize=address,undefined', str(cpp), '-o', str(binary)],
                   check=True, timeout=30)
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=20)


def main():
    prefix = harness(ROOT).split('int main(){', 1)[0]
    with tempfile.TemporaryDirectory(prefix='halo-psram-fallback-') as name:
        folder = Path(name)
        current = execute(prefix + CASES, folder, 'current')
        assert current.returncode == 0, current.stdout + current.stderr
        print(current.stdout, end='')
        disabled = prefix.replace(
            'const bool psram_dma = largest < CAMERA_DMA_RESERVE_BYTES && ESP.getPsramSize() != 0;',
            'const bool psram_dma = false;')
        assert disabled != prefix
        negative = execute(disabled + CASES, folder, 'old-allocation-path')
        assert negative.returncode != 0 and 'measured persistent13812-byte hole' in negative.stderr
        print('PASS negative control rejects repeated internal-DMA allocation')
        retained = prefix.replace('#define HALO_CAMERA_KEEP_INIT 0', '#define HALO_CAMERA_KEEP_INIT 1')
        retained += r'''
int main(){
 reset();fragment_without_wifi=true;connected_largest=13812;
 check(init_camera(),"first retained fallback succeeds");
 auto allocations=init_count, changes=mode_sets;connected_largest=40000;
 check(init_camera(),"retained camera reused");
 check(init_count==allocations&&mode_sets==changes&&psram_mode,"live reuse never changes DMA mode or driver");
 deinit_camera();check(!camera_allocated&&!grab_worker&&!g_camera_radio_off_owned.load(),"retained cleanup complete");
 printf("PASS %u retained-camera fallback assertions\n",checks);
}
'''
        reuse = execute(retained, folder, 'reuse')
        assert reuse.returncode == 0, reuse.stdout + reuse.stderr
        print(reuse.stdout, end='')


if __name__ == '__main__':
    main()
