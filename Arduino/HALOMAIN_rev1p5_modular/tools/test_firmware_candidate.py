"""Exercise the development/publication boundary without network or devices."""
import hashlib
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import firmware_candidate as dev
import test_prepare_production_release as fixtures


class CandidateTests(unittest.TestCase):
    def setUp(self):
        self.f = fixtures.ReleasePreparationTests('test_reproducible_metadata_and_scope')
        self.f.setUp()
        self.addCleanup(self.f.doCleanups)
        self.out = self.f.base / 'candidate'
        checker = self.f.base / 'checker.py'
        checker.write_text('# local checker fixture\n')
        for key, value in [('CHECKER', checker), ('CHECKER_SHA', dev.reference(checker)['sha256'])]:
            guard = patch.object(dev, key, value)
            guard.start()
            self.addCleanup(guard.stop)

    def prepare(self, route='production'):
        return dev.prepare(self.f.source, self.out, '6.4.102', 1790100000, route)

    def test_plan_is_local_and_default_build_is_exact_production_route(self):
        cmds = dev.commands(self.f.source, self.out, '6.4.102', 1790100000, 'production')
        self.assertEqual(set(cmds), {'prepare', 'test', 'build', 'check_artifacts', 'verify'})
        self.assertNotIn('--private-canary', cmds['build'])
        self.assertFalse(self.out.exists())
        self.assertFalse(any('publish_pair.py' in arg for cmd in cmds.values() for arg in cmd))
        canary = dev.commands(self.f.source, self.out, '6.4.102', 1790100000, 'private-canary')
        self.assertEqual(canary['build'], cmds['build'] + ['--private-canary'])

    def test_prepare_binds_clean_source_without_changing_production(self):
        before = (self.f.source / 'PRODUCTION_BASELINE.json').read_bytes()
        with patch('urllib.request.build_opener', side_effect=AssertionError('network is forbidden')):
            result = self.prepare()
        self.assertEqual(result['production_at_preparation'], '6.4.101')
        self.assertIs(result['published'], False)
        self.assertEqual(result['device_acceptance'], 'NOT_TESTED')
        self.assertEqual(result['source_commit'], self.f.run_git('rev-parse', 'HEAD').decode().strip())
        self.assertEqual(before, (self.f.source / 'PRODUCTION_BASELINE.json').read_bytes())
        self.assertEqual(dev.reference(self.out / 'snapshot/materialization.json'), result['materialization'])
        with self.assertRaisesRegex(ValueError, 'must be new'):
            self.prepare()

    def test_old_version_bad_epoch_and_unknown_route_refused(self):
        for version, epoch, route in [('6.4.101', 1790100000, 'production'),
                                      ('6.4.102', 0, 'production'),
                                      ('6.4.102', 1790100000, 'prod-typo')]:
            with self.subTest(version=version, epoch=epoch, route=route), self.assertRaises(ValueError):
                dev.prepare(self.f.source, self.out, version, epoch, route)
            self.assertFalse(self.out.exists())

    def test_dirty_source_never_gets_candidate_record(self):
        (self.f.source / 'vendor/LICENSE').write_text('uncommitted\n')
        with self.assertRaisesRegex(ValueError, 'uncommitted'):
            self.prepare()
        self.assertFalse((self.out / 'CANDIDATE.json').exists())

    def test_in_repo_output_and_changed_checker_refused(self):
        with self.assertRaisesRegex(ValueError, 'outside Git'):
            dev.prepare(self.f.source, self.f.repo / 'candidate', '6.4.102', 1790100000, 'production')
        dev.CHECKER.write_text('# changed\n')
        with self.assertRaisesRegex(ValueError, 'checker changed'):
            self.prepare()
        self.assertFalse(self.out.exists())

    def test_known_relocated_checker_allowed_but_changed_bytes_refused(self):
        digest = dev.reference(dev.CHECKER)['sha256']
        with patch.object(dev, 'CHECKER_SHA', '0' * 64), patch.object(dev, 'MINI_CHECKER_SHA', digest):
            result = self.prepare()
            self.assertEqual(result['artifact_checker']['sha256'], digest)
            self.assertEqual(result['commands']['check_artifacts'][2], str(dev.CHECKER))
            dev.CHECKER.write_text('# altered relocated checker\n')
            with self.assertRaisesRegex(ValueError, 'checker changed'):
                dev.prepare(self.f.source, self.f.base / 'other', '6.4.102', 1790100000, 'production')

    def test_changed_materialization_and_source_refused_before_verifier_import(self):
        self.prepare()
        path = self.out / 'snapshot/materialization.json'
        raw = path.read_bytes()
        path.write_bytes(raw + b' ')
        with self.assertRaisesRegex(ValueError, 'evidence changed'):
            dev.verify(self.out)
        path.write_bytes(raw)
        (self.out / 'snapshot/source/vendor/LICENSE').write_text('tampered\n')
        with self.assertRaisesRegex(ValueError, 'source changed'):
            dev.verify(self.out)

    def production_fixture(self):
        bodies, expected = {}, {}
        for board in ('sense', 'lcd'):
            raw = json.dumps({'board': board, 'version': '6.4.211', 'sha256': board, 'size': 42}).encode()
            bodies[dev.ORIGIN + ('lcd/' if board == 'lcd' else '') + 'manifest_latest.json'] = raw
            expected[board] = {'manifest_sha256': hashlib.sha256(raw).hexdigest(), 'sha256': board, 'bytes': 42}
        path = self.f.source / 'PRODUCTION_BASELINE.json'
        policy = json.loads(path.read_text())
        policy.update(public_ota_version='6.4.211', public_readback={'boards': expected})
        path.write_text(json.dumps(policy))
        return bodies

    def test_production_check_reads_both_exact_manifests(self):
        bodies = self.production_fixture()
        calls = []
        def get(url):
            calls.append(url)
            return bodies[url]
        result = dev.production_status(self.f.source, get)
        self.assertEqual(result['status'], 'PASS_RECORDED_PRODUCTION_LATEST')
        self.assertEqual(set(calls), set(bodies))
        self.assertEqual(result['cloud_writes'], 0)

    def test_one_changed_public_manifest_is_not_a_pass(self):
        bodies = self.production_fixture()
        bodies[dev.ORIGIN + 'lcd/manifest_latest.json'] += b' '
        with self.assertRaisesRegex(ValueError, 'differs.*lcd'):
            dev.production_status(self.f.source, bodies.__getitem__)


if __name__ == '__main__':
    unittest.main()
