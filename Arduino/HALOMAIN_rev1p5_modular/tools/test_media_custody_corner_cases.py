#!/usr/bin/env python3
"""Fault every actual Store budget checkpoint and inspect receipt cleanup.

Extends the syscall stress harness with a persistent exhausted-budget boundary.
Both real production Store headers execute unchanged under ASan/UBSan. These
are finite host filesystem tests, not SD/FAT power-loss or backend acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import zlib

from stress_media_storage import HARNESS, pin, snapshot


def harness():
    code = HARNESS.replace(
        'static bool armed=false,hit=false;',
        '''static bool armed=false,hit=false;
static unsigned budget_calls=0,budget_abort_at=0,target_request=100;
static bool budget_hit=false;
static bool remaining_budget(){
  ++budget_calls;
  if(budget_abort_at && budget_calls>=budget_abort_at){budget_hit=true;return false;}
  return true;
}''')
    code = code.replace('Store s(root.c_str());Meta m=', '''
 const char* request=std::getenv("HALO_TEST_REQUEST_ID");
 if(request)target_request=(unsigned)std::strtoul(request,nullptr,10);
 const char* capacity=std::getenv("HALO_TEST_SLOT_CAPACITY");
 Store s(root.c_str(),capacity?(unsigned)std::strtoul(capacity,nullptr,10):40);
 const char* limit=std::getenv("HALO_TEST_BUDGET_ABORT_AT");
 budget_abort_at=limit?(unsigned)std::strtoul(limit,nullptr,10):0;
 s.set_budget(remaining_budget);Meta m=''').replace('meta(100)', 'meta(target_request)')
    code = code.replace('}else if(op=="wrong-erase")', '''}
  else if(op=="wrong-device-erase")r=s.erase(m.request_id,m.owner_id,"other-device",m.len,m.crc32);
  else if(op=="wrong-crc-erase")r=s.erase(m.request_id,m.owner_id,m.device_id,m.len,m.crc32^1);
  else if(op=="wrong-len-erase")r=s.erase(m.request_id,m.owner_id,m.device_id,m.len+2,m.crc32);
  else if(op=="invalid-request-erase")r=s.erase("../escape",m.owner_id,m.device_id,m.len,m.crc32);
  else if(op=="wrong-erase")''')
    code = code.replace('::close(tracefd);tracefd=-1;armed=false;', '''
 std::fprintf(stderr,"{\\"budget_calls\\":%u,\\"budget_hit\\":%s}\\n",budget_calls,budget_hit?"true":"false");
 ::close(tracefd);tracefd=-1;armed=false;''')
    code = code.replace('Result listed=reboot.list(m.owner_id,m.device_id,&first,&stats);',
                        'Result listed=Result::Invalid;(void)first;')
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--expect-pre-marker-regression', action='store_true',
                        help='Negative control: require old cleanup/capacity failures and no other failures')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    compiler = shutil.which('clang++') or shutil.which('c++')
    if not compiler:
        raise SystemExit('Host C++ compiler required')
    pins = [pin(args.source_root / 'halo_common' / name)
            for name in ('VoiceSpoolStore.h', 'ImageSpoolStore.h')]
    source = args.out / 'harness.cpp'
    source.write_text(harness())
    cases, observations = [], []
    started = time.time()
    result = {'status': 'RUNNING', 'runtime_sources': pins, 'script': pin(Path(__file__)),
              'harness': pin(source), 'started_epoch': started, 'cases': cases,
              'observations': observations, 'hardware_executed': False,
              'runtime_modified': False}

    def record(media, category, name, passed, **detail):
        case = dict(media=media, category=category, name=name, passed=bool(passed), **detail)
        cases.append(case)
        if not passed:
            print('FAIL', json.dumps(case), flush=True)

    try:
        with tempfile.TemporaryDirectory(prefix='halo-custody-corners-') as td:
            base = Path(td)
            payload = base / 'payload'
            # Multiple payload CRC read checkpoints, plus a partial final block.
            payload.write_bytes(bytes((i * 31 + i // 101) & 255 for i in range(4098)))
            digest = hashlib.sha256(payload.read_bytes()).hexdigest()
            trace = base / 'trace.tsv'
            for media in ('voice', 'image'):
                binary = args.out / ('test-' + media)
                command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                           '-Wno-unused-function', '-Wno-misleading-indentation',
                           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                           '-I', str(args.source_root / 'halo_common'), str(source), '-o', str(binary)]
                if media == 'image':
                    command.insert(1, '-DIMAGE')
                compiled = subprocess.run(command, text=True, capture_output=True, timeout=60)
                (args.out / (media + '-compile.log')).write_text(compiled.stdout + compiled.stderr)
                if compiled.returncode:
                    raise RuntimeError(media + ' compilation failed')
                namespace = media + '-spool-v1'
                suffix = '.pcm' if media == 'voice' else '.jpg'
                stem = f'{100:032x}'

                def run(root, operation, abort_at=0, fault=-1, mode='none', count=2,
                        request_id=100, capacity=40):
                    environment = dict(os.environ, HALO_TEST_BUDGET_ABORT_AT=str(abort_at),
                                       HALO_TEST_REQUEST_ID=str(request_id),
                                       HALO_TEST_SLOT_CAPACITY=str(capacity))
                    argv = [str(binary), str(root / namespace), operation, str(payload), digest,
                            str(fault), mode, str(trace), str(count)]
                    child = subprocess.run(argv, env=environment, text=True, capture_output=True, timeout=20)
                    if child.returncode in (80, 81):
                        response = {'process_exit': child.returncode}
                    elif child.returncode:
                        raise RuntimeError(repr((argv, child.returncode, child.stdout, child.stderr)))
                    else:
                        response = json.loads(child.stdout)
                        response.update(json.loads(child.stderr))
                    catalogue = [dict(zip(('id', 'op', 'path'), line.split('\t', 2)))
                                 for line in trace.read_text().splitlines()]
                    return response, catalogue

                seeded = base / ('seeded-' + media)
                seeded.mkdir()
                run(seeded, 'seed')
                original = snapshot(seeded)
                committed = base / ('committed-' + media)
                shutil.copytree(seeded, committed)
                saved, _ = run(committed, 'save')
                assert saved['result'] == 'ok' and saved['target_exact']
                trial = base / 'trial'

                def copy_trial(from_root):
                    if trial.exists():
                        shutil.rmtree(trial)
                    shutil.copytree(from_root, trial)

                def reject_wrong_receipts(label):
                    before = snapshot(trial)
                    for operation in ('wrong-erase', 'wrong-device-erase', 'wrong-crc-erase',
                                      'wrong-len-erase', 'invalid-request-erase'):
                        rejected, _ = run(trial, operation)
                        record(media, 'receipt_binding', label + ':' + operation,
                               rejected['result'] not in ('ok', 'already_stored')
                               and snapshot(trial) == before, returned=rejected)

                copy_trial(committed)
                reject_wrong_receipts('live-committed')
                # Capture was never committed: erase cannot discard its only
                # staged payload, even with an otherwise matching identity.
                copy_trial(seeded)
                run(trial, 'begin')
                staged = snapshot(trial)
                rejected, _ = run(trial, 'erase')
                record(media, 'unaccepted_partial', 'matching-receipt-cannot-delete-stage',
                       rejected['result'] != 'ok' and snapshot(trial) == staged)

                for operation in ('save', 'attempt', 'erase', 'verify', 'list', 'inventory'):
                    initial = seeded if operation == 'save' else committed
                    before = snapshot(initial)
                    copy_trial(initial)
                    complete, _ = run(trial, operation)
                    for boundary in range(1, complete['budget_calls'] + 1):
                        copy_trial(initial)
                        interrupted, _ = run(trial, operation, abort_at=boundary)
                        after = snapshot(trial)
                        reopened, _ = run(trial, 'verify')
                        safe = (interrupted['budget_hit'] and interrupted['result'] == 'io'
                                and reopened['seed_preserved']
                                and all(after.get(p) == value for p, value in original.items()))
                        if operation in ('verify', 'list', 'inventory'):
                            safe &= after == before
                        if operation == 'attempt':
                            safe &= all(after.get(namespace + '/' + stem + ending)
                                        == before[namespace + '/' + stem + ending]
                                        for ending in ('.meta', suffix))
                        if operation in ('save', 'attempt'):
                            resumed, _ = run(trial, operation)
                            safe &= resumed['result'] in ('ok', 'already_stored')
                            safe &= resumed['lookup'] == 'ok'
                            if operation == 'save':
                                safe &= resumed['target_exact']
                            else:
                                safe &= resumed['epoch'] == 1789501234
                        record(media, 'budget_checkpoint', operation + ':' + str(boundary), safe,
                               interrupted=interrupted, reopened=reopened)
                        if not safe:
                            shutil.copytree(trial, args.out / (media + '-' + operation + '-' + str(boundary)))

                # A successful delete whose UART acknowledgement was lost must
                # not cause later duplicate receipts to recreate media or touch
                # other requests. NotFound is the existing API result.
                copy_trial(committed)
                erased, erase_catalogue = run(trial, 'erase')
                assert erased['result'] == 'ok'
                after_delete = snapshot(trial)
                for repeat in range(1, 9):
                    duplicate, _ = run(trial, 'erase')
                    record(media, 'duplicate_delete', str(repeat),
                           duplicate['result'] == 'not_found' and duplicate['seed_preserved']
                           and snapshot(trial) == after_delete, returned=duplicate)

                # Inject one failure at each actual remove after cloud custody.
                # Verify no re-uploadable partial item, then inspect whether an
                # exact repeated receipt can complete cleanup after I/O recovers.
                for event in erase_catalogue:
                    if event['op'] != 'remove':
                        continue
                    copy_trial(committed)
                    failed, _ = run(trial, 'erase', fault=int(event['id']), mode='error')
                    leftover = snapshot(trial)
                    reject_wrong_receipts('delete-fault-' + event['id'])
                    repeated, _ = run(trial, 'list')
                    after_repeat = snapshot(trial)
                    own_files = [p for p in after_repeat if p.startswith(namespace + '/' + stem)]
                    record(media, 'delete_cleanup_custody', event['id'],
                           failed['result'] == 'io' and repeated['seed_preserved']
                           and all(after_repeat.get(p) == value for p, value in original.items())
                           and not own_files,
                           fault=event, returned=failed, duplicate=repeated,
                           retained_files=own_files)
                    if own_files:
                        observations.append({'media': media, 'kind': 'delete_cleanup_residue',
                                             'fault': event, 'retry_result': repeated['result'],
                                             'retained_files': own_files,
                           'unchanged_after_retry': after_repeat == leftover,
                                             'reuploadable': repeated['lookup'] == 'ok'})
                    # With the two unrelated seeds plus this item, the old
                    # three-slot queue stayed full. A completed cleanup must
                    # actually admit the next capture without evicting seeds.
                    next_save, _ = run(trial, 'save', request_id=101, capacity=3)
                    record(media, 'capacity_reclaimed', 'delete-fault-' + event['id'],
                           next_save['result'] == 'ok' and next_save['seed_preserved'],
                           returned=next_save)

                # Also exercise each removal on an item with a committed first
                # attempt marker, since the ordinary delete has absent markers.
                marked = base / ('marked-' + media)
                shutil.copytree(committed, marked)
                run(marked, 'attempt')
                copy_trial(marked)
                _, catalogue = run(trial, 'erase')
                for event in catalogue:
                    if event['op'] != 'remove':
                        continue
                    copy_trial(marked)
                    failed, _ = run(trial, 'erase', fault=int(event['id']), mode='error')
                    reject_wrong_receipts('attempted-delete-fault-' + event['id'])
                    repeated, _ = run(trial, 'list')
                    after_repeat = snapshot(trial)
                    own_files = [p for p in after_repeat if p.startswith(namespace + '/' + stem)]
                    record(media, 'attempted_delete_cleanup', event['id'],
                           failed['result'] == 'io' and repeated['seed_preserved']
                           and all(after_repeat.get(p) == value for p, value in original.items())
                           and not own_files,
                           fault=event, duplicate=repeated, retained_files=own_files)
                    if own_files:
                        observations.append({'media': media, 'kind': 'attempted_delete_cleanup_residue',
                                             'fault': event, 'retry_result': repeated['result'],
                                             'retained_files': own_files,
                                             'reuploadable': repeated['lookup'] == 'ok'})

                # Every observed accepted-delete commit/cleanup syscall can fail
                # or lose its process. Reopen/list receives no cloud receipt.
                # Exact committed marker bytes are the independent authority
                # oracle; a .part or corrupt marker cannot authorize deletion.
                if args.expect_pre_marker_regression:
                    print(media, 'completed negative control', len(cases), 'cumulative cases', flush=True)
                    continue
                for attempted, initial in ((False, committed), (True, marked)):
                    original_files = snapshot(initial)
                    expected_marker = (initial / namespace / (stem + ('.attempt' if attempted else '.meta'))).read_bytes()
                    copy_trial(initial)
                    _, catalogue = run(trial, 'erase')
                    (args.out / (media + ('-attempted' if attempted else '') + '-erase-catalogue.json')).write_text(json.dumps(catalogue, indent=2) + '\n')
                    for event in catalogue:
                        modes = ['error', 'crash_before', 'crash_after']
                        if event['op'] in ('fwrite', 'fread'):
                            modes.append('short')
                        if event['op'] == 'fwrite':
                            modes.append('corrupt')
                        for mode in modes:
                            copy_trial(initial)
                            stopped, _ = run(trial, 'erase', fault=int(event['id']), mode=mode)
                            marker = trial / namespace / (stem + '.delete')
                            authority = marker.exists() and marker.read_bytes() == expected_marker
                            marker_present = marker.exists()
                            before_recovery = snapshot(trial)
                            fully_removed = not any(p.startswith(namespace + '/' + stem) for p in before_recovery)
                            recovered, _ = run(trial, 'list')
                            after_recovery = snapshot(trial)
                            retained = all(after_recovery.get(p) == value for p, value in original.items())
                            own_files = [p for p in after_recovery if p.startswith(namespace + '/' + stem)]
                            if authority or fully_removed:
                                safe = retained and not own_files
                            else:
                                # No committed authority means every original
                                # payload/metadata byte remains untouched.
                                safe = retained and all(after_recovery.get(p) == value
                                                        for p, value in original_files.items())
                                safe &= after_recovery == before_recovery
                            record(media, 'delete_syscall_recovery',
                                   ('attempted:' if attempted else 'unattempted:') + event['id'] + ':' + mode,
                                   safe, fault=event, stopped=stopped,
                                   marker_authority=authority, marker_present=marker_present,
                                   fully_removed=fully_removed, recovered=recovered,
                                   retained_files=own_files)
                            if not safe:
                                shutil.copytree(trial, args.out / (media + '-delete-failure-' + str(len(cases))))

                # Capture a real committed/read-back marker before any payload
                # removal, then challenge the automatic recovery trust boundary.
                copy_trial(marked)
                _, catalogue = run(trial, 'erase')
                first_remove = next(event for event in catalogue if event['op'] == 'remove')
                pending = base / ('pending-' + media)
                shutil.copytree(marked, pending)
                failed, _ = run(pending, 'erase', fault=int(first_remove['id']), mode='error')
                assert failed['result'] == 'io'
                accepted = pending / namespace / (stem + '.delete')
                assert accepted.read_bytes() == (marked / namespace / (stem + '.attempt')).read_bytes()

                def changed_owner(raw):
                    raw = raw.replace(b'stress-owner', b'foreignowner')
                    return raw[:-4] + zlib.crc32(raw[:-4]).to_bytes(4, 'little')

                for mutation in ('wrong_owner', 'wrong_request_filename', 'corrupt_marker',
                                 'stage_only', 'conflicting_payload', 'conflicting_part',
                                 'conflicting_meta_stage', 'conflicting_attempt_stage',
                                 'conflicting_delete_stage'):
                    copy_trial(pending)
                    marker = trial / namespace / (stem + '.delete')
                    if mutation == 'wrong_owner':
                        marker.write_bytes(changed_owner(marker.read_bytes()))
                    elif mutation == 'wrong_request_filename':
                        marker.rename(marker.with_name(f'{101:032x}' + '.delete'))
                    elif mutation == 'corrupt_marker':
                        raw = bytearray(marker.read_bytes()); raw[len(raw) // 2] ^= 1
                        marker.write_bytes(raw)
                    elif mutation == 'stage_only':
                        marker.rename(marker.with_name(stem + '.delete.part'))
                    elif mutation in ('conflicting_payload', 'conflicting_part'):
                        target = trial / namespace / (stem + (suffix if mutation == 'conflicting_payload' else '.part'))
                        raw = bytearray(payload.read_bytes()); raw[0] ^= 1; target.write_bytes(raw)
                    else:
                        extension = {'conflicting_meta_stage': '.meta.part',
                                     'conflicting_attempt_stage': '.attempt.part',
                                     'conflicting_delete_stage': '.delete.part'}[mutation]
                        (trial / namespace / (stem + extension)).write_bytes(changed_owner(marker.read_bytes()))
                    before = snapshot(trial)
                    read, _ = run(trial, 'list')
                    record(media, 'untrusted_delete_marker', mutation,
                           snapshot(trial) == before and read['seed_preserved'], returned=read)
                    if mutation == 'stage_only':
                        record(media, 'staged_marker_no_authority', 'original-remains-replayable',
                               read['lookup'] == 'ok')

                # A different incoming capture recovers capacity without a
                # repeated receipt. The same request cannot be resurrected in
                # the call that retires its accepted old record.
                copy_trial(pending)
                new_capture, _ = run(trial, 'save', request_id=101, capacity=3)
                record(media, 'admission_recovers_capacity', 'new-request',
                       new_capture['result'] == 'ok' and new_capture['seed_preserved']
                       and not any(p.startswith(namespace + '/' + stem) for p in snapshot(trial)))
                copy_trial(pending)
                repeated_capture, _ = run(trial, 'save')
                record(media, 'admission_same_request', 'never-resurrect',
                       repeated_capture['result'] == 'conflict' and repeated_capture['seed_preserved']
                       and not any(p.startswith(namespace + '/' + stem) for p in snapshot(trial)))
                print(media, 'completed', len(cases), 'cumulative cases', flush=True)
        failed = [c for c in cases if not c['passed']]
        if args.expect_pre_marker_regression:
            allowed = {'delete_cleanup_custody', 'attempted_delete_cleanup', 'capacity_reclaimed'}
            reproduced = {c['media'] for c in failed if c['category'] == 'capacity_reclaimed'} == {'voice', 'image'}
            result['status'] = ('PASS_EXPECTED_PRE_MARKER_REGRESSION' if reproduced
                                and all(c['category'] in allowed for c in failed) else 'FAIL_NEGATIVE_CONTROL')
        else:
            result['status'] = 'PASS' if not failed else 'FAIL'
    except Exception as exc:
        result.update(status='FAIL_OR_SETUP_ERROR', error=repr(exc))
        raise
    finally:
        result['runtime_sources_after'] = [pin(Path(item['path'])) for item in pins]
        if result['runtime_sources_after'] != pins:
            result.update(status='FAIL_SOURCE_CHANGED')
        result.update(finished_epoch=time.time(), case_count=len(cases),
                      failure_count=sum(not c['passed'] for c in cases),
                      limits=['Budget callback interruption is a host boundary; no physical user-input latency is measured.',
                              'Guardian reset/power interruption is not simulated by budget exhaustion.',
                              'Delete is invoked with a presumed validated cloud receipt; no backend/network operation occurs.',
                              'Legacy or corrupt remnants without a committed accepted-delete marker remain held.',
                              'Host POSIX filesystem plus ASan/UBSan, not SD/FAT electrical durability.'])
        (args.out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'status': result['status'], 'cases': len(cases),
                      'observations': len(observations), 'result': pin(args.out / 'RESULT.json')}))
    raise SystemExit(0 if result['status'] in ('PASS', 'PASS_EXPECTED_PRE_MARKER_REGRESSION') else 1)


if __name__ == '__main__':
    main()
