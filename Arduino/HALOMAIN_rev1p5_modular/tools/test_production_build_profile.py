"""Portable admission checks for the canonical shipping command; no compiler/device I/O."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import json
from types import SimpleNamespace
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('production_build', Path(__file__).with_name('build_ota_policy_production.py'))
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)
SOURCE = Path(__file__).resolve().parents[1]


class ProductionProfileTests(unittest.TestCase):
    def test_qualified_shipping_features_and_fixed_boards(self):
        for board in ('sense', 'lcd'):
            with self.subTest(board=board):
                argv = build.command(board, SOURCE, '/tmp/halo-profile-check', '/usr/bin/arduino-cli')
                flags = argv[argv.index('--build-property') + 1]
                self.assertEqual(argv[argv.index('--fqbn') + 1], build.FQBNS[board])
                self.assertEqual(argv[-2:], ['--jobs', '2'])
                for macro in ('HALO_DURABLE_DIAGNOSTICS=1', 'HALO_LCD_SLEEP_WITNESS=1',
                              'HALO_OTA_BENCH_PROFILE=0', 'HALO_OTA_ONE_SHOT=0',
                              'HALO_DIAG_AUTH_PROVISIONING=0'):
                    self.assertIn('-D' + macro, flags)
                for forbidden in ('OTA_CHANNEL', 'OTA_S3_', 'HALO_TEST_', 'INSECURE_DEBUG=1',
                                  'HALO_OTA_BENCH_PROFILE=1', 'HALO_IDLE_NETWORK_PROBE=1'):
                    self.assertNotIn(forbidden, flags)
                if board == 'sense':
                    for macro in ('HALO_DURABLE_OTA_POLICY=1', 'HALO_DIAGNOSTIC_ADMISSION=1',
                                  'HALO_IDLE_NETWORK_RECOVERY=1', 'HALO_IDLE_NETWORK_PROBE=0'):
                        self.assertIn('-D' + macro, flags)
                    self.assertIn(build.ADMISSION_URL, flags)
                else:
                    self.assertNotIn('HALO_DIAG_B1_URL', flags)
                    self.assertIn('-DHALO_UI_REVIEW=1 -DLAYOUT_AUDIT=1', flags)

    def source_shell(self, root):
        relative = Path('halo_ota_demo/firmware/halo_sense_prod/partitions.csv')
        (root / relative).parent.mkdir(parents=True)
        (root / relative).write_bytes((SOURCE / relative).read_bytes())

    def test_local_credentials_refused_before_compile(self):
        for relative in ('halo_ota_demo/firmware/shared/MqttSecrets.local.h',
                         'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.h',
                         'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.cpp'):
            with self.subTest(path=relative), tempfile.TemporaryDirectory() as name:
                root = Path(name)
                self.source_shell(root)
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('// unexpected local override\n')
                with self.assertRaisesRegex(AssertionError, 'tracked disabled-MQTT'):
                    build.command('sense', root, root / 'output', '/usr/bin/arduino-cli')

    def test_partition_change_refused_before_compile(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.source_shell(root)
            path = root / 'halo_ota_demo/firmware/halo_sense_prod/partitions.csv'
            path.write_text('unreviewed partition table\n')
            with self.assertRaisesRegex(AssertionError, 'Partition layout changed'):
                build.command('sense', root, root / 'output', '/usr/bin/arduino-cli')

    def test_unknown_board_refused(self):
        with self.assertRaises(ValueError):
            build.shipping_flags('unreviewed')

    def test_private_canary_changes_only_manifest_route(self):
        suffix = (' -DOTA_CHANNEL_ENABLED -DOTA_CHANNEL="dev"'
                  ' -DOTA_S3_BUCKET="halo-ota-dev" -DOTA_S3_REGION="us-east-1"'
                  ' -DOTA_S3_PREFIX="halo/ota/canary/production-release-20260909"')
        for board in ('sense', 'lcd'):
            plain = build.command(board, SOURCE, '/tmp/halo-profile-check', '/usr/bin/arduino-cli')
            canary = build.command(board, SOURCE, '/tmp/halo-profile-check', '/usr/bin/arduino-cli', True)
            index = plain.index('--build-property') + 1
            self.assertEqual(canary[index], plain[index] + suffix)
            canary[index] = plain[index]
            self.assertEqual(canary, plain)

    def test_pending_signal_after_spawn_retains_compiler_custody(self):
        child = SimpleNamespace(pid=123456789, poll=lambda: 0, returncode=0)
        def mask(how, signals):
            if how == build.signal.SIG_SETMASK:
                raise InterruptedError('simulated pending signal after compiler spawn')
            return set()
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / 'build'
            with patch.object(build.signal, 'pthread_sigmask', side_effect=mask), \
                    patch.object(build.subprocess, 'Popen', return_value=child), \
                    patch.object(build, 'close_owned_group', return_value={'group_absent': True, 'signals': []}) as close:
                with self.assertRaises(AssertionError):
                    build.run('sense', SOURCE, out, '/synthetic/arduino-cli')
                close.assert_called_once_with(child)
            self.assertEqual(json.loads((out / 'started.json').read_text())['pid'], child.pid)
            result = json.loads((out / 'result.json').read_text())
            self.assertTrue(result['reaped'] and result['group_absent'])
            self.assertIn('InterruptedError', result['error'])


if __name__ == '__main__':
    unittest.main()
