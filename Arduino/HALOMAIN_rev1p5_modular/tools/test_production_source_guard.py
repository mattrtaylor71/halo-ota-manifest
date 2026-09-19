"""Exercise actual ancestry and snapshot admission in disposable Git repositories."""
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / (name + '.py'))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


guard = load('production_source_guard')
prepare_tests = load('test_prepare_production_release')
builder = load('build_ota_policy_production')


class SourceGuardTests(unittest.TestCase):
    def setUp(self):
        self.fixture = prepare_tests.ReleasePreparationTests('test_reproducible_metadata_and_scope')
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.source = self.fixture.source

    def snapshot(self):
        data = self.fixture.prepare()
        return Path(data['source_root']), data

    def test_floor_and_descendant_accepted_old_and_unrelated_refused(self):
        f = self.fixture
        self.assertEqual(guard.committed_source(self.source, f.floor)['source_commit'], f.floor)
        self.assertEqual(guard.committed_source(self.source)['minimum_source_commit'], f.floor)
        unrelated = f.run_git('commit-tree', f.run_git('rev-parse', 'HEAD^{tree}').decode().strip(),
                              '-m', 'unrelated root').decode().strip()
        for candidate in (f.old_commit, unrelated):
            with self.subTest(candidate=candidate), self.assertRaisesRegex(ValueError, 'does not descend'):
                guard.committed_source(self.source, candidate, '6.4.999')

    def test_minimum_version_refused_before_prepare_output(self):
        with self.assertRaisesRegex(ValueError, 'below minimum'):
            self.fixture.prepare(version='6.4.101')
        self.assertFalse((self.fixture.base / 'result').exists())
        self.fixture.prepare(version='6.4.102')

    def test_floor_tree_pin_and_missing_policy_fail_closed(self):
        path = self.source / guard.POLICY
        rules = dict(self.fixture.policy, minimum_source_firmware_tree='0' * 40)
        path.write_text(json.dumps(rules))
        with self.assertRaisesRegex(ValueError, 'tree mismatch'):
            guard.committed_source(self.source)
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'policy missing'):
            guard.committed_source(self.source)

    def test_exact_prepared_snapshot_uses_recorded_history(self):
        source, data = self.snapshot()
        checked = guard.materialized_source(source)
        self.assertEqual(checked['source_commit'], data['git_commit'])
        self.assertEqual(checked['source_files_verified'], len(data['source_snapshot']))

    def test_tampered_snapshot_and_self_rehashed_receipt_refused(self):
        source, data = self.snapshot()
        changed = source / 'vendor/LICENSE'
        changed.write_text('older source substituted\n')
        with self.assertRaisesRegex(ValueError, 'source changed'):
            guard.materialized_source(source)
        data['source_snapshot']['vendor/LICENSE'] = guard.sha(changed.read_bytes())
        (source.parent / 'materialization.json').write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'non-generated'):
            guard.materialized_source(source)
        data['original_source_hashes']['vendor/LICENSE'] = data['source_snapshot']['vendor/LICENSE']
        (source.parent / 'materialization.json').write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'original Git source mismatch'):
            guard.materialized_source(source)

    def test_missing_history_and_source_identity_mismatch_refused(self):
        source, data = self.snapshot()
        receipt = source.parent / 'materialization.json'
        data['original_source_root'] = str(self.fixture.base / 'missing-history')
        receipt.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'history is missing'):
            guard.materialized_source(source)
        data['source_root'] = str(self.fixture.base / 'wrong-source')
        receipt.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'source identity mismatch'):
            guard.materialized_source(source)

    def test_self_rehashed_header_runtime_change_refused_for_every_board(self):
        source, data = self.snapshot()
        receipt = source.parent / 'materialization.json'
        for name in guard.HEADERS:
            path = source / name
            original = path.read_bytes()
            for before, after in ((b'#define SAFE_MODE 0', b'#define SAFE_MODE 1'),
                                  (b'if (major < other.major) return true;',
                                   b'if (major < other.major) return false;')):
                with self.subTest(header=name, change=before):
                    self.assertIn(before, original)
                    changed = original.replace(before, after)
                    path.write_bytes(changed)
                    data['source_snapshot'][name] = guard.sha(changed)
                    receipt.write_text(json.dumps(data))
                    with self.assertRaisesRegex(ValueError, 'generated header bytes mismatch'):
                        guard.materialized_source(source)
                path.write_bytes(original)
                data['source_snapshot'][name] = guard.sha(original)
                receipt.write_text(json.dumps(data))
        self.assertEqual(guard.materialized_source(source)['source_commit'], data['git_commit'])

    def test_current_policy_cannot_be_downgraded_by_snapshot(self):
        source, data = self.snapshot()
        rules = dict(self.fixture.policy, minimum_new_version='6.4.103')
        (self.source / guard.POLICY).write_text(json.dumps(rules))
        with self.assertRaisesRegex(ValueError, 'differs from current source policy'):
            guard.materialized_source(source)

    def test_builder_refuses_unprepared_or_modified_source_before_any_output(self):
        out = self.fixture.base / 'build'
        with patch.object(builder, 'launch_compiler') as launch:
            with self.assertRaisesRegex(ValueError, 'prepared materialization'):
                builder.run('sense', self.source, out, '/unused/compiler')
            self.assertFalse(out.exists())
            launch.assert_not_called()
        source, _ = self.snapshot()
        (source / 'vendor/LICENSE').write_text('changed\n')
        with patch.object(builder, 'launch_compiler') as launch:
            with self.assertRaisesRegex(ValueError, 'source changed'):
                builder.run('sense', source, out, '/unused/compiler')
            self.assertFalse(out.exists())
            launch.assert_not_called()


if __name__ == '__main__':
    unittest.main()
