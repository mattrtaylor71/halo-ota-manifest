"""Exercise the real Git snapshot workflow in disposable repositories only."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('release_prepare', Path(__file__).with_name('prepare_production_release.py'))
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)
REAL_SOURCE = Path(__file__).resolve().parents[1]


class ReleasePreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / 'repo'
        self.repo.mkdir()
        self.source = self.repo / 'Arduino/HALOMAIN_rev1p5_modular'
        self.source.mkdir(parents=True)
        self.run_git('init', '-q')
        self.run_git('config', 'user.email', 'local-test@example.invalid')
        self.run_git('config', 'user.name', 'Local test')
        for name in release.HEADERS:
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((REAL_SOURCE / name).read_bytes())
        (self.source / 'vendor').mkdir()
        (self.source / 'vendor/LICENSE').write_text('local fixture license\n')
        (self.source / 'tools').mkdir()
        required = list(release.HEADERS) + ['vendor/LICENSE']
        (self.source / release.REQUIRED).write_text(json.dumps({'required_paths': required}))
        (self.repo / 'unrelated.txt').write_text('not firmware\n')
        self.commit()

    def run_git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.repo), *args], stderr=subprocess.STDOUT, timeout=15)

    def commit(self):
        self.run_git('add', '.')
        self.run_git('commit', '-qm', 'fixture')

    def prepare(self, name='result', version='6.4.102'):
        return release.prepare(self.source, self.base / name, version, 1789002000)

    def test_reproducible_metadata_and_scope(self):
        (self.repo / 'unrelated.txt').write_text('unrelated user change\n')
        one, two = self.prepare('one'), self.prepare('two')
        self.assertEqual(one['source_snapshot'], two['source_snapshot'])
        self.assertEqual(one['build_id'], two['build_id'])
        self.assertEqual(one['git_commit'], self.run_git('rev-parse', 'HEAD').decode().strip())
        self.assertNotIn('unrelated.txt', one['source_snapshot'])
        changed = {p for p, h in one['source_snapshot'].items() if h != one['original_source_hashes'][p]}
        self.assertEqual(changed, set(release.HEADERS))
        for name, board in release.HEADERS.items():
            data = (self.base / 'one/source' / name).read_text()
            self.assertIn('BOARD:' + board, data)
            self.assertIn(one['git_commit'], data)
            self.assertNotIn('precommit-', data)
        self.assertEqual(self.run_git('status', '--porcelain', '--', str(self.source)), b'')

    def test_missing_committed_dependency_refused(self):
        (self.source / 'vendor/LICENSE').unlink()
        self.commit()
        with self.assertRaisesRegex(ValueError, 'missing required'):
            self.prepare()
        self.assertFalse((self.base / 'result').exists())

    def test_dirty_or_untracked_firmware_refused(self):
        (self.source / 'vendor/LICENSE').write_text('dirty\n')
        with self.assertRaisesRegex(ValueError, 'uncommitted'):
            self.prepare()
        self.run_git('checkout', '--', str(self.source / 'vendor/LICENSE'))
        (self.source / 'untracked.cpp').write_text('// not committed\n')
        with self.assertRaisesRegex(ValueError, 'uncommitted'):
            self.prepare()

    def test_invalid_versions_and_epochs_refused(self):
        for version in ('6.4', '06.4.102', '6.4.102-fault', '6.4.65536', '6.4.102\n'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.prepare(version=version)
        for epoch in (0, True, 4294967296):
            with self.subTest(epoch=epoch), self.assertRaises(ValueError):
                release.prepare(self.source, self.base / 'result', '6.4.102', epoch)

    def test_existing_or_in_repo_output_refused(self):
        with self.assertRaisesRegex(ValueError, 'outside Git'):
            release.prepare(self.source, self.repo / 'build', '6.4.102', 1789002000)
        self.prepare()
        with self.assertRaisesRegex(ValueError, 'new directory'):
            self.prepare()


if __name__ == '__main__':
    unittest.main()
