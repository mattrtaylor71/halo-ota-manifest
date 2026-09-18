#!/usr/bin/env python3
"""Compose actual voice cancellation, parked custody, offline gate and replay.

One captured item crosses several worker attempts without resetting its identity.
HTTP/receipt/cancellation/worker decisions are production functions; the existing
SDK and storage boundary doubles record custody. No hardware or network access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import signal
import subprocess

import test_fresh_upload_user_priority as fresh
from test_voice_persistence import definition

ROOT = Path(__file__).resolve().parents[1]


def harness(root, omit_parking=False):
    code = fresh.harness(root).split('int main(){', 1)[0]
    replace = fresh.corners.replace_once
    code = replace(code, 'static unsigned persisted=0,delivered=0,deleted=0;',
                   'static unsigned persisted=0,delivered=0,deleted=0;\n'
                   'static bool durable_present=false,delete_ack=true;\n'
                   'static UploadJob delete_request={};')
    code = replace(code,
        'static bool sense_voice_spool_delete(const UploadJob&){++deleted;return true;}',
        'static bool sense_voice_spool_delete(const UploadJob& job){\n'
        ' assert(durable_present);++deleted;delete_request=job;\n'
        ' if(delete_ack)durable_present=false;return delete_ack;\n}')
    code = replace(code, 'assert(!strcmp(reason,"voice_post_fail")||',
                   'assert(!strcmp(reason,"network_or_clock_pending")||!strcmp(reason,"voice_post_fail")||')
    main = (root / 'Sense_Minimal/Sense_Minimal.ino').read_text()
    worker = definition(main, 'static void upload_worker_task(')
    paused = definition(worker,
        '      if ((job.from_voice_sd || job.from_image_sd || job.from_persisted) &&')
    offline = definition(worker,
        '      if (!wifi_is_connected() || !sense_time_has_fresh_sync()) {')
    # This optional negative control changes generated host code only. It proves
    # the composed test observes the production parking call, not just the mock.
    if omit_parking:
        code = replace(code, 'if (park_cancelled_fresh("voice_cancelled")) continue;',
                       'if (false) continue; // negative control: omitted parking')
    code += '\n#define free worker_free\nstatic bool actual_worker_preflight(const UploadJob& job){\n'
    code += ' upload_inflight=true;do{\n' + paused + '\n' + offline
    code += '\n return true;\n }while(false);return false;\n}\n#undef free\n'
    return code + r'''
static void same_capture(const UploadJob& a,const UploadJob& b){
 assert(a.job_id==b.job_id&&a.image_len==b.image_len&&a.is_voice==b.is_voice);
 assert(!memcmp(&a.voice,&b.voice,sizeof(a.voice)));
 assert(!memcmp(a.image_buf,b.image_buf,a.image_len));
}
int main(){
 unsigned scenarios=0;
 for(const char* cancel_at:{"post_status","reply_body","ack_hash_done"})
 for(bool clock_missing:{false,true}){
  fresh_reset(std::vector<int>(16,202));durable_present=false;delete_ack=true;delete_request={};
  const auto captured=fixture();const auto valid_reply=reply_body;
  input_at(cancel_at);
  {MediaRetryNetworkScope attempt(captured);
   const bool accepted=voice_upload_and_parse(captured);assert(!accepted);
   actual_worker_voice_dispatch(captured,accepted);
  }
  released();
  assert(upload_worker_parked_pending()&&persisted==0&&worker_freed==0&&deleted==0);
  // More input while the item is parked cannot replace or free its descriptor.
  sense_note_admitted_user_action();sense_note_admitted_user_action();
  UploadJob resumed{};assert(upload_worker_take_parked_job(resumed));
  same_capture(captured,resumed);assert(!upload_worker_take_parked_job(resumed));
  boundary_hook={};connected=clock_missing;age_ok=!clock_missing;
  const auto posts_before_offline=posts;
  {MediaRetryNetworkScope attempt(resumed);
   assert(!media_retry_network_cancelled()); // old gesture is not a new cancellation
   assert(!actual_worker_preflight(resumed));
  }
  released();assert(posts==posts_before_offline&&persisted==1&&worker_freed==1&&!upload_inflight);
  same_capture(captured,retained);assert(retained_bytes.size()==captured.image_len);
  // Storage commit/readback is explicitly a boundary double. Replay gets a
  // separate byte copy from that retained envelope, never the freed RAM owner.
  durable_present=true;std::vector<uint8_t> disk=retained_bytes;
  auto replay=retained;replay.image_buf=disk.data();replay.from_voice_sd=true;
  connected=age_ok=true;g_media_retry_user_paused=true;
  const auto posts_before_pause=posts;
  {MediaRetryNetworkScope attempt(replay);assert(!actual_worker_preflight(replay));}
  released();assert(durable_present&&posts==posts_before_pause&&deleted==0&&persisted==1);
  // A later wake permits retry. A positive HTTP code for the wrong request
  // cannot remove this committed item, even after the original uncertain POST.
  g_media_retry_user_paused=false;
  reply_body=R"({"accepted":true,"async":true,"duplicate":true,"status":"completed","jobId":"wrong-request"})";
  {MediaRetryNetworkScope attempt(replay);
   assert(actual_worker_preflight(replay));
   const bool accepted=voice_upload_and_parse(replay);assert(!accepted);
   actual_worker_voice_dispatch(replay,accepted);
  }
  released();assert(durable_present&&deleted==0&&delivered==0);
  same_capture(captured,retained);
  // Correct duplicate acceptance closes network custody, but a lost LCD
  // delete acknowledgment leaves the same record available on another wake.
  reply_body=valid_reply;
  const auto pos=reply_body.find("\"duplicate\":false");assert(pos!=std::string::npos);
  reply_body.replace(pos,17,"\"duplicate\":true,\"status\":\"completed\"");delete_ack=false;
  const auto persists_before_accept=persisted;
  for(unsigned attempt_number=0;attempt_number<2;++attempt_number){
   g_media_retry_user_paused=false;
   {MediaRetryNetworkScope attempt(replay);
    assert(actual_worker_preflight(replay));
    const bool accepted=voice_upload_and_parse(replay);assert(accepted);
    // Input after a validated receipt must not park the accepted saved item.
    sense_note_admitted_user_action();g_media_retry_user_paused=true;
    actual_worker_voice_dispatch(replay,accepted);
   }
   released();same_capture(captured,delete_request);
   assert(deleted==attempt_number+1&&delivered==attempt_number+1);
   assert(persisted==persists_before_accept&&!upload_worker_parked_pending()&&!upload_inflight);
   assert(durable_present==(attempt_number==0));delete_ack=true;
  }
  for(const auto& headers:sent_headers){
   assert(headers.at("x-request-id")==captured.voice.request_id);
   assert(headers.at("x-session-id")==captured.voice.session_id);
  }
  ++scenarios;
 }
 printf("PASS %u actual-function workflow assertions across %u composed custody scenarios\n",checks,scenarios);
}
'''


def execute(root, out, negative=False):
    out.mkdir(parents=True, exist_ok=False)
    cpp, binary = out / 'test.cpp', out / 'test'
    cpp.write_text(harness(root, omit_parking=negative))
    command = [shutil.which('clang++') or 'c++', '-std=c++17', '-pthread',
               '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', '-I', str(root), '-I',
               str(root / 'halo_ota_demo/firmware/halo_sense_prod/libraries/ArduinoJson/src'),
               str(cpp), '-o', str(binary)]
    build = subprocess.run(command, capture_output=True, timeout=30,
                           preexec_fn=fresh.corners.network.no_core)
    (out / 'compile.log').write_bytes(build.stdout + build.stderr)
    if build.returncode:
        return {'compiled': False, 'exit_code': build.returncode}
    run = subprocess.run([str(binary)], capture_output=True, timeout=15,
                         preexec_fn=fresh.corners.network.no_core)
    (out / 'run.log').write_bytes(run.stdout + run.stderr)
    return {'compiled': True, 'exit_code': run.returncode,
            'output': (run.stdout + run.stderr).decode()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=ROOT)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--negative-control', action='store_true')
    a = p.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    result = {'current': execute(a.source_root, a.out / 'current'),
              'hardware': False, 'storage_and_sdk_boundaries': 'doubled'}
    if a.negative_control:
        result['negative_omitted_parking'] = execute(a.source_root, a.out / 'negative', True)
    result['pass'] = result['current']['compiled'] and result['current']['exit_code'] == 0
    if a.negative_control:
        negative = result['negative_omitted_parking']
        negative['expected_custody_assertion'] = bool(
            negative['compiled'] and negative['exit_code'] == -signal.SIGABRT and
            re.fullmatch(
                r'FAIL line \d+: upload_worker_parked_pending\(\)&&persisted==0'
                r'&&worker_freed==0&&deleted==0\n', negative.get('output', '')))
        # A compiler failure, unrelated abort, or sanitizer finding is not
        # evidence that this mutation reached the intended custody assertion.
        result['pass'] &= negative['expected_custody_assertion']
    paths = ['Sense_Minimal/Sense_Minimal.ino', 'Sense_Minimal/sense_ops.h',
             'Sense_Minimal/sense_media_retry.h', 'Sense_Minimal/sense_media_retry_client.h',
             'Sense_Minimal/sense_user_activity.h', 'Sense_Minimal/sense_voice.h',
             'Sense_Minimal/sense_upload.h', 'Sense_Minimal/sense_op_queue.h',
             'tools/test_user_workflow_transitions.py', 'tools/test_fresh_upload_user_priority.py',
             'tools/test_media_network_corner_cases.py', 'tools/test_media_network_retry.py']
    result['source_sha256'] = {s: hashlib.sha256((a.source_root / s).read_bytes()).hexdigest()
                             for s in paths}
    (a.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
