#!/usr/bin/env python3
"""Deterministic fault stress of the actual production media Store headers.

Host filesystem/process fault model, not physical FAT/SD power-loss qualification.
Only this new test and external evidence are written; no firmware or live SD I/O.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
import time

HARNESS = r'''
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
static bool armed=false,hit=false;
static int event_no=0,target=-1,tracefd=-1;
static std::string fault;
static std::map<FILE*,std::string> names;
static std::map<FILE*,bool> read_errors;
static int before(const char* op,const std::string& path="") {
  if(!armed)return 0;
  int n=++event_no;
  if(tracefd>=0){std::string s=std::to_string(n)+"\t"+op+"\t"+path+"\n";::write(tracefd,s.data(),s.size());}
  if(n==target){hit=true;if(fault=="crash_before")::_exit(80);return 1;}
  return 0;
}
static void after(int hit_here){if(hit_here&&fault=="crash_after")::_exit(81);}
static bool bad(int h){if(h&&fault=="error"){errno=EIO;return true;}return false;}
static FILE* fi_fopen(const char* p,const char* m){int h=before("fopen",std::string(p)+" "+m);if(bad(h))return nullptr;FILE* f=::fopen(p,m);if(f)names[f]=p;after(h);return f;}
static size_t fi_fwrite(const void* p,size_t s,size_t n,FILE* f){int h=before("fwrite",names[f]);if(bad(h)){errno=ENOSPC;return 0;}size_t r;if(h&&fault=="short")r=::fwrite(p,s,n/2,f);else if(h&&fault=="corrupt"){std::vector<unsigned char> b((const unsigned char*)p,(const unsigned char*)p+s*n);if(!b.empty())b[b.size()/2]^=1;r=::fwrite(b.data(),s,n,f);}else r=::fwrite(p,s,n,f);after(h);return r;}
static size_t fi_fread(void* p,size_t s,size_t n,FILE* f){int h=before("fread",names[f]);if(bad(h)){read_errors[f]=true;return 0;}bool short_io=h&&fault=="short";size_t r=::fread(p,s,short_io?n/2:n,f);if(short_io){read_errors[f]=true;errno=EIO;}after(h);return r;}
static int fi_ferror(FILE* f){return read_errors[f]?1: ::ferror(f);}
static int fi_fflush(FILE* f){int h=before("fflush",names[f]);if(bad(h))return EOF;int r=::fflush(f);after(h);return r;}
static int fi_fsync(int fd){int h=before("fsync",std::to_string(fd));if(bad(h))return -1;int r=::fsync(fd);after(h);return r;}
static int fi_fclose(FILE* f){int h=before("fclose",names[f]);names.erase(f);read_errors.erase(f);int r=::fclose(f);if(bad(h))return EOF;after(h);return r;}
static int fi_rename(const char* a,const char* b){int h=before("rename",std::string(a)+" -> "+b);if(bad(h))return -1;int r=::rename(a,b);after(h);return r;}
static int fi_remove(const char* p){int h=before("remove",p);if(bad(h))return -1;int r=::remove(p);after(h);return r;}
static int fi_mkdir(const char* p,mode_t m){int h=before("mkdir",p);if(bad(h))return -1;int r=::mkdir(p,m);after(h);return r;}
static int fi_stat(const char* p,struct stat* st){int h=before("stat",p);if(bad(h))return -1;int r=::stat(p,st);after(h);return r;}
static DIR* fi_opendir(const char* p){int h=before("opendir",p);if(bad(h))return nullptr;DIR* d=::opendir(p);after(h);return d;}
static struct dirent* fi_readdir(DIR* d){int h=before("readdir");if(bad(h))return nullptr;auto e=::readdir(d);after(h);return e;}
static int fi_closedir(DIR* d){int h=before("closedir");int r=::closedir(d);if(bad(h))return -1;after(h);return r;}
#define fopen fi_fopen
#define fwrite fi_fwrite
#define fread fi_fread
#define ferror fi_ferror
#define fflush fi_fflush
#define fsync fi_fsync
#define fclose fi_fclose
#define rename fi_rename
#define remove fi_remove
#define mkdir fi_mkdir
// Function-like macro leaves `struct stat` intact.
#define stat(p,s) fi_stat(p,s)
#define opendir fi_opendir
#define readdir fi_readdir
#define closedir fi_closedir
#ifdef IMAGE
#include "ImageSpoolStore.h"
namespace media=halo_image;
#else
#include "VoiceSpoolStore.h"
namespace media=halo_voice;
#endif
#undef fopen
#undef fwrite
#undef fread
#undef ferror
#undef fflush
#undef fsync
#undef fclose
#undef rename
#undef remove
#undef mkdir
#undef stat
#undef opendir
#undef readdir
#undef closedir
using media::Store;using media::Meta;using media::Stats;using media::Result;
namespace fs=std::filesystem;
static std::vector<uint8_t> bytes;
static std::string digest;
static uint32_t independent_crc(const uint8_t* p,size_t n){uint32_t c=~0u;for(size_t i=0;i<n;++i){c^=p[i];for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320u&-(c&1));}return c^~0u;}
static Meta meta(unsigned id){Meta m{};m.len=bytes.size();m.crc32=independent_crc(bytes.data(),bytes.size());m.job_id=id;m.epoch=id==100?0:1789500000;m.retries=1;std::strcpy(m.owner_id,"stress-owner");std::strcpy(m.device_id,"stress-device");std::snprintf(m.request_id,sizeof(m.request_id),"%032x",id);
#ifdef IMAGE
 std::strcpy(m.checksum_sha256,digest.c_str());std::strcpy(m.mode,"check-in");std::strcpy(m.expiry,"2026-10-01");m.quantity=2;m.add_to_shopping_list=1;m.camera.actual_width=640;m.camera.actual_height=480;m.camera.scene_luma=-7;m.camera.xclk_hz=20000000;
#else
 std::strcpy(m.session_id,"stress-frozen-session");
#endif
 return m;}
static Result save(Store& s,const Meta& m){Result r=s.begin(m);if(r!=Result::Ok)return r;uint16_t seq=0;for(size_t off=0;off<bytes.size();off+=512,++seq){r=s.append(seq,bytes.data()+off,std::min<size_t>(512,bytes.size()-off));if(r!=Result::Ok)return r;}return s.finish(seq);}
static bool lookup_same(Store& s,unsigned id){Meta m=meta(id),got;return s.lookup(m.request_id,m.owner_id,m.device_id,&got)==Result::Ok&&media::same(m,got);}
int main(int argc,char** argv){
 if(argc!=9)return 2;
 std::string root=argv[1],op=argv[2];std::ifstream in(argv[3],std::ios::binary);bytes=std::vector<uint8_t>(std::istreambuf_iterator<char>(in),{});digest=argv[4];target=std::atoi(argv[5]);fault=argv[6];tracefd=::open(argv[7],O_WRONLY|O_CREAT|O_TRUNC,0600);int count=std::atoi(argv[8]);
 if(bytes.empty()||tracefd<0)return 3;
 Store s(root.c_str());Meta m=meta(100),got{};Result r=Result::Invalid;
 if(op=="seed"){for(int i=1;i<=count;++i)if(save(s,meta(i))!=Result::Ok)return 4;r=Result::Ok;}
 else {
  armed=true;
  if(op=="save")r=save(s,m);
  else if(op=="begin")r=s.begin(m);
  else if(op=="attempt")r=s.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,1789501234,&got);
  else if(op=="erase")r=s.erase(m.request_id,m.owner_id,m.device_id,m.len,m.crc32);
  else if(op=="verify")r=s.lookup(m.request_id,m.owner_id,m.device_id,&got);
  else if(op=="list"){Stats st{};r=s.list(m.owner_id,m.device_id,&got,&st);}
  else if(op=="inventory"){Stats st{};r=s.inventory(&st);}
  else if(op=="save-old"){m.epoch=1577836800;r=save(s,m);}
  else if(op=="save-future"){m.epoch=4294960000U;r=save(s,m);}
  else if(op.rfind("collision:",0)==0){std::string field=op.substr(10);
    if(field=="owner")std::strcpy(m.owner_id,"other-owner");else if(field=="device")std::strcpy(m.device_id,"other-device");else if(field=="job")m.job_id++;else if(field=="epoch")m.epoch=1789504444;else if(field=="crc")m.crc32^=1;else if(field=="len")m.len+=2;
#ifdef IMAGE
    else if(field=="sha")m.checksum_sha256[0]=m.checksum_sha256[0]=='0'?'1':'0';else if(field=="mode")std::strcpy(m.mode,"dish");else if(field=="quantity")m.quantity++;else if(field=="camera")m.camera.actual_width++;
#else
    else if(field=="session")std::strcpy(m.session_id,"changed-session");
#endif
    r=s.begin(m);
  }else if(op=="wrong-erase")r=s.erase(m.request_id,"other-owner",m.device_id,m.len,m.crc32);
  else if(op=="wrong-attempt")r=s.mark_attempt(m.request_id,m.owner_id,"other-device",m.len,m.crc32,1789501234,&got);
  else if(op=="renew-attempt")r=s.mark_attempt(m.request_id,m.owner_id,m.device_id,m.len,m.crc32,1789501235,&got);
  armed=false;
 }
 ::close(tracefd);tracefd=-1;armed=false;
 Store reboot(root.c_str());Meta check{};m=meta(100);Result look=reboot.lookup(m.request_id,m.owner_id,m.device_id,&check);Stats stats{};Meta first{};Result listed=reboot.list(m.owner_id,m.device_id,&first,&stats);Stats inv{};Result ir=reboot.inventory(&inv);
 bool preserved=true;for(int i=1;i<=count;++i)preserved=preserved&&lookup_same(reboot,i);
 bool equal=look==Result::Ok&&media::same(m,check);
 if(op=="attempt"||op=="renew-attempt"||op=="verify"){Meta attempted=m;attempted.epoch=1789501234;equal=look==Result::Ok&&(media::same(m,check)||media::same(attempted,check));}
 std::printf("{\"result\":\"%s\",\"lookup\":\"%s\",\"list_result\":\"%s\",\"count\":%u,\"invalid\":%u,\"incomplete\":%u,\"inventory_result\":\"%s\",\"inventory_count\":%u,\"epoch\":%u,\"ordinal\":%llu,\"target_exact\":%s,\"seed_preserved\":%s,\"hit\":%s,\"events\":%d}\n",media::result_name(r),media::result_name(look),media::result_name(listed),stats.count,stats.invalid,stats.incomplete,media::result_name(ir),inv.count,check.epoch,(unsigned long long)check.ordinal,equal?"true":"false",preserved?"true":"false",hit?"true":"false",event_no);
 return 0;
}
'''


def pin(path):
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def snapshot(path):
    return {str(p.relative_to(path)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob('*') if p.is_file()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--seed', type=int, default=16320260915)
    ap.add_argument('--random-cases', type=int, default=160)
    args = ap.parse_args()
    if args.out.exists():
        raise SystemExit('Evidence directory already exists; choose a fresh path')
    args.out.mkdir(parents=True)
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise SystemExit('Host C++ compiler required')
    rng = random.Random(args.seed)
    started = time.time()
    cases = []
    findings = []
    commands = []
    counts = {}
    harness = args.out / 'harness.cpp'
    harness.write_text(HARNESS)
    (args.out / 'stress_media_storage.py').write_bytes(Path(__file__).read_bytes())
    source_pins = [pin(args.source_root / 'halo_common' / n) for n in ('VoiceSpoolStore.h', 'ImageSpoolStore.h')]
    script_pin = pin(Path(__file__).resolve())

    def record(media, category, name, ok, details=None):
        case = {'media': media, 'category': category, 'name': name, 'pass': bool(ok)}
        if details is not None:
            case['details'] = details
        cases.append(case)
        counts[category] = counts.get(category, 0) + 1
        if not ok:
            findings.append(case)
            if sum(x.get('category') == category for x in findings) <= 3:
                print('FINDING', json.dumps(case), flush=True)
        with (args.out / 'CASES.jsonl').open('a') as f:
            f.write(json.dumps(case) + '\n')

    try:
        with tempfile.TemporaryDirectory(prefix='halo-media-stress-', dir='/tmp') as temporary:
            base = Path(temporary)
            payload = bytes(rng.getrandbits(8) for _ in range(1536))
            payload_file = base / 'payload'
            payload_file.write_bytes(payload)
            payload_sha = hashlib.sha256(payload).hexdigest()
            for media in ('voice', 'image'):
                binary = args.out / ('stress-' + media)
                argv = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-Wno-misleading-indentation', '-I', str(args.source_root / 'halo_common'),
                        str(harness), '-o', str(binary)]
                if media == 'image':
                    argv.insert(1, '-DIMAGE')
                commands.append(argv)
                compiled = subprocess.run(argv, text=True, capture_output=True, timeout=60)
                (args.out / (media + '-compile.log')).write_text(compiled.stdout + compiled.stderr)
                if compiled.returncode:
                    raise RuntimeError(media + ' harness compile failed')
                namespace = 'voice-spool-v1' if media == 'voice' else 'image-spool-v1'
                suffix = '.pcm' if media == 'voice' else '.jpg'
                trace = base / 'trace.tsv'

                def run(root, op, count=2, fault_id=-1, mode='none'):
                    argv = [str(binary), str(root / namespace), op, str(payload_file), payload_sha,
                            str(fault_id), mode, str(trace), str(count)]
                    p = subprocess.run(argv, text=True, capture_output=True, timeout=15)
                    events = [dict(zip(('id', 'op', 'path'), line.split('\t', 2)))
                              for line in trace.read_text().splitlines()] if trace.exists() else []
                    result = json.loads(p.stdout) if p.returncode == 0 else None
                    if p.returncode not in (0, 80, 81):
                        raise RuntimeError('Unexpected harness exit: ' + repr((argv, p.returncode, p.stdout, p.stderr)))
                    return p.returncode, result, events

                seeded = base / ('seeded-' + media)
                seeded.mkdir()
                legacy = seeded / 'spool'
                legacy.mkdir()
                (legacy / 'untouched.jpg').write_bytes(b'legacy-retained-sentinel')
                _, initial, _ = run(seeded, 'seed')
                if not initial['seed_preserved']:
                    raise RuntimeError('seed failed')
                original = snapshot(seeded)
                committed = base / ('committed-' + media)
                shutil.copytree(seeded, committed)
                _, good, _ = run(committed, 'save')
                if good['result'] != 'ok' or not good['target_exact']:
                    raise RuntimeError('unfaulted save failed')
                target_stem = f'{100:032x}'
                target_paths = [namespace + '/' + target_stem + '.meta', namespace + '/' + target_stem + suffix]
                committed_bytes = snapshot(committed)
                trial = base / 'trial'

                def copy_trial(source):
                    if trial.exists():
                        shutil.rmtree(trial)
                    shutil.copytree(source, trial)

                # Enumerate every observed actual filesystem call. Crash at all
                # boundaries; short/corrupt writes and return errors are separate.
                for operation in ('save', 'attempt', 'erase'):
                    source = seeded if operation == 'save' else committed
                    copy_trial(source)
                    _, clean, catalogue = run(trial, operation)
                    (args.out / f'{media}-{operation}-catalogue.json').write_text(json.dumps(catalogue, indent=2) + '\n')
                    for event in catalogue:
                        modes = ['error', 'crash_before', 'crash_after']
                        if event['op'] in ('fwrite', 'fread'):
                            modes.append('short')
                        if event['op'] == 'fwrite':
                            modes.append('corrupt')
                        for mode in modes:
                            copy_trial(source)
                            code, res, observed = run(trial, operation, fault_id=int(event['id']), mode=mode)
                            _, reboot, _ = run(trial, 'verify')
                            current = snapshot(trial)
                            intact = all(current.get(p) == h for p, h in original.items())
                            ok = intact and reboot['seed_preserved']
                            if code == 0 and res['result'] in ('ok', 'already_stored'):
                                if operation == 'save':
                                    ok &= reboot['lookup'] == 'ok' and reboot['target_exact'] and reboot['ordinal'] > 2
                                elif operation == 'attempt':
                                    ok &= reboot['lookup'] == 'ok' and reboot['epoch'] == 1789501234
                                else:
                                    ok &= not any(p in current for p in target_paths)
                            if operation == 'attempt':
                                # Its old metadata/payload are immutable even if
                                # the new marker becomes corrupt and is held.
                                ok &= all(current.get(p) == committed_bytes[p] for p in target_paths)
                            if reboot['lookup'] == 'ok':
                                ok &= reboot['target_exact']
                            detail = {'operation': operation, 'fault': event, 'mode': mode,
                                      'exit_code': code, 'returned': res, 'reopened': reboot,
                                      'seed_files_unchanged': intact}
                            record(media, 'syscall_fault', operation + ':' + event['id'] + ':' + mode, ok, detail)
                            if not ok:
                                dest = args.out / ('failure-' + media + '-' + str(len(findings)))
                                shutil.copytree(trial, dest)

                # Read-only scan failures must never pretend the queue is empty
                # or complete. No mutation is allowed by these inventory calls.
                for operation in ('list', 'inventory'):
                    copy_trial(committed)
                    _, clean_scan, scan_catalogue = run(trial, operation)
                    for event in scan_catalogue:
                        if event['op'] != 'readdir':
                            continue
                        copy_trial(committed)
                        _, res, _ = run(trial, operation, fault_id=int(event['id']), mode='error')
                        record(media, 'readonly_scan_error', operation + ':' + event['id'],
                               res['result'] == 'io' and snapshot(trial) == committed_bytes,
                               {'fault': event, 'returned': res['result'], 'unchanged': snapshot(trial) == committed_bytes})
                # Age is retained metadata, not a store deletion policy. The
                # actual Sense replay eligibility layer separately holds it.
                for operation, epoch in [('save-old', 1577836800), ('save-future', 4294960000)]:
                    copy_trial(seeded)
                    _, saved, _ = run(trial, operation)
                    aged = snapshot(trial)
                    _, reopened, _ = run(trial, 'verify')
                    record(media, 'age_retention', operation,
                           saved['result'] == 'ok' and reopened['lookup'] == 'ok' and reopened['epoch'] == epoch and snapshot(trial) == aged,
                           {'epoch': reopened['epoch'], 'retained': snapshot(trial) == aged})

                # Every metadata byte including terminators/checksum, plus torn
                # prefixes, payload truncation/extension and seeded bit errors.
                for kind in ('metadata_bit', 'metadata_prefix', 'payload_bit', 'payload_length'):
                    meta_path = committed / target_paths[0]
                    raw_meta = meta_path.read_bytes()
                    if kind == 'metadata_bit':
                        values = range(len(raw_meta))
                    elif kind == 'metadata_prefix':
                        values = range(len(raw_meta))
                    elif kind == 'payload_bit':
                        values = rng.sample(range(len(payload)), 64)
                    else:
                        values = [0, 1, 511, 512, len(payload) - 1, len(payload) + 1]
                    for value in values:
                        copy_trial(committed)
                        p = trial / target_paths[0 if kind.startswith('metadata') else 1]
                        data = bytearray(p.read_bytes())
                        if kind.endswith('bit'):
                            data[value] ^= 1 << (rng.randrange(8))
                        elif kind == 'metadata_prefix':
                            data = data[:value]
                        else:
                            data = data[:value] if value <= len(data) else data + b'x'
                        p.write_bytes(data)
                        bad_before = snapshot(trial)
                        _, res, _ = run(trial, 'verify')
                        _, retry, _ = run(trial, 'begin')
                        ok = res['lookup'] != 'ok' and res['seed_preserved'] and retry['result'] not in ('ok', 'already_stored') and snapshot(trial) == bad_before
                        record(media, kind, str(value), ok, {'lookup': res['lookup'], 'retry': retry['result'], 'retained': snapshot(trial) == bad_before})

                # Exact identity collision/foreign owner cannot replace/delete.
                fields = ['owner', 'device', 'job', 'epoch', 'crc', 'len']
                fields += ['sha', 'mode', 'quantity', 'camera'] if media == 'image' else ['session']
                for op in ['collision:' + f for f in fields] + ['wrong-erase', 'wrong-attempt']:
                    copy_trial(committed)
                    _, res, _ = run(trial, op)
                    record(media, 'collision_owner', op, res['result'] not in ('ok', 'already_stored') and snapshot(trial) == committed_bytes, res)
                copy_trial(committed)
                _, first, _ = run(trial, 'attempt')
                marked = snapshot(trial)
                _, second, _ = run(trial, 'renew-attempt')
                record(media, 'attempt_horizon', 'cannot-renew-first-attempt', first['result'] == 'ok' and second['result'] == 'conflict' and marked == snapshot(trial), second)

                # Fully occupied capacity includes held/incomplete stems. Check
                # all directory-enumeration error points, not just ENOSPC writes.
                full = base / ('full-' + media)
                full.mkdir()
                _, seeded40, _ = run(full, 'seed', count=40)
                full_snapshot = snapshot(full)
                _, refused, catalogue = run(full, 'save', count=40)
                record(media, 'capacity40', 'ordinary-41st-refused', seeded40['count'] == 40 and refused['result'] == 'full' and snapshot(full) == full_snapshot, refused)
                for event in catalogue:
                    if event['op'] not in ('opendir', 'readdir', 'closedir'):
                        continue
                    copy_trial(full)
                    _, res, _ = run(trial, 'save', count=40, fault_id=int(event['id']), mode='error')
                    now = snapshot(trial)
                    retained = all(now.get(p) == h for p, h in full_snapshot.items())
                    record(media, 'capacity_scan_error', event['id'], res['result'] not in ('ok', 'already_stored') and res['inventory_count'] <= 40 and retained, {'fault': event, 'result': res, 'existing40_unchanged': retained})
                copy_trial(full)
                # Turn one committed slot into incomplete metadata: still40.
                slot = trial / namespace / (f'{1:032x}' + '.meta')
                slot.rename(slot.with_name(slot.name + '.part'))
                held_snapshot = snapshot(trial)
                _, res, _ = run(trial, 'save', count=0)
                record(media, 'capacity40', 'incomplete-stem-counts', res['result'] == 'full' and snapshot(trial) == held_snapshot, res)

                # Missing mount represented by ENODEV/EIO calls is covered above;
                # also test a physically inaccessible root path (ENOTDIR).
                missing = base / ('missing-' + media)
                missing.mkdir()
                (missing / namespace).write_bytes(b'not-a-directory')
                missing_before = snapshot(missing)
                _, res, _ = run(missing, 'save', count=0)
                record(media, 'missing_storage', 'not-a-mounted-directory', res['result'] not in ('ok', 'already_stored') and snapshot(missing) == missing_before, res)

                # Deterministic multi-fault/reopen sequences. An item is held if
                # damage prevents retry; never auto-delete it to make progress.
                for i in range(args.random_cases):
                    copy_trial(seeded)
                    history = []
                    prior_durable = dict(original)
                    ok = True
                    for step in range(3):
                        choose = rng.choice(('error', 'short', 'crash_before', 'crash_after'))
                        fi = rng.randrange(1, 55)
                        code, res, _ = run(trial, 'save', fault_id=fi, mode=choose)
                        _, reboot, _ = run(trial, 'verify')
                        now = snapshot(trial)
                        ok &= all(now.get(p) == h for p, h in prior_durable.items())
                        if code == 0 and res['result'] in ('ok', 'already_stored'):
                            ok &= reboot['lookup'] == 'ok' and reboot['target_exact']
                        if reboot['lookup'] == 'ok':
                            ok &= reboot['target_exact']
                            prior_durable.update({p: now[p] for p in target_paths})
                        history.append({'step': step, 'fault_id': fi, 'mode': choose, 'exit': code, 'result': res, 'reopen': reboot})
                    record(media, 'seeded_sequence', str(i), ok, history if not ok else None)
                print(media, 'completed', len(cases), 'total cases;', len(findings), 'findings', flush=True)
            # Both actual store implementations coexist with identical request
            # IDs; an accepted erase in one must leave the other and legacy data.
            mixed = base / 'mixed'; mixed.mkdir()
            (mixed / 'spool').mkdir(); (mixed / 'spool/old.jpg').write_bytes(b'legacy')
            def mixed_run(which, operation):
                ns = 'voice-spool-v1' if which == 'voice' else 'image-spool-v1'
                argv = [str(args.out / ('stress-' + which)), str(mixed / ns), operation,
                        str(payload_file), payload_sha, '-1', 'none', str(trace), '2']
                result = subprocess.run(argv, text=True, capture_output=True, timeout=15)
                if result.returncode: raise RuntimeError('mixed namespace child failed')
                return json.loads(result.stdout)
            for which in ('voice', 'image'):
                mixed_run(which, 'seed'); got = mixed_run(which, 'save')
                record(which, 'namespace', 'same-request-independent-commit', got['result'] == 'ok' and got['target_exact'], got)
            both = snapshot(mixed)
            erased = mixed_run('voice', 'erase')
            remaining = snapshot(mixed)
            record('both', 'namespace', 'voice-erase-preserves-image-and-legacy',
                   erased['result'] == 'ok' and all(remaining.get(p) == h for p, h in both.items() if not p.startswith('voice-spool-v1/')), None)
            image = mixed_run('image', 'verify')
            record('both', 'namespace', 'image-reopens-after-other-erase', image['lookup'] == 'ok' and image['target_exact'], image)
    except Exception as exc:
        findings.append({'infrastructure_error': repr(exc)})
        raise
    finally:
        current_pins = [pin(args.source_root / 'halo_common' / n) for n in ('VoiceSpoolStore.h', 'ImageSpoolStore.h')]
        if current_pins != source_pins:
            findings.append({'source_changed_during_run': True})
        receipt = {'status': 'PASS' if not findings else 'FAIL', 'seed': args.seed,
                   'started_epoch': started, 'finished_epoch': time.time(),
                   'cases': len(cases), 'categories': counts, 'findings': findings,
                   'script': script_pin, 'runtime_sources': source_pins, 'harness': pin(harness),
                   'compiler_commands': commands, 'hardware_executed': False,
                   'runtime_modified': False,
                   'limits': ['Host POSIX fault model; not SD/FAT physical power-cut or mount-driver qualification.',
                              'Read EIO, including partial reads, sets ferror as POSIX does; actual truncated files are tested separately.',
                              'Persistent corrupt-held committed metadata is skipped during ordinal selection, preserving admission availability but not a global FIFO guarantee across corruption.',
                              'Store verifies CRC and metadata integrity; image SHA256 admission remains Sense/backend responsibility.',
                              'Store does not age-delete records. First-attempt epoch is immutable; runtime replay-age admission is a separate layer.',
                              'Read/custody interpretation is checked after process reopen; no filesystem cache eviction or actual power interruption is claimed.']}
        (args.out / 'RESULT.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({'status': receipt['status'], 'cases': len(cases), 'findings': len(findings), 'result': pin(args.out / 'RESULT.json')}, indent=2))
    raise SystemExit(0 if not findings else 1)


if __name__ == '__main__':
    main()
