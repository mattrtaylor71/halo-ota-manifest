"""Synthetic private partitions and the exact production codec; no saved user data/I/O."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
import production_nvs_fixture as f

NOW = 1789002000
DUE = NOW+300


def header(ns, typ, key, span=1, chunk=255):
    data=bytearray(b'\xff'*32);data[:4]=bytes((ns,typ,span,chunk));data[8:24]=key.encode()+b'\0'*(16-len(key));return data


def scalar(ns,key,value):
    h=header(ns,1,key);h[24]=value;f.entry_crc(h,0);return bytes(h)


def variable(ns,typ,key,payload,chunk=255):
    span=1+(len(payload)+31)//32;h=header(ns,typ,key,span,chunk)
    struct.pack_into('<H',h,24,len(payload));struct.pack_into('<I',h,28,f.crc(payload));f.entry_crc(h,0)
    return bytes(h)+payload+b'\xff'*(span*32-32-len(payload))


def blob_index(ns,key,size,count,start):
    h=header(ns,0x48,key);struct.pack_into('<I',h,24,size);h[28]=count;h[29]=start;f.entry_crc(h,0);return bytes(h)


def partition(record, timezone=None, fill=False, duplicate=False, extra=None):
    raw=bytearray(b'\xff'*20480);struct.pack_into('<II',raw,0,0xfffffffe,17);raw[8]=0xfe;struct.pack_into('<I',raw,28,f.crc(raw[4:28]))
    entries=[scalar(0,'ota_coord',1),scalar(0,'halo_prov',2),scalar(0,'private',3),
             variable(3,0x21,'preserve',b'synthetic-secret\0'),
             variable(1,0x42,'retry_v1',record[:352],8),
             variable(3,0x21,'between',b'unchanged\0'),
             variable(1,0x42,'retry_v1',record[352:],9),blob_index(1,'retry_v1',768,2,8)]
    if timezone is not None:entries.append(variable(2,0x21,'tz',timezone.encode()+b'\0'))
    if duplicate:entries.append(blob_index(1,'retry_v1',768,2,8))
    entries.extend(extra or [])
    index=0
    for data in entries:
        raw[64+index*32:64+index*32+len(data)]=data
        for _ in range(len(data)//32):
            off=32+index//4;shift=(index%4)*2;raw[off]=(raw[off]&~(3<<shift))|(2<<shift);index+=1
    if fill:
        while index<125:
            data=scalar(3,'k'+str(index),1);raw[64+index*32:96+index*32]=data
            off=32+index//4;shift=(index%4)*2;raw[off]=(raw[off]&~(3<<shift))|(2<<shift);index+=1
    return bytes(raw)


class FixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.root=Path(cls.temp.name)
        cls.codec=cls.root/'codec';cxx=shutil.which('c++')
        f.run_child([cxx,'-std=c++17','-O2',str(f.HERE/'production_policy_fixture.cpp'),'-o',str(cls.codec)])
        generator=cls.root/'generate.cpp'
        generator.write_text('#include "'+str(f.HERE.parent/'halo_ota_demo/firmware/shared/DurableOtaPolicy.h')+'"\n'+r'''
#include <cstdio>
#include <cstring>
int main(){using namespace durable_ota;Record r{};r.generation=7;r.phase=Phase::DEFERRED;r.deferred_path=true;
memset(r.campaign,1,16);strcpy(r.origin,"synthetic_fixture_source");strcpy(r.target.version,"6.4.103");strcpy(r.target.peer_version,"6.4.103");
strcpy(r.target.url,"https://halo-ota-dev.s3.us-east-1.amazonaws.com/synthetic.bin");memset(r.target.sha256,1,32);memset(r.target.peer_sha256,2,32);r.target.bytes=100;r.target.peer_bytes=101;
r.created=r.high_water=r.budget_granted=1789001000;r.budget_day=r.created/86400;r.not_before=1789002300;r.network_windows=2;r.day_attempts=2;r.fast_opportunities=2;r.begins[0]=r.begins[1]=4;
uint8_t b[768];if(!encode(r,b))return 2;return fwrite(b,1,768,stdout)==768?0:2;}
''')
        binary=cls.root/'generate';f.run_child([cxx,'-std=c++17',str(generator),'-o',str(binary)]);cls.record=f.run_child([str(binary)])

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def fixture(self, record=None, now=NOW, due=DUE, installed='6.4.102'):
        return f.run_child([str(self.codec),'exhausted-yesterday',str(now),str(due),installed],self.record if record is None else record)

    def test_exact_codec_previous_day_and_no_current_credit(self):
        new=self.fixture();d=json.loads(f.run_child([str(self.codec),'inspect'],new))
        self.assertEqual((d['generation'],d['phase'],d['budget_day'],d['work_remaining_ms'],d['reserved_work_ms']),(8,6,NOW//86400-1,0,0))
        self.assertEqual(d['not_before'],DUE);self.assertEqual(len(new),768)

    def test_dynamic_noncontiguous_multichunk_replacement(self):
        raw=partition(self.record);new=self.fixture();after,scope=f.transform(raw,new)
        self.assertEqual(f.parse(after)['values'][f.POLICY],('blob',new))
        self.assertTrue(scope['all_other_logical_keys_unchanged']);self.assertTrue(scope['only_declared_bytes_changed'])

    def test_timezone_append_preserves_all_other_keys(self):
        raw=partition(self.record);new,scope=f.transform(raw,timezone='UTC-1:35')
        a,b=f.parse(raw),f.parse(new)
        self.assertEqual(b['values'][f.TIMEZONE],('string',b'UTC-1:35\0'))
        self.assertEqual(a['values'][f.POLICY],b['values'][f.POLICY]);self.assertTrue(scope['only_declared_bytes_changed'])

    def test_timezone_replacement_and_pacific_restore(self):
        raw=partition(self.record,'UTC0');new,_=f.transform(raw,timezone='PST8PDT,M3.2.0,M11.1.0')
        self.assertEqual(f.parse(new)['values'][f.TIMEZONE],('string',b'PST8PDT,M3.2.0,M11.1.0\0'))
        after,_=f.transform(new,timezone='UTC-1:35');self.assertEqual(f.parse(after)['values'][f.TIMEZONE],('string',b'UTC-1:35\0'))

    def test_combined_policy_timezone_change(self):
        raw=partition(self.record,'UTC0');new=self.fixture();after,scope=f.transform(raw,new,'UTC-1:35')
        self.assertEqual(f.parse(after)['values'][f.POLICY],('blob',new));self.assertTrue(scope['all_other_logical_keys_unchanged'])

    def test_missing_capacity_and_bad_timezone_refused(self):
        for raw,tz in ((partition(self.record,fill=True),'UTC0'),(partition(self.record),'bad\nzone'),(partition(self.record),'x'*64)):
            with self.subTest(length=len(tz)),self.assertRaises(ValueError):f.transform(raw,timezone=tz)

    def test_ambiguous_blob_index_refused(self):
        with self.assertRaises(ValueError):f.parse(partition(self.record,duplicate=True))

    def test_corrupt_page_entry_data_and_bitmap_refused(self):
        base=partition(self.record);parsed=f.parse(base);entry=parsed['locations'][f.POLICY][0]['offset']
        for off in (28,entry+4,entry+32,32):
            raw=bytearray(base);raw[off]^=1
            with self.subTest(offset=off),self.assertRaises(ValueError):f.parse(bytes(raw))

    def test_canonical_crc_and_state_refused(self):
        raw=bytearray(self.record);raw[764]^=1
        with self.assertRaises(ValueError):self.fixture(bytes(raw))
        for phase in (3,8):
            raw=bytearray(self.record);raw[5]=phase;struct.pack_into('<I',raw,764,zlib.crc32(raw[:764])&0xffffffff)
            with self.subTest(phase=phase),self.assertRaises(ValueError):self.fixture(bytes(raw))

    def test_target_and_due_boundaries_refused(self):
        for kwargs in ({'installed':'6.4.103'},{'installed':'6.4.104'},{'installed':'nonnumeric'},{'due':NOW+60},{'due':(NOW//86400+1)*86400},{'due':0},{'now':0}):
            with self.subTest(kwargs=kwargs),self.assertRaises(ValueError):self.fixture(**kwargs)

    def test_private_preparation_backup_and_no_source_values_in_summary(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);source=root/'input.bin';source.write_bytes(partition(self.record));source.chmod(0o600)
            out=root/'prepared';r=f.prepare(source,f.sha(source.read_bytes()),out,'testcase_001','6.4.102',DUE,'UTC-0:55',True,NOW)
            self.assertEqual((out/'before-nvs.bin').read_bytes(),source.read_bytes());self.assertEqual(r['hardware_actions'],0)
            self.assertNotIn('synthetic-secret',json.dumps(r));self.assertEqual(stat.S_IMODE(out.stat().st_mode),0o700)
            for p in out.iterdir():self.assertEqual(stat.S_IMODE(p.stat().st_mode),0o600)
            self.assertTrue(all(p['reaped'] and p['group_absent'] and p['exit_code']==0 for p in r['host_children']))

    def test_calendar_due_matches_exact_production_helper(self):
        value=int(f.run_child([str(self.codec),'calendar',str(NOW),'UTC-0:55']))
        self.assertEqual(value,DUE)
        with self.assertRaises(ValueError):f.validate_timezone('not-a-timezone')
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);p=root/'input';p.write_bytes(partition(self.record));p.chmod(0o600)
            with self.assertRaises(ValueError):f.prepare(p,f.sha(p.read_bytes()),root/'out','testcase_001','6.4.102',DUE+1,'UTC-0:55',True,NOW)
            self.assertFalse((root/'out').exists())

    def test_input_sha_permissions_symlink_refused(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);p=root/'input';p.write_bytes(partition(self.record));p.chmod(0o600)
            with self.assertRaises(ValueError):f.prepare(p,'0'*64,root/'out','testcase_001','6.4.102',0,timezone='UTC0')
            p.chmod(0o644)
            with self.assertRaises(ValueError):f.prepare(p,f.sha(p.read_bytes()),root/'out','testcase_001','6.4.102',0,timezone='UTC0')
            p.chmod(0o600);link=root/'link';link.symlink_to(p)
            with self.assertRaises(ValueError):f.prepare(link,f.sha(p.read_bytes()),root/'out','testcase_001','6.4.102',0,timezone='UTC0')
            self.assertFalse((root/'out').exists())

    def test_new_case_sense_retires_all_chunks_index_and_legacy_only(self):
        extra=[scalar(0,'ota_expect',4),scalar(0,'halo',5),scalar(1,'gen_v1',9),
               scalar(4,'last_ok_ts',7),scalar(5,'boot_count',17)]
        namespaces={'ota_coord':1,'ota_expect':4,'halo':5}
        for namespace,key in sorted(f.NEW_CASE_KEYS['sense']-{f.POLICY}):
            extra.append(scalar(namespaces[namespace],key,1))
        raw=partition(self.record,extra=extra);after,scope=f.transform(raw,new_case_board='sense')
        values=f.parse(after)['values']
        self.assertFalse(f.NEW_CASE_KEYS['sense'] & set(values))
        self.assertIn(('ota_coord','gen_v1'),values);self.assertIn(('ota_expect','last_ok_ts'),values)
        self.assertTrue(scope['all_other_logical_keys_unchanged'])
        self.assertEqual(sum(a!=b for a,b in zip(raw,after)),scope['changed_bytes'])
        self.assertTrue(all(i<64 for i,(a,b) in enumerate(zip(raw,after)) if a!=b))

    def test_new_case_lcd_preserves_fail_guard_and_sense_policy(self):
        extra=[scalar(0,'lcd_durable',4),scalar(0,'lcd_maint',5),scalar(0,'ota_guard',6),scalar(6,'fail',7)]
        for namespace,key in sorted(f.NEW_CASE_KEYS['lcd']):extra.append(scalar(4 if namespace=='lcd_durable' else 5,key,1))
        raw=partition(self.record,extra=extra);after,_=f.transform(raw,new_case_board='lcd');values=f.parse(after)['values']
        self.assertFalse(f.NEW_CASE_KEYS['lcd'] & set(values));self.assertIn(('ota_guard','fail'),values)
        self.assertEqual(values[f.POLICY],('blob',self.record))
        with self.assertRaises(ValueError):f.transform(raw,self.fixture(),new_case_board='sense')
        with self.assertRaises(ValueError):f.transform(raw,new_case_board='unknown')

    def test_new_case_backup_and_explicit_absence(self):
        with tempfile.TemporaryDirectory() as name:
            root=Path(name);p=root/'input';p.write_bytes(partition(self.record));p.chmod(0o600)
            result=f.prepare(p,f.sha(p.read_bytes()),root/'out','newcase_001','6.4.102',DUE,'UTC-0:55',False,NOW,'sense')
            self.assertIsNone(result['policy']);self.assertEqual(result['archived_previous_policy']['generation'],7)
            self.assertFalse(result['factory_fresh_claim_proven']);self.assertEqual((root/'out/before-nvs.bin').read_bytes(),p.read_bytes())

    def test_host_timeout_reaps_owned_child_without_core_dumps(self):
        receipts=[]
        with self.assertRaises(subprocess.TimeoutExpired):
            f.run_child([sys.executable,'-c','import time; time.sleep(10)'],timeout=.05,receipts=receipts)
        self.assertEqual(len(receipts),1)
        self.assertTrue(receipts[0]['reaped'] and receipts[0]['group_absent'] and receipts[0]['core_dumps_disabled'])
        self.assertLess(receipts[0]['exit_code'],0)


if __name__=='__main__':unittest.main()
