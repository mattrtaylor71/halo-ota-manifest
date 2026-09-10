"""Offline release identity, conditional publication and process-custody tests."""
import copy
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import tempfile
from types import SimpleNamespace
import unittest
import time
from unittest.mock import patch

import publish_pair as p
import release_s3_guard as guard


class FakeStore:
    def __init__(self, plan, out, staged=False):
        self.out, self.actions = Path(out), []
        self.out.mkdir()
        self.data = {v['key']: p.pin(v['file']).read_bytes() for v in plan['previous_latest'].values() if v['file']}
        if staged:
            self.data.update({v['key']: p.pin(v['file']).read_bytes() for v in plan['objects']})
        self.fail_put = None
        self.fail_get_after_put = None
    def get(self, key, bucket=None):
        self.actions.append(('get', key))
        if key == self.fail_get_after_put and any(a[0] == 'put' and a[1] == key for a in self.actions):
            raise RuntimeError('Synthetic failed verification after accepted PUT')
        return self.data[key], '"' + p.sha(self.data[key]) + '"'
    def put(self, key, file, etag=None):
        self.actions.append(('put', key, etag))
        if key.endswith('/manifest_latest.json'):
            board = 'lcd' if '/lcd/' in key else 'sense'
            assert (self.out / ('promotion-attempt-' + board + '.json')).exists()
        if self.fail_put == key:
            raise RuntimeError('Synthetic uncertain PUT')
        if etag is None:
            assert key not in self.data
        else:
            assert etag == '"' + p.sha(self.data[key]) + '"'
        self.data[key] = p.pin(file).read_bytes()
    def version_absent(self, key):
        self.actions.append(('absent', key))
        p.require(key not in self.data, 'Existing version')


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.proofs = {}
        self.old = {}
        for board in ('sense', 'lcd'):
            out = self.root / board; out.mkdir()
            artifact = out / 'image.bin'
            artifact.write_bytes(('HALO_FW_MARKER:6.4.104|BUILD_ID:synthetic-offline|BOARD:' + board + '\0').encode())
            configuration = p.save(out / 'command.json', {'argv': p.canonical.command(board, p.BUILDER.parent.parent, out / 'compile', '/synthetic/arduino-cli')})
            generic = out / 'synthetic-provenance'; generic.write_bytes(b'explicitly synthetic offline fixture')
            partition = p.BUILDER.parent.parent / ('halo_ota_demo/firmware/halo_' + board + '_prod/partitions.csv')
            expected = {'HALO_DURABLE_DIAGNOSTICS': '1', 'HALO_LCD_SLEEP_WITNESS': '1', 'HALO_OTA_ONE_SHOT': '0', 'HALO_OTA_BENCH_PROFILE': '0', 'HALO_DIAG_AUTH_PROVISIONING': '0'}
            if board == 'sense':
                expected.update(HALO_DURABLE_OTA_POLICY='1', HALO_DIAGNOSTIC_ADMISSION='1', HALO_IDLE_NETWORK_RECOVERY='1', HALO_IDLE_NETWORK_PROBE='0')
            flags = p.save(out / 'flags.json', {'status': 'PASS_ACTUAL_PRODUCTION_FLAGS', 'expected': expected, 'command': configuration,
                           'fixture_override': False, 'private_route_override': False, 'one_shot': False, 'bench_profile': False,
                           'build_options': p.ref(generic), 'response_files': {str(generic): p.ref(generic)['sha256']}})
            compiled = p.save(out / 'result.json', {'exit_code': 0, 'reaped': True, 'group_absent': True, 'error': None})
            proof = {'status': 'PASS_LOCAL_CANONICAL_PRODUCTION_ARTIFACTS', 'board': board, 'profile': 'shipping', 'version': '6.4.104',
                     'build_id': 'synthetic-offline', 'slot_bytes': p.SLOTS[board], 'size': artifact.stat().st_size,
                     'bench_profile': False, 'one_shot': False, 'private_route_override': False,
                     'source_root': str(p.BUILDER.parent.parent), 'artifact': p.ref(artifact), 'elf': p.ref(generic),
                     'partition': p.ref(partition), 'configuration': configuration, 'flags': flags, 'source_manifest': p.ref(generic), 'compiler_result': compiled}
            self.proofs[board] = p.save(out / 'verified.json', proof)
            self.old[board] = p.save(out / 'old.json', {'board': board, 'version': '6.4.14'})
        self.plan_ref = p.prepare('6.4.104', self.proofs['sense']['path'], self.proofs['lcd']['path'],
                                  self.old['sense']['path'], self.old['lcd']['path'], self.root / 'package')
        self.plan = p.load_plan(self.plan_ref['path'], self.plan_ref['sha256'])

    def mutate_proof(self, change):
        proof = json.loads(p.pin(self.proofs['sense']).read_text()); change(proof)
        file = self.root / 'mutated.json'; p.save(file, proof)
        return file

    def test_exact_packaged_bytes_and_no_compiler(self):
        self.assertEqual(self.plan['compiler_actions'], 0)
        for obj in self.plan['objects']:
            if obj['role'] == 'artifact':
                self.assertEqual(obj['file']['sha256'], json.loads(p.pin(obj['proof']).read_text())['artifact']['sha256'])

    def test_altered_package_refused(self):
        p.pin(self.plan['objects'][0]['file']).write_bytes(b'changed')
        with self.assertRaises(ValueError): p.load_plan(self.plan_ref['path'], self.plan_ref['sha256'])

    def test_wrong_plan_hash_refused(self):
        with self.assertRaises(ValueError): p.load_plan(self.plan_ref['path'], '0' * 64)

    def test_profile_and_slot_refusals(self):
        for field, value in [('profile', 'bench'), ('bench_profile', True), ('one_shot', True), ('private_route_override', True), ('version', '6.4.103'), ('board', 'lcd'), ('slot_bytes', p.SLOTS['sense'] + 1), ('provisional_precommit_only', True), ('publishable', False)]:
            with self.subTest(field=field):
                file = self.root / ('bad-' + field + '.json')
                proof = json.loads(p.pin(self.proofs['sense']).read_text()); proof[field] = value; p.save(file, proof)
                with self.assertRaises(ValueError): p.shipping_proof(file, 'sense', '6.4.104')

    def test_stale_false_route_cannot_hide_actual_override(self):
        proof = json.loads(p.pin(self.proofs['sense']).read_text())
        command = json.loads(p.pin(proof['configuration']).read_text())
        command['argv'][command['argv'].index('--build-property') + 1] += ' -DOTA_CHANNEL="dev"'
        proof['configuration'] = p.save(self.root / 'bad-command.json', command)
        flags = json.loads(p.pin(proof['flags']).read_text()); flags['command'] = proof['configuration']
        proof['flags'] = p.save(self.root / 'bad-flags.json', flags)
        file = self.root / 'stale-profile.json'; p.save(file, proof)
        with self.assertRaisesRegex(ValueError, 'canonical'): p.shipping_proof(file, 'sense', '6.4.104')

    def test_missing_recovery_admission_flag_refused(self):
        proof = json.loads(p.pin(self.proofs['sense']).read_text())
        flags = json.loads(p.pin(proof['flags']).read_text()); del flags['expected']['HALO_IDLE_NETWORK_RECOVERY']
        proof['flags'] = p.save(self.root / 'incomplete-flags.json', flags)
        file = self.root / 'incomplete-proof.json'; p.save(file, proof)
        with self.assertRaises(ValueError): p.shipping_proof(file, 'sense', '6.4.104')

    def test_production_requires_predecessors(self):
        with self.assertRaises(ValueError):
            p.prepare('6.4.104', self.proofs['sense']['path'], self.proofs['lcd']['path'], None, None, self.root / 'no-baseline')
        self.assertFalse((self.root / 'no-baseline').exists())

    def test_stage_is_four_immutable_writes_no_latest(self):
        store = FakeStore(self.plan, self.root / 'stage')
        result = p.execute(self.plan, store, 'stage')
        writes = [a for a in store.actions if a[0] == 'put']
        self.assertEqual(len(writes), 4)
        self.assertTrue(all(not a[1].endswith('/manifest_latest.json') and a[2] is None for a in writes))
        self.assertEqual(result['latest_writes'], 0)

    def test_changed_predecessor_prevents_every_write(self):
        store = FakeStore(self.plan, self.root / 'stage')
        store.data[self.plan['previous_latest']['lcd']['key']] = b'newer deployment'
        with self.assertRaises(ValueError): p.execute(self.plan, store, 'stage')
        self.assertFalse(any(a[0] == 'put' for a in store.actions))

    def test_lcd_version_collision_prevents_every_stage_write(self):
        store = FakeStore(self.plan, self.root / 'stage')
        store.data[self.plan['objects'][3]['key']] = b'exists'
        with self.assertRaises(ValueError): p.execute(self.plan, store, 'stage')
        self.assertFalse(any(a[0] == 'put' for a in store.actions))

    def test_promotion_requires_all_four_served_bytes(self):
        store = FakeStore(self.plan, self.root / 'promote', staged=True)
        store.data[self.plan['objects'][2]['key']] = b'corrupt served LCD'
        with self.assertRaises(ValueError): p.execute(self.plan, store, 'promote')
        self.assertFalse(any(a[0] == 'put' for a in store.actions))

    def test_promotion_lcd_first_exact_etags(self):
        store = FakeStore(self.plan, self.root / 'promote', staged=True)
        result = p.execute(self.plan, store, 'promote')
        self.assertEqual(result['promoted'], ['lcd', 'sense'])
        self.assertEqual([a[1] for a in store.actions if a[0] == 'put'], [self.plan['previous_latest'][b]['key'] for b in ('lcd', 'sense')])
        for board in ('lcd', 'sense'):
            self.assertTrue((store.out / ('promoted-' + board + '.json')).exists())

    def test_uncertain_sense_promotion_never_retries_or_rolls_back(self):
        store = FakeStore(self.plan, self.root / 'promote', staged=True)
        store.fail_put = self.plan['previous_latest']['sense']['key']
        with self.assertRaises(RuntimeError): p.execute(self.plan, store, 'promote')
        self.assertEqual(len([a for a in store.actions if a[0] == 'put']), 2)
        self.assertTrue((store.out / 'promoted-lcd.json').exists())
        self.assertTrue((store.out / 'promotion-attempt-sense.json').exists())
        self.assertFalse((store.out / 'promoted-sense.json').exists())

    def test_accepted_put_failed_get_retains_unknown_attempt(self):
        store = FakeStore(self.plan, self.root / 'promote', staged=True)
        store.fail_get_after_put = self.plan['previous_latest']['lcd']['key']
        with self.assertRaises(RuntimeError): p.execute(self.plan, store, 'promote')
        self.assertEqual(len([a for a in store.actions if a[0] == 'put']), 1)
        self.assertTrue((store.out / 'promotion-attempt-lcd.json').exists())
        self.assertFalse((store.out / 'promoted-lcd.json').exists())

    def canary_proofs(self):
        proofs = {}
        for board in ('sense', 'lcd'):
            proof = json.loads(p.pin(self.proofs[board]).read_text())
            command = json.loads(p.pin(proof['configuration']).read_text())
            command['argv'] = p.canonical.command(board, p.BUILDER.parent.parent, self.root / board / 'compile', '/synthetic/arduino-cli', True)
            proof['configuration'] = p.save(self.root / (board + '-canary-command.json'), command)
            flags = json.loads(p.pin(proof['flags']).read_text()); flags.update(command=proof['configuration'], private_route_override=True)
            proof['flags'] = p.save(self.root / (board + '-canary-flags.json'), flags); proof['private_route_override'] = True
            proofs[board] = p.save(self.root / (board + '-canary-proof.json'), proof)
        return proofs

    def test_fresh_canary_requires_actual_absence_and_shipping_flags(self):
        proofs = self.canary_proofs()
        plan_ref = p.prepare('6.4.104', proofs['sense']['path'], proofs['lcd']['path'], None, None, self.root / 'canary-package', 'private-canary')
        plan = p.load_plan(plan_ref['path'], plan_ref['sha256'])
        store = FakeStore(plan, self.root / 'canary-stage')
        p.execute(plan, store, 'stage')
        self.assertTrue(all(a[1].startswith(p.DESTINATIONS['private-canary'][1]) for a in store.actions))
        p.execute(plan, store, 'promote')
        self.assertEqual([a[2] for a in store.actions if a[0] == 'put' and a[1].endswith('/manifest_latest.json')], [None, None])

    def bridge_plan(self):
        old = {}
        for board in ('sense', 'lcd'):
            old[board] = p.save(self.root / (board + '-canary103.json'), {'version': '6.4.103', 'board': board})
        r = p.prepare_bridge(self.plan_ref['path'], self.plan_ref['sha256'], old['sense']['path'], old['lcd']['path'], self.root / 'bridge-package')
        return p.load_plan(r['path'], r['sha256'])

    def test_bridge_stages_only_two_byte_identical_manifests(self):
        plan = self.bridge_plan()
        store = FakeStore(plan, self.root / 'bridge-stage')
        store.data.update({o['key']: p.pin(o['file']).read_bytes() for o in self.plan['objects']})
        result = p.execute(plan, store, 'stage')
        writes = [a for a in store.actions if a[0] == 'put']
        self.assertEqual(result['binary_writes'], 0); self.assertEqual(len(writes), 2)
        self.assertTrue(all(a[1].endswith('/manifest_6.4.104.json') for a in writes))
        for original, bridged in zip(self.plan['objects'], plan['objects']):
            self.assertEqual(original['file']['sha256'], bridged['file']['sha256'])
        self.assertTrue((store.out / 'bridge-production-verified.json').exists())

    def test_bridge_missing_production_versioned_manifest_blocks_all_writes(self):
        plan = self.bridge_plan()
        store = FakeStore(plan, self.root / 'bridge-stage')
        store.data.update({o['key']: p.pin(o['file']).read_bytes() for o in self.plan['objects'] if o['role'] == 'artifact'})
        with self.assertRaises(KeyError): p.execute(plan, store, 'stage')
        self.assertFalse(any(a[0] == 'put' for a in store.actions))

    def test_bridge_cannot_change_url_or_manifest_whitespace(self):
        plan = self.bridge_plan()
        manifest = plan['objects'][1]
        file = self.root / 'changed-manifest.json'; file.write_bytes(p.pin(manifest['file']).read_bytes() + b'\n')
        manifest['file'] = p.ref(file)
        changed = p.save(self.root / 'changed-bridge.json', plan)
        with self.assertRaisesRegex(ValueError, 'byte-identical'): p.load_plan(changed['path'], changed['sha256'])


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
    def make(self, bucket='halo-ota-prod', key='halo/ota/prod/manifest_latest.json', condition='--if-match', value='"old"'):
        args = ['--region', 'us-east-1', 's3api', 'put-object', '--bucket', bucket, '--key', key, condition, value]
        return guard.OneS3Send(args, self.root / ('audit-' + str(len(list(self.root.iterdir()))))), args
    def request(self, host='halo-ota-prod.s3.us-east-1.amazonaws.com', key='halo/ota/prod/manifest_latest.json'):
        return SimpleNamespace(url='https://' + host + '/' + key, method='PUT', headers={'If-Match': b'"old"'})
    def test_second_s3_send_refused(self):
        g, _ = self.make(); request = self.request(); g(request)
        with self.assertRaises(ValueError): g(request)
        self.assertEqual(g.allowed, 1)
    def test_wrong_endpoint_key_method_or_condition_never_sent(self):
        for change in [('url', 'https://example.com/halo/ota/prod/manifest_latest.json'), ('url', 'https://halo-ota-prod.s3.us-east-1.amazonaws.com/other'), ('method', 'POST'), ('headers', {})]:
            with self.subTest(change=change):
                g, _ = self.make(); request = self.request(); setattr(request, *change)
                with self.assertRaises(ValueError): g(request)
                self.assertEqual(g.allowed, 0)
    def test_immutable_requires_none_match(self):
        with self.assertRaises(ValueError): self.make(key='halo/ota/prod/artifacts/a.bin')
        g, _ = self.make(key='halo/ota/prod/artifacts/a.bin', condition='--if-none-match', value='*')
        request = self.request(key='halo/ota/prod/artifacts/a.bin'); request.headers = {'If-None-Match': '*'}
        g(request); self.assertEqual(g.allowed, 1)
    def test_canary_limited_to_selected_prefix(self):
        key = 'halo/ota/canary/production-release-20260909/dev/manifest_latest.json'
        g, _ = self.make(bucket='halo-ota-dev', key=key)
        g(self.request(host='halo-ota-dev.s3.us-east-1.amazonaws.com', key=key))
        with self.assertRaises(ValueError): self.make(bucket='halo-ota-dev', key='halo/ota/bench/unrelated/manifest_latest.json')
    def test_scoped_list_one_send(self):
        prefix = 'halo/ota/prod/manifest_6.4.104.json'
        g = guard.OneS3Send(['--region', 'us-east-1', 's3api', 'list-objects-v2', '--bucket', 'halo-ota-prod', '--prefix', prefix], self.root / 'list')
        g(SimpleNamespace(url='https://halo-ota-prod.s3.us-east-1.amazonaws.com/?list-type=2&prefix=' + prefix, method='GET', headers={}))
        self.assertEqual(g.allowed, 1)


class CustodyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
    def test_exited_leader_descendant_closed(self):
        child = SimpleNamespace(pid=123456789, poll=lambda: 0)
        present = [True]
        def killpg(pid, sig):
            self.assertEqual(pid, child.pid)
            if not present[0]: raise ProcessLookupError()
            if sig == signal.SIGTERM: present[0] = False
        with patch.object(p.os, 'killpg', side_effect=killpg) as calls:
            receipt = p.close_owned_group(child)
        self.assertTrue(receipt['group_absent'])
        self.assertEqual(receipt['cleanup_signals'], [signal.SIGTERM])

    def test_permission_error_never_claims_group_absence(self):
        child = SimpleNamespace(pid=123456789, poll=lambda: 0)
        with patch.object(p.os, 'killpg', side_effect=PermissionError()), patch.object(p.time, 'monotonic', side_effect=range(100)):
            receipt = p.close_owned_group(child)
        self.assertFalse(receipt['group_absent'])
        self.assertIn('group_probe_permission', receipt['cleanup_errors'])
    def test_spawn_failure_records_closure_without_retry(self):
        store = p.Store(self.root, 'synthetic', sys.executable)
        with self.assertRaises(FileNotFoundError): store.command(['/does-not-exist'], 'local-refusal')
        self.assertEqual(store.sequence, 1)
        receipt = json.loads((self.root / '001-local-refusal-reap.json').read_text())
        self.assertEqual(receipt['error_type'], 'FileNotFoundError')

    def test_pending_signal_after_spawn_keeps_child_custody(self):
        store = p.Store(self.root, 'synthetic', sys.executable)
        child = SimpleNamespace(pid=123456789, poll=lambda: 0, returncode=0)
        def mask(how, signals):
            if how == signal.SIG_SETMASK: raise InterruptedError('synthetic signal after spawn')
            return set()
        with patch.object(p.signal, 'pthread_sigmask', side_effect=mask), patch.object(p.subprocess, 'Popen', return_value=child), patch.object(p, 'close_owned_group', return_value={'group_absent': True}) as close:
            with self.assertRaises(InterruptedError): store.command(['synthetic'], 'spawn-signal')
            close.assert_called_once_with(child)
        receipt = json.loads((self.root / '001-spawn-signal-reap.json').read_text())
        self.assertEqual(receipt['pid'], child.pid)
        self.assertTrue(receipt['reaped'] and receipt['group_absent'])
    def test_cleanup_reserve_refuses_before_spawn(self):
        store = p.Store(self.root, 'synthetic', sys.executable); store.deadline = p.time.monotonic() + 4.9
        with patch.object(p.subprocess, 'Popen') as spawn:
            with self.assertRaises(ValueError): store.command([sys.executable, '-c', 'pass'], 'refused')
            spawn.assert_not_called()

    def test_owner_sigterm_closes_owned_child(self):
        out = self.root / 'signal'; out.mkdir()
        marker = self.root / 'child-started'
        code = '''import sys,signal
from pathlib import Path
sys.path.insert(0,sys.argv[1])
import publish_pair as p
def stop(sig,frame): raise InterruptedError('local test interruption')
signal.signal(signal.SIGTERM,stop)
store=p.Store(sys.argv[2],'synthetic',sys.executable)
child="from pathlib import Path;import time;Path(%r).touch();time.sleep(30)" % sys.argv[3]
try: store.command([sys.executable,'-c',child],'signal-child')
except InterruptedError: pass
'''
        owner = subprocess.Popen([sys.executable, '-c', code, str(Path(p.__file__).parent), str(out), str(marker)], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            deadline = time.monotonic() + 3
            while not marker.exists() and time.monotonic() < deadline: time.sleep(.02)
            self.assertTrue(marker.exists())
            owner.send_signal(signal.SIGTERM)
            stdout, stderr = owner.communicate(timeout=6)
            self.assertEqual(owner.returncode, 0, stderr.decode())
            receipt = json.loads((out / '001-signal-child-reap.json').read_text())
            self.assertTrue(receipt['reaped'] and receipt['group_absent'])
            self.assertEqual(receipt['error_type'], 'InterruptedError')
        finally:
            if owner.poll() is None:
                owner.kill(); owner.wait(timeout=2)


if __name__ == '__main__':
    unittest.main()
