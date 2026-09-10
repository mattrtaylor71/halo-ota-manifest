#!/usr/bin/env python3
"""Prepare a private, declared NVS fixture offline. No serial, flash or network API."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import stat
import struct
import subprocess
import tempfile
import time
import zlib

HERE = Path(__file__).resolve().parent
PARSER = HERE / 'production_nvs_vendor/nvs_parser.py'
PARSER_SHA = '621bdbf0ac60e34ae190f63be10f0f1a4dd4d18c25ebab0cf56ff53a6f2b6c2c'
POLICY = ('ota_coord', 'retry_v1')
TIMEZONE = ('halo_prov', 'tz')
PACIFIC = 'PST8PDT,M3.2.0,M11.1.0'
# Explicit NEW archived test-case scope only; never an in-case retry/refund.
# Namespace membership, boot/generation identity, provisioning and failure history stay.
NEW_CASE_KEYS = {
    'sense': {('ota_coord', k) for k in ('retry_v1', 'elig_v1', 'schedule', 'pending', 'complete', 'done_ids', 'target')} |
             {('ota_expect', k) for k in ('pending', 'exp_ver', 'prev_lbl', 'prev_addr')} | {('halo', 'lcd_ota_due')},
    'lcd': {('lcd_durable', k) for k in ('coord_v2', 'cont_v2', 'progress_v2')} |
           {('lcd_maint', k) for k in ('arm_v1', 'valid', 'armed', 'wake_s', 'remain_s', 'req_id', 'start_ep', 'dur_s', 'grace_b', 'grace_a')},
}


def need(ok, code):
    if not ok:
        raise ValueError(code)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def crc(data):
    return zlib.crc32(data, 0xffffffff) & 0xffffffff


def parse(raw):
    """Strict existing parser custody plus complete logical-key/chunk validation."""
    need(len(raw) == 20480, 'exact_20KiB_partition_required')
    need(sha(PARSER.read_bytes()) == PARSER_SHA, 'parser_pin_changed')
    spec = importlib.util.spec_from_file_location('production_nvs_parser', PARSER)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    try:
        partition = module.NVS_Partition('private_fixture', bytearray(raw))
    except Exception:
        raise ValueError('NVS_structure') from None
    written, sequences, active = [], [], []
    for page in partition.pages:
        off = page.start_address
        if page.header['status'] == 'Empty':
            need(raw[off:off+4096] == b'\xff'*4096, 'uninitialized_page_data'); continue
        need(page.header['status'] in ('Active', 'Full'), 'NVS_page_state')
        need(page.header['version'] in (1, 2), 'NVS_version')
        need(page.header['crc']['original'] == page.header['crc']['computed'], 'NVS_page_CRC')
        sequences.append(page.header['page_index'])
        need(page.header['page_index'] < 0xffffffff, 'NVS_sequence_wrap')
        if page.header['status'] == 'Active': active.append(page)
        states = [(page.raw_entry_state_bitmap[i//4] >> ((i%4)*2)) & 3 for i in range(126)]
        need(all(s in (0, 2, 3) for s in states), 'NVS_bitmap')
        covered = set()
        for entry in page.entries:
            members = [entry] + entry.children
            span = entry.metadata['span']; count = 1 if span in (0, 255) else span
            need(len(members) == count and entry.index + count <= 126, 'NVS_span')
            for child in members:
                need(child.index not in covered, 'NVS_overlap'); covered.add(child.index)
            if entry.state == 'Empty':
                need(count == 1 and bytes(entry.raw) == b'\xff'*32, 'NVS_empty_data'); continue
            if entry.state == 'Erased':
                need(all(c.state == 'Erased' for c in members), 'NVS_partial_erased'); continue
            need(entry.state == 'Written' and all(c.state == 'Written' for c in members), 'NVS_partial_written')
            need(span not in (0, 255) and entry.metadata['crc']['original'] == entry.metadata['crc']['computed'], 'NVS_entry_CRC')
            key_bytes = bytes(entry.raw[8:24]); key = key_bytes.split(b'\0', 1)[0]
            need(0 < len(key) <= 15 and key_bytes == key+b'\0'*(16-len(key)) and all(32 <= c < 127 for c in key), 'NVS_key')
            typ = entry.metadata['type']
            need(typ in module.nvs_const.item_type.values(), 'NVS_type')
            payload = None
            if typ in ('string', 'blob', 'blob_data'):
                size = entry.data['size']
                need(count == 1+(size+31)//32, 'NVS_variable_span')
                payload = b''.join(bytes(c.raw) for c in entry.children)[:size]
                need(len(payload) == size and crc(payload) == entry.metadata['crc']['data_original'], 'NVS_data_CRC')
                if typ == 'string': need(payload.endswith(b'\0') and b'\0' not in payload[:-1], 'NVS_string')
            else:
                need(count == 1, 'NVS_scalar_span')
            written.append({'offset':off+64+32*entry.index, 'page':page, 'entry':entry, 'key':key.decode('ascii'), 'payload':payload})
        need(covered == set(range(126)), 'NVS_coverage')
    need(len(active) <= 1 and len(sequences) == len(set(sequences)), 'NVS_ambiguous_pages')
    names = {}; groups = {}
    for row in written:
        e = row['entry']
        if e.metadata['namespace'] == 0:
            need(e.metadata['type'] == 'uint8_t', 'NVS_namespace_type')
            index = e.data['value']
            need(1 <= index <= 254 and index not in names and row['key'] not in names.values(), 'NVS_namespace_ambiguity')
            names[index] = row['key']
    for row in written:
        ns = row['entry'].metadata['namespace']
        if not ns: continue
        need(ns in names, 'NVS_unknown_namespace')
        groups.setdefault((names[ns], row['key']), []).append(row)
    values = {}; locations = {}
    for key, rows in groups.items():
        indices = [r for r in rows if r['entry'].metadata['type'] == 'blob_index']
        if indices:
            need(len(indices) == 1, 'NVS_duplicate_blob_index')
            index = indices[0]['entry'].data
            chunks = sorted([r for r in rows if r['entry'].metadata['type'] == 'blob_data'], key=lambda r:r['entry'].metadata['chunk_index'])
            need(len(rows) == len(chunks)+1 and len(chunks) == index['chunk_count'] and index['chunk_count'] > 0, 'NVS_blob_join')
            need([r['entry'].metadata['chunk_index'] for r in chunks] == list(range(index['chunk_start'], index['chunk_start']+index['chunk_count'])), 'NVS_chunk_sequence')
            payload = b''.join(r['payload'] for r in chunks)
            need(len(payload) == index['size'], 'NVS_blob_size')
            values[key] = ('blob', payload); locations[key] = chunks
        else:
            need(len(rows) == 1 and rows[0]['entry'].metadata['type'] != 'blob_data', 'NVS_duplicate_or_orphan')
            row = rows[0]; typ = row['entry'].metadata['type']
            values[key] = (typ, row['payload'] if row['payload'] is not None else bytes(row['entry'].raw[24:32]))
            locations[key] = rows
    return {'partition':partition, 'active':active, 'names':names, 'values':values, 'locations':locations, 'entries':groups}


def entry_crc(data, offset):
    struct.pack_into('<I', data, offset+4, crc(data[offset:offset+4]+data[offset+8:offset+32]))


def replace_policy(data, parsed, replacement):
    need(POLICY in parsed['values'] and parsed['values'][POLICY][0] == 'blob', 'existing_policy_blob_required')
    need(len(parsed['values'][POLICY][1]) == len(replacement) == 768, 'policy_size')
    allowed = set(); cursor = 0
    for row in parsed['locations'][POLICY]:
        off = row['offset']; count = len(row['payload'])
        data[off+32:off+32+count] = replacement[cursor:cursor+count]; cursor += count
        struct.pack_into('<I', data, off+28, crc(data[off+32:off+32+count])); entry_crc(data, off)
        allowed.update(range(off+32, off+32+count)); allowed.update(range(off+4, off+8)); allowed.update(range(off+28, off+32))
    need(cursor == 768, 'policy_chunk_total')
    return allowed


def set_state(data, page, index, state):
    off = page.start_address+32+index//4; shift = (index%4)*2
    data[off] = (data[off] & ~(3 << shift)) | state << shift
    return off


def replace_timezone(data, parsed, timezone):
    validate_timezone(timezone)
    payload = timezone.encode('ascii')+b'\0'
    need(1 < len(payload) <= 64 and all(32 <= c < 127 for c in payload[:-1]), 'timezone_length_or_encoding')
    # Same consumer format as ProvisioningState::saveTimezone. No allocator/GC rewrite.
    ns = [i for i,n in parsed['names'].items() if n == TIMEZONE[0]]
    need(len(ns) == 1 and len(parsed['active']) == 1, 'existing_provisioning_namespace_and_active_page_required')
    old = parsed['locations'].get(TIMEZONE, [])
    if old: need(parsed['values'][TIMEZONE][0] == 'string' and len(old) == 1, 'timezone_storage_type')
    page = parsed['active'][0]; span = 1+(len(payload)+31)//32
    bitmap = page.raw_entry_state_bitmap
    states = [(bitmap[i//4] >> ((i%4)*2)) & 3 for i in range(126)]
    tail = max([i+1 for i,s in enumerate(states) if s != 3] or [0])
    need(tail+span <= 126, 'no_safe_active_page_tail_capacity')
    off = page.start_address+64+tail*32
    need(data[off:off+span*32] == b'\xff'*(span*32), 'nonempty_append_tail')
    header = bytearray(b'\xff'*32); header[:4] = bytes((ns[0],0x21,span,255))
    header[8:24] = b'tz'+b'\0'*14; struct.pack_into('<H', header,24,len(payload)); struct.pack_into('<I',header,28,crc(payload)); entry_crc(header,0)
    data[off:off+32] = header; data[off+32:off+32+len(payload)] = payload
    allowed = set(range(off,off+span*32))
    for i in range(tail,tail+span): allowed.add(set_state(data,page,i,2))
    for row in old:
        e = row['entry']
        for i in range(e.index,e.index+e.metadata['span']): allowed.add(set_state(data,row['page'],i,0))
    return allowed


def validate_timezone(timezone):
    fixed = re.fullmatch(r'[A-Za-z]{3,10}[+-]?(\d{1,2})(?::[0-5]\d){0,2}',timezone)
    need(timezone == PACIFIC or (fixed is not None and int(fixed.group(1)) <= 24),'fixture_timezone_must_be_Pacific_or_fixed_POSIX_offset')


def transform(raw, replacement=None, timezone=None, new_case_board=None):
    before = parse(raw); data = bytearray(raw); allowed = set(); changed_keys = set()
    if new_case_board is not None:
        need(new_case_board in NEW_CASE_KEYS and replacement is None, 'separate_new_case_mode_required')
        for key in NEW_CASE_KEYS[new_case_board]:
            for row in before['entries'].get(key, []):
                e=row['entry']
                for i in range(e.index,e.index+e.metadata['span']):
                    allowed.add(set_state(data,row['page'],i,0))
                changed_keys.add(key)
    if replacement is not None:
        allowed |= replace_policy(data,before,replacement); changed_keys.add(POLICY)
    if timezone is not None:
        allowed |= replace_timezone(data,before,timezone); changed_keys.add(TIMEZONE)
    need(changed_keys, 'no_fixture_edit_requested')
    after = parse(bytes(data))
    need(before['names'] == after['names'], 'namespace_changed')
    need({k:v for k,v in before['values'].items() if k not in changed_keys} == {k:v for k,v in after['values'].items() if k not in changed_keys}, 'untouched_logical_key_changed')
    if replacement is not None: need(after['values'][POLICY] == ('blob',replacement), 'policy_readback')
    if timezone is not None: need(after['values'][TIMEZONE] == ('string',timezone.encode('ascii')+b'\0'), 'timezone_readback')
    if new_case_board is not None: need(not (NEW_CASE_KEYS[new_case_board] & set(after['values'])), 'new_case_policy_absence_readback')
    differences = [i for i,(a,b) in enumerate(zip(raw,data)) if a != b]
    need(set(differences) <= allowed, 'undeclared_byte_change')
    return bytes(data), {'changed_bytes':len(differences), 'changed_sectors':sorted({i//4096*4096 for i in differences}), 'all_other_logical_keys_unchanged':True, 'only_declared_bytes_changed':True, 'new_case_board':new_case_board, 'changed_allowlisted_keys':['/'.join(k) for k in sorted(changed_keys)]}


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0,0))
    signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGINT,signal.SIGTERM})


def run_child(argv, data=None, timeout=30, receipts=None):
    started=time.time()
    child = None
    try:
        mask=signal.pthread_sigmask(signal.SIG_BLOCK,{signal.SIGINT,signal.SIGTERM})
        try:
            child = subprocess.Popen(argv,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True,preexec_fn=no_core)
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK,mask)
        stdout,stderr = child.communicate(input=data,timeout=timeout)
        need(child.returncode == 0, 'host_codec_or_compiler_refused')
        return stdout
    finally:
        previous = {s:signal.signal(s,signal.SIG_IGN) for s in (signal.SIGINT,signal.SIGTERM)}
        try:
            if child is None: raise ValueError('host_child_not_started')
            def present():
                child.poll()
                try: os.killpg(child.pid,0); return True
                except ProcessLookupError: return False
                except PermissionError: return True  # Unknown, never proof of absence.
            for sig in (signal.SIGTERM,signal.SIGKILL):
                if not present(): break
                try: os.killpg(child.pid,sig)
                except ProcessLookupError: break
                except PermissionError: pass  # Keep bounded polling and always reap the child.
                until=time.monotonic()+2
                while present() and time.monotonic()<until:
                    time.sleep(.02)
            child.wait(timeout=2)
            need(not present(),'owned_host_group_not_closed')
            if receipts is not None:
                receipts.append({'pid':child.pid,'exit_code':child.returncode,'reaped':True,'group_absent':True,'core_dumps_disabled':True,'started_epoch':started,'finished_epoch':time.time()})
        finally:
            if child is not None:
                for pipe in (child.stdin,child.stdout,child.stderr):
                    if pipe is not None: pipe.close()
            for sig,handler in previous.items(): signal.signal(sig,handler)


def private_write(path, data):
    fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(data); f.flush(); os.fsync(f.fileno())


def prepare(input_path, expected_sha, output, case_id, installed_version, due, timezone=None, exhausted=False, now=None, new_case_board=None):
    input_path=Path(input_path);output=Path(output)
    output=output.resolve()
    need(output != HERE.parent and HERE.parent not in output.parents,'private_output_must_be_outside_release_checkout')
    need(input_path.is_file() and not input_path.is_symlink() and stat.S_IMODE(input_path.stat().st_mode)==0o600,'private_regular_input_required')
    raw=input_path.read_bytes();need(sha(raw)==expected_sha,'input_SHA_mismatch')
    need(re.fullmatch(r'[a-zA-Z0-9_-]{8,80}',case_id) is not None,'explicit_new_case_id_required')
    need(new_case_board is None or (new_case_board in NEW_CASE_KEYS and not exhausted), 'separate_new_case_mode_required')
    need(new_case_board != 'lcd' or timezone is None, 'timezone_is_Sense_owned')
    parsed=parse(raw); replacement=None; policy_after=None; children=[]
    now=int(time.time()) if now is None else now
    zone=timezone if timezone is not None else parsed['values'].get(TIMEZONE,('string',(PACIFIC+'\0').encode()))[1][:-1].decode('ascii')
    validate_timezone(zone)
    if exhausted:need(POLICY in parsed['values'],'existing_policy_required')
    compiler=shutil.which('c++');need(compiler is not None,'host_C++_compiler_required')
    source_paths=(HERE/'production_policy_fixture.cpp',HERE.parent/'halo_ota_demo/firmware/shared/DurableOtaPolicy.h',HERE.parent/'halo_ota_demo/firmware/shared/NightlySchedule.h')
    contract_paths=tuple(HERE.parent/p for p in ('halo_ota_demo/firmware/shared/SenseDurablePolicyState.h','halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino','LCD_Minimal/lcd_nvs_store.h','LCD_Minimal/lcd_maintenance_arm.h','LCD_Minimal/lcd_sleep.h'))
    source_pins={str(p):sha(p.read_bytes()) for p in (*source_paths,*contract_paths)}
    with tempfile.TemporaryDirectory(prefix='halo-policy-fixture-') as temp:
        os.chmod(temp,0o700);binary=Path(temp)/'codec'
        run_child([compiler,'-std=c++17','-O2',str(source_paths[0]),'-o',str(binary)],receipts=children)
        calendar_due=int(run_child([str(binary),'calendar',str(now),zone],receipts=children))
        need(not due or due==calendar_due,'due_must_match_actual_NightlySchedule_and_timezone')
        due=calendar_due
        if POLICY in parsed['values']:
            policy_after=json.loads(run_child([str(binary),'inspect'],parsed['values'][POLICY][1],receipts=children))
        if exhausted:
            replacement=run_child([str(binary),'exhausted-yesterday',str(now),str(due),installed_version],parsed['values'][POLICY][1],receipts=children)
            policy_after=json.loads(run_child([str(binary),'inspect'],replacement,receipts=children))
    need(source_pins=={str(p):sha(p.read_bytes()) for p in (*source_paths,*contract_paths)},'codec_source_changed_during_preparation')
    changed,scope=transform(raw,replacement,timezone,new_case_board)
    policy_before=policy_after if new_case_board == 'sense' else None
    if new_case_board == 'sense': policy_after=None
    output.mkdir(parents=True,exist_ok=False,mode=0o700);os.chmod(output,0o700)
    private_write(output/'before-nvs.bin',raw);private_write(output/'fixture-nvs.bin',changed)
    if replacement is not None: private_write(output/'fixture-policy.bin',replacement)
    result={'status':'PREPARED_NEW_CASE_NVS_FIXTURE_NOT_INSTALLED','case_id':case_id,'declared_current_epoch':now,'normal_due_epoch':due,'installed_version':installed_version,'input_sha256':sha(raw),'fixture_sha256':sha(changed),'bytes':len(raw),'scope':scope,'policy':policy_after,'archived_previous_policy':policy_before,'timezone_changed':timezone is not None,'synthetic_prior_state':bool(exhausted or new_case_board),'factory_fresh_claim_proven':False,'real_elapsed_overnight_proven':False,'hardware_actions':0,'network_actions':0,'host_children':children,'requires':'Fresh actual partition/security/identity admission and exact sector write/readback by the sole hardware owner; current-clock and calendar-origin validation remain hardware evidence. No RTC state is changed.','source_refs':{str(p.relative_to(HERE.parent)):sha(p.read_bytes()) for p in (Path(__file__).resolve(),*source_paths,*contract_paths,PARSER)}}
    private_write(output/'result.json',(json.dumps(result,indent=2)+'\n').encode())
    return result


def main():
    os.umask(0o077)
    def interrupted(signum,frame): raise InterruptedError('host fixture interrupted')
    for sig in (signal.SIGTERM,signal.SIGINT): signal.signal(sig,interrupted)
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input-nvs',required=True);p.add_argument('--input-sha256',required=True);p.add_argument('--out',required=True);p.add_argument('--case-id',required=True)
    p.add_argument('--installed-version',required=True);p.add_argument('--normal-due-epoch',type=int,default=0);p.add_argument('--exhausted-yesterday',action='store_true');p.add_argument('--timezone')
    p.add_argument('--new-case-policy-absence',choices=('sense','lcd'),help='Prepare a separately archived NEW case by retiring only named policy/coordinator/legacy keys; never use within an active failure case')
    a=p.parse_args()
    try:
        result=prepare(a.input_nvs,a.input_sha256,a.out,a.case_id,a.installed_version,a.normal_due_epoch,a.timezone,a.exhausted_yesterday,new_case_board=a.new_case_policy_absence)
    except (ValueError,OSError,subprocess.SubprocessError,UnicodeError):
        print(json.dumps({'status':'REFUSED_HOST_FIXTURE','hardware_actions':0,'network_actions':0}));return 2
    print(json.dumps({'status':result['status'],'result':str(Path(a.out).resolve()/'result.json'),'fixture_sha256':result['fixture_sha256'],'hardware_actions':0,'network_actions':0}));return 0


if __name__=='__main__': raise SystemExit(main())
