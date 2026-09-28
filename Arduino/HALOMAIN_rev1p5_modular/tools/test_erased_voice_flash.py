#!/usr/bin/env python3
"""Run the actual persistence startup with real retry policy and raw-read faults.

The SPIFFS/partition/NVS boundaries are host doubles. No hardware, network,
formatting or flash writes are performed. --negative-source-root demonstrates
that the prior runtime leaves an erased VoiceFlash store pending.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_voice_persistence import harness


CASES = r'''
static void prepare(unsigned pending=halo_media_retry::VoiceFlash,size_t bytes=196608){
 reset();mount_arguments.clear();mount_failure=true;g_upload_persist_ready=false;
 raw_partition_present=true;raw_partition={uint32_t(bytes),false};
 raw_partition_bytes.assign(bytes,255);raw_read_fail_at=SIZE_MAX;
 raw_read_elapsed_ms=raw_read_calls=0;raw_bytes_read=0;
 retry_state={};retry_state.pending=pending;retry_state.backoff=3;retry_state.image_first=true;
 retry_durable_word=halo_media_retry::encode(retry_state);retry_inventory_calls=0;
}
static void retained(const char* why){
 const auto raw_before=raw_partition_bytes;const auto files_before=files;
 const auto word_before=retry_durable_word;const auto hint_before=halo_media_retry::encode(retry_state);
 const unsigned writes_before=writes,removes_before=removes,calls_before=retry_inventory_calls;
 upload_persist_setup();
 check(!g_upload_persist_ready&&retry_inventory_calls==calls_before,why);
 check(raw_partition_bytes==raw_before&&files==files_before&&writes==writes_before&&removes==removes_before,
       "uncertain partition never writes, formats, or deletes bytes");
 check(retry_durable_word==word_before&&halo_media_retry::encode(retry_state)==hint_before,
       "uncertain partition preserves every pending bit and backoff");
 check(mount_arguments.size()==1&&!mount_arguments[0],"mount failure never enables formatting");
}
int main(){
 prepare();const auto erased=raw_partition_bytes;
 upload_persist_setup();
 check(retry_state.pending==0,"complete erased partition clears stale VoiceFlash hint");
 check(raw_bytes_read==196608&&raw_read_calls==768,"every byte of actual 192KiB partition read once");
 check(!g_upload_persist_ready&&files.empty()&&writes==0&&removes==0&&raw_partition_bytes==erased,
       "empty proof neither formats nor enables unavailable flash fallback");
 check(halo_media_retry::interval(retry_state,false)==0,"empty flash alone no longer schedules media retries");
 check(retry_inventory_calls==1&&mount_arguments.size()==1&&!mount_arguments[0],"one authoritative empty inventory");
 halo_media_retry::State rebooted;
 check(halo_media_retry::decode(retry_durable_word,rebooted)&&!rebooted.pending,
       "successful hint clear survives reboot");

 // All other stores and retry priorities survive; only a sole empty store ends retries.
 for(unsigned pending=0;pending<8;++pending){
  prepare(pending);upload_persist_setup();
  check(retry_state.pending==(pending&~unsigned(halo_media_retry::VoiceFlash)),"clear only VoiceFlash bit");
  check(retry_state.image_first,"other store fairness priority retained");
  if(retry_state.pending)check(retry_state.backoff==3&&halo_media_retry::interval(retry_state,false)==21600,
                            "other pending stores retain existing backoff and wake eligibility");
  check(halo_media_retry::decode(retry_durable_word,rebooted)&&rebooted.pending==retry_state.pending,
        "other pending stores survive reboot");
 }
 // Probe first/middle/last bytes, including byte/chunk boundaries. Any content wins over blank.
 for(size_t offset:{size_t(0),size_t(255),size_t(256),size_t(98304),size_t(196607)}){
  prepare(7);raw_partition_bytes[offset]=0;retained("any nonblank byte retains unknown store");
 }
 for(size_t offset:{size_t(0),size_t(98304),size_t(196607)}){
  prepare(7);raw_read_fail_at=offset;retained("read error at any position cannot prove empty");
 }
 prepare(7);raw_partition_present=false;retained("missing partition remains unknown");
 check(!raw_read_calls,"missing partition has no read");
 prepare(7,0);retained("zero length is not an empty proof");
 prepare(7,1048577);retained("oversized partition exceeds fixed probe bound");
 check(!raw_read_calls,"oversized partition is not scanned");
 prepare(7);raw_partition.encrypted=true;retained("encrypted partition is not an erased raw-store proof");
 prepare(7);raw_read_elapsed_ms=100;retained("deadline failure preserves pending state");
 check(raw_read_calls<=5&&now_ms<=1501,"probe has bounded work and does not renew deadline");
 prepare(7,511);upload_persist_setup();
 check(raw_read_calls==2&&raw_bytes_read==511&&retry_state.pending==3,"final partial chunk fully checked");
 prepare(7,511);raw_partition_bytes.back()=1;retained("nonblank final partial chunk retained");
 prepare(7,256);raw_read_elapsed_ms=500;retained("even final successful read exceeding deadline cannot clear");

 // A failed durable hint write can repeat discovery after reboot, never erase media.
 prepare();retry_write_ok=false;const auto prior_word=retry_durable_word;
 upload_persist_setup();check(!retry_state.pending&&retry_durable_word==prior_word,"failed NVS write keeps old durable hint");
 retry_state={};check(halo_media_retry::decode(retry_durable_word,retry_state)&&retry_state.pending==4,
                      "reboot after failed write conservatively rediscovers flash");
 retry_write_ok=true;upload_persist_setup();
 check(halo_media_retry::decode(retry_durable_word,rebooted)&&!rebooted.pending,"later full proof repairs durable hint");

 // Mount success remains authoritative and bypasses raw inspection. Real save/load/replay still works.
 prepare();mount_failure=false;upload_persist_setup();
 check(g_upload_persist_ready&&!raw_read_calls&&!retry_state.pending,"healthy empty mount avoids raw scan");
 UploadJob job=fixture();check(upload_persist_save(job,1),"new recording can use later healthy flash mount");
 check(retry_state.pending==halo_media_retry::VoiceFlash,"real durable save restores pending hint");
 const auto saved_files=files;g_upload_persist_ready=false;upload_persist_setup();
 check(retry_state.pending==4&&files==saved_files&&!raw_read_calls,"mounted recording retained without raw scan");
 replay_queue_accept=true;now_ms=10000;upload_persist_maybe_replay();
 check(replay_queue_calls==1&&files==saved_files,"healthy recording replay remains eligible without premature deletion");
 check(retry_state.pending==4,"queue admission alone never clears durable recording hint");
 printf("PASS %u actual-startup erased flash / retry policy / fault / reboot checks\n",checks);
}
'''


def run(root, directory):
    source = directory / 'test.cpp'
    binary = directory / 'test'
    source.write_text(harness(root).split('int main(){', 1)[0] + CASES)
    subprocess.run([shutil.which('clang++') or 'c++', '-std=c++17', '-pthread', '-O1',
                    '-fsanitize=address,undefined', '-I', str(root), str(source), '-o', str(binary)],
                   check=True, timeout=30)
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--negative-source-root', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='halo-erased-voice-') as temp:
        root = Path(temp)
        if args.negative_source_root:
            negative = run(args.negative_source_root, root)
            assert negative.returncode != 0 and 'complete erased partition clears stale VoiceFlash hint' in negative.stderr
            print('PASS negative control: prior actual header reproduces stale erased-store hint')
        result = run(args.source_root, root)
        print(result.stdout, end='')
        if result.returncode:
            print(result.stderr, end='')
            raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()
