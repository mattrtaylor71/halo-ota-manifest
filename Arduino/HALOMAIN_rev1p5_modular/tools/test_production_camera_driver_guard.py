"""Offline byte-pin and actual-link provenance checks; never modify the SDK."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('camera_guard', Path(__file__).with_name('production_camera_driver_guard.py'))
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)


class CameraDriverGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        data = subprocess.check_output(['arduino-cli', 'config', 'get', 'directories.data'],
                                       text=True, timeout=10).strip()
        cls.installed = Path(data) / guard.SDK_RELATIVE
        cls.originals = {name: (cls.installed / entry[0]).read_bytes()
                         for name, entry in guard.PINS.items()}
        guard.pinned_files(cls.installed)

    @classmethod
    def tearDownClass(cls):
        for name, entry in guard.PINS.items():
            assert (cls.installed / entry[0]).read_bytes() == cls.originals[name], 'Test changed SDK'

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='camera-driver-guard-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.sdk = self.root / guard.SDK_RELATIVE
        for name, entry in guard.PINS.items():
            path = self.sdk / entry[0]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.originals[name])
        self.out = self.root / 'output'
        self.out.mkdir()

    def prepare(self):
        with patch.object(guard.subprocess, 'check_output', return_value=str(self.root)):
            return guard.prepare('/synthetic/arduino-cli', self.out)

    def link_fixture(self, board='sense', archive=None, symbol_archive=None,
                     discarded_only=False, wrong_member=False, zero_address=False,
                     wrapper=True, wrapper_reference=True,
                     wrapper_section='.text.__wrap_heap_caps_aligned_alloc'):
        build = self.out / 'compile'
        build.mkdir(exist_ok=True)
        archive = archive or self.sdk / guard.PINS['archive'][0]
        symbol_archive = symbol_archive or archive
        sections = []
        for symbol, member in guard.SYMBOL_MEMBERS.items():
            sections.append(' .text.%s\n                %s 0x24 %s(%s)\n' %
                            (symbol, '0x00000000' if zero_address else '0x42001234',
                             symbol_archive, 'override.o' if wrong_member else member))
        text = 'Discarded input sections\n'
        if discarded_only:
            text += ''.join(sections)
        text += '\nLinker script and memory map\nLOAD %s\n' % archive
        if not discarded_only and board == 'sense':
            text += ''.join(sections)
        if board == 'sense' and wrapper:
            obj = build / 'sketch/halo_sense_prod.ino.cpp.o'
            text += ' %s\n 0x40370000 0x60 %s\n' % (wrapper_section, obj)
            text += '                0x40370000 __wrap_heap_caps_aligned_alloc\n'
            text += 'Cross Reference Table\n__wrap_heap_caps_aligned_alloc %s\n' % obj
            text += '    %s(%s)\n' % (archive, 'cam_hal.c.obj' if wrapper_reference else 'other.c.obj')
        (build / ('halo_%s_prod.ino.map' % board)).write_text(text)
        (build / 'sketch').mkdir(exist_ok=True)
        dependencies = [str(self.sdk / guard.PINS[name][0]) for name in ('header', 'sdkconfig')]
        (build / 'sketch/halo_sense_prod.ino.cpp.d').write_text('synthetic.o: \\\n ' + ' \\\n '.join(dependencies) + '\n')
        return build

    def test_known_bytes_and_retained_api_linkage_recorded(self):
        before = self.prepare()
        build = self.link_fixture()
        after = guard.verify_compiled(before, build, 'sense')
        self.assertEqual(before['files'], after['files'])
        self.assertEqual(set(after['linked_symbols']), set(guard.SYMBOL_MEMBERS))
        self.assertEqual(after['linked_archive']['sha256'], guard.PINS['archive'][2])
        self.assertTrue(after['allocation_wrapper']['camera_reference_verified'])
        self.assertEqual(json.loads((self.out / 'sdk-camera-compiled.json').read_text()), after)

    def test_each_altered_component_refused_before_compile(self):
        for name, entry in guard.PINS.items():
            with self.subTest(component=name):
                path = self.sdk / entry[0]
                raw = self.originals[name]
                path.write_bytes(bytes([raw[0] ^ 1]) + raw[1:])
                with self.assertRaisesRegex(ValueError, 'Unreviewed'):
                    self.prepare()
                self.assertFalse((self.out / 'sdk-camera-before.json').exists())
                path.write_bytes(raw)

    def test_each_component_drift_after_compile_refused(self):
        before = self.prepare()
        build = self.link_fixture()
        for name, entry in guard.PINS.items():
            with self.subTest(component=name):
                path = self.sdk / entry[0]
                path.write_bytes(self.originals[name] + b'changed')
                with self.assertRaisesRegex(ValueError, 'Unreviewed'):
                    guard.verify_compiled(before, build, 'sense')
                self.assertFalse((self.out / 'sdk-camera-compiled.json').exists())
                path.write_bytes(self.originals[name])

    def test_different_or_missing_archive_load_refused(self):
        before = self.prepare()
        build = self.link_fixture(archive=self.root / 'other/libespressif__esp32-camera.a')
        with self.assertRaisesRegex(ValueError, 'different or missing camera archive'):
            guard.verify_compiled(before, build, 'sense')
        path = build / 'halo_sense_prod.ino.map'
        path.write_text('Linker script and memory map\n')
        with self.assertRaisesRegex(ValueError, 'different or missing camera archive'):
            guard.verify_compiled(before, build, 'sense')

    def test_discarded_zero_or_different_api_provider_refused(self):
        before = self.prepare()
        for options in ({'discarded_only': True}, {'zero_address': True}, {'wrong_member': True},
                        {'symbol_archive': self.root / 'override/libespressif__esp32-camera.a'}):
            with self.subTest(options=options):
                build = self.link_fixture(**options)
                with self.assertRaisesRegex(ValueError, 'Camera API not linked'):
                    guard.verify_compiled(before, build, 'sense')

    def test_shadow_header_or_config_refused(self):
        before = self.prepare()
        for name in ('header', 'sdkconfig'):
            with self.subTest(component=name):
                build = self.link_fixture()
                dependency = build / 'sketch/halo_sense_prod.ino.cpp.d'
                dependency.write_text(dependency.read_text().replace(str(self.sdk / guard.PINS[name][0]),
                                                                     str(self.root / Path(guard.PINS[name][0]).name)))
                with self.assertRaisesRegex(ValueError, 'different or missing camera ' + name):
                    guard.verify_compiled(before, build, 'sense')

    def test_lcd_records_archive_without_claiming_camera_api_usage(self):
        before = self.prepare()
        after = guard.verify_compiled(before, self.link_fixture(board='lcd'), 'lcd')
        self.assertFalse(after['linked_symbols'])
        self.assertNotIn('header_dependencies', after)
        self.assertEqual(after['linked_archive'], before['files']['archive'])

    def test_missing_or_bypassed_allocation_wrapper_refused(self):
        before = self.prepare()
        build = self.link_fixture(wrapper=False)
        with self.assertRaisesRegex(ValueError, 'wrapper is not retained'):
            guard.verify_compiled(before, build, 'sense')
        build = self.link_fixture(wrapper_reference=False)
        with self.assertRaisesRegex(ValueError, 'reference does not resolve to the wrapper'):
            guard.verify_compiled(before, build, 'sense')

    def test_iram_wrapper_bound_to_retained_symbol_and_sketch_object(self):
        before = self.prepare()
        build = self.link_fixture(wrapper_section='.iram1.27')
        result = guard.verify_compiled(before, build, 'sense')
        self.assertEqual(result['allocation_wrapper']['section'], '.iram1.27')
        self.assertTrue(result['allocation_wrapper']['camera_reference_verified'])

    def test_relative_sdk_directory_and_missing_map_refused(self):
        with patch.object(guard.subprocess, 'check_output', return_value='relative/data'):
            with self.assertRaisesRegex(ValueError, 'must be absolute'):
                guard.prepare('/synthetic/arduino-cli', self.out)
        before = self.prepare()
        build = self.link_fixture()
        (build / 'halo_sense_prod.ino.map').write_text('Discarded input sections only\n')
        with self.assertRaisesRegex(ValueError, 'canonical camera linker map'):
            guard.verify_compiled(before, build, 'sense')


if __name__ == '__main__':
    unittest.main()
