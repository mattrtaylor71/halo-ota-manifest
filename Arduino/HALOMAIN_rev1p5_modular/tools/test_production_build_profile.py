"""Portable admission checks for the canonical shipping command; no compiler/device I/O."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import json
import shutil
import subprocess
import sys
import contextlib
import io
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
                self.assertIn('-DOTA_CHANNEL="prod"', flags)
                for forbidden in ('OTA_CHANNEL_ENABLED', 'OTA_S3_', 'HALO_TEST_', 'INSECURE_DEBUG=1',
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
            report_label = ' -DOTA_CHANNEL="prod"'
            self.assertEqual(plain[index].count(report_label), 1)
            self.assertEqual(canary[index], plain[index].replace(report_label, suffix))
            config = ' -DLV_CONF_PATH=' + str(SOURCE / 'LCD_Minimal/lv_conf.h')
            self.assertEqual(config in plain[index], board == 'lcd')
            self.assertEqual(config in canary[index], board == 'lcd')
            canary[index] = plain[index]
            self.assertEqual(canary, plain)

    def test_report_label_does_not_change_actual_default_resolver(self):
        # Preprocess the actual defaults/resolver, rather than a modeled route.
        source = (SOURCE / 'halo_ota_demo/firmware/halo_sense_prod/halo_sense_prod.ino').read_text()
        defaults = source[source.index('// OTA configuration defaults'):source.index('#ifndef OTA_SCHED_HTTP_URL')]
        begin = source.index('void resolveOtaManifestUrl(OtaUrlConfig* config) {')
        end = source.index('// ----------------------------------------------------------------------------', begin)
        snippet = (defaults + source[begin:end]).encode()
        compiler = shutil.which('c++')
        self.assertIsNotNone(compiler)
        def preprocess(extra, text):
            return subprocess.check_output([compiler, '-E', '-P', '-x', 'c++', *extra, '-'], input=text, timeout=15)
        old = preprocess([], snippet)
        current = preprocess(['-DOTA_CHANNEL="prod"'], snippet)
        self.assertEqual(current, old)
        self.assertIn(b'"default_env"', current)
        self.assertNotIn(b'"channel_mode"', current)
        payload = (defaults + 'payload["channel"] = OTA_CHANNEL;\n').encode()
        self.assertIn(b'payload["channel"] = "dev";', preprocess([], payload))
        self.assertIn(b'payload["channel"] = "prod";', preprocess(['-DOTA_CHANNEL="prod"'], payload))

    def test_disk_reserve_default_and_explicit_cli(self):
        for extra, expected in (([], 8), (['--min-free-gib', '4'], 4)):
            with patch.object(sys, 'argv', ['builder', '--out', '/tmp/unused-build', *extra]), \
                    patch.object(build.shutil, 'which', return_value='/synthetic/arduino-cli'), \
                    patch.object(build, 'run') as run:
                build.main()
                self.assertEqual(len(run.call_args_list), 2)
                self.assertTrue(all(call.args[-1] == expected for call in run.call_args_list))
        for value in ('0', '3', '-1', 'nan', '4.5', '1025'):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()), \
                    patch.object(sys, 'argv', ['builder', '--out', '/tmp/unused-build', '--min-free-gib', value]), \
                    patch.object(build, 'run') as run:
                with self.assertRaises(SystemExit):
                    build.main()
                run.assert_not_called()

    def test_disk_reserve_rechecked_between_boards_and_recorded(self):
        child = SimpleNamespace(pid=123456789, poll=lambda: 0, wait=lambda timeout: 0)
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / 'build'
            with patch.object(sys, 'argv', ['builder', '--out', str(out), '--min-free-gib', '4']), \
                    patch.object(build.shutil, 'which', return_value='/synthetic/arduino-cli'), \
                    patch.object(build.shutil, 'disk_usage', side_effect=[SimpleNamespace(free=n*1024**3) for n in (5, 3, 3)]), \
                    patch.object(build.sdk_patch, 'prepare', return_value={}), \
                    patch.object(build.sdk_patch, 'verify_compiled'), \
                    patch.object(build.subprocess, 'Popen', return_value=child) as popen, \
                    patch.object(build, 'close_owned_group', return_value={'group_absent': True, 'signals': []}):
                with self.assertRaisesRegex(AssertionError, 'host build reserve'):
                    build.main()
                self.assertEqual(popen.call_count, 1)
            sense = json.loads((out / 'sense/disk-space.json').read_text())
            lcd = json.loads((out / 'lcd/disk-space.json').read_text())
            self.assertEqual(sense['minimum_free_bytes'], 4*1024**3)
            self.assertEqual(sense['before_free_bytes'], 5*1024**3)
            self.assertEqual(sense['after_free_bytes'], 3*1024**3)
            self.assertTrue(sense['admitted'])
            self.assertFalse(lcd['admitted'])
            self.assertEqual(lcd['before_free_bytes'], 3*1024**3)
            self.assertIsNone(lcd['after_free_bytes'])
            self.assertFalse((out / 'lcd/started.json').exists())

    def test_pending_signal_after_spawn_retains_compiler_custody(self):
        child = SimpleNamespace(pid=123456789, poll=lambda: 0, returncode=0)
        def mask(how, signals):
            if how == build.signal.SIG_SETMASK:
                raise InterruptedError('simulated pending signal after compiler spawn')
            return set()
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / 'build'
            with patch.object(build.shutil, 'disk_usage', return_value=SimpleNamespace(free=9*1024**3)), \
                    patch.object(build.sdk_patch, 'prepare', return_value={}), \
                    patch.object(build.signal, 'pthread_sigmask', side_effect=mask), \
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
