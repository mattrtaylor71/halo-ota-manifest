#!/usr/bin/env python3
"""Finite host tests for static reporting boundaries; no compiler/device/network."""
import argparse
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

import resource_report as report


def output_forbidden_roots(script):
    source = Path(script).resolve().parents[1]
    # An exported snapshot has no .git: its campaign/regression sibling is a
    # valid test-output location. Ignore unrelated ancestor repositories (for
    # example a home-directory Git repository containing the evidence folder).
    if source.name == 'HALOMAIN_rev1p5_modular' and source.parent.name == 'Arduino':
        checkout = source.parent.parent
        if (checkout/'.git').exists():
            return [checkout]
    return [source]


SECTIONS = '''There are 8 section headers:
  [ 0]                   NULL            00000000 000000 000000 00      0   0  0
  [ 1] .dram0.data       PROGBITS        3fc90000 001000 000010 00  WA  0   0 16
  [ 2] .dram0.bss        NOBITS          3fc90010 001010 018000 00  WA  0   0  8
  [ 3] .flash.rodata     PROGBITS        3c000010 002000 030000 00  WA  0   0 16
  [ 4] .dram0.dummy      NOBITS          3fc80000 003000 010000 00  WA  0   0  1
  [ 5] .ext_ram.dummy    NOBITS          3c000010 004000 030000 00  WA  0   0  1
  [ 6] .debug_info       PROGBITS        00000000 005000 700000 00      0   0  1
  [ 7] .rtc_noinit       NOBITS          50000200 006000 000008 00  WA  0   0  4
'''


class ResourceReportTests(unittest.TestCase):
    def test_sections_exclude_aliases_and_debug(self):
        sections = report.parse_readelf(SECTIONS)
        dram = sum(r['bytes'] for r in sections if r['category'] == 'dram')
        self.assertEqual(dram, 98320)
        self.assertEqual([r['category'] for r in sections[-3:]], ['dummy_alias', 'nonallocated', 'rtc_slow'])
        self.assertEqual(report.classify('.flash_rodata_dummy', 0x3C000000, 'WA'), 'dummy_alias')

    def test_flash_d_symbols_are_not_ram(self):
        symbols = '3c000010 00020000 d glyph_bitmap\n3fc90010 00018000 b work_mem_int$0\n50000200 00000008 b retained\n'
        rows = report.ram_symbols(symbols, report.parse_readelf(SECTIONS))
        self.assertEqual([(s['name'], s['bytes']) for s in rows], [('work_mem_int$0', 98304), ('retained', 8)])

    def test_symbol_must_fit_actual_section(self):
        rows = report.ram_symbols('3fc90008 00000010 d crosses_end\n', report.parse_readelf(SECTIONS))
        self.assertEqual(rows, [])

    def test_section_types_preserve_other_allocated(self):
        self.assertEqual(report.classify('.future_data', 0x3FC90000, 'WA'), 'other_allocated')
        self.assertEqual(report.classify('.rtc.text', 0x600FE000, 'AX'), 'rtc_fast')
        self.assertEqual(report.classify('.ext_ram.bss', 0x3C100000, 'WA'), 'psram_static')
        self.assertEqual(report.classify('.iram0.text_end', 0x40380000, 'WA'), 'iram')

    def test_size_parses_only_sections(self):
        self.assertEqual(report.parse_size('file.elf :\nsection size addr\n.dram0.bss 16 1070000000\nTotal 999\n'), {'.dram0.bss': (16, 1070000000)})

    def test_malformed_and_duplicate_sections_rejected(self):
        for data in ('garbage', '.data -1 100', '.data 1 100\n.data 1 100'):
            with self.subTest(data=data), self.assertRaises(ValueError):
                report.parse_size(data)
        for data in ('[ 1] broken', SECTIONS + SECTIONS):
            with self.subTest(data=data[:20]), self.assertRaises(ValueError):
                report.parse_readelf(data)
        with self.assertRaises(ValueError):
            report.ram_symbols('garbled nm record', [])

    def test_actual_partition_margin_uses_smaller_slot(self):
        table = b''.join(struct.pack('<HBBII16sI', 0x50AA, 0, subtype, start, size, name, 0)
                         for subtype,start,size,name in [(0x10,0x10000,1966080,b'app0'),(0x11,0x1F0000,1966080,b'app1')])
        slots = report.ota_slots(table + b'\xff'*32)
        self.assertEqual(min(s['bytes'] for s in slots)-1873408, 92672)
        self.assertLess(min(s['bytes'] for s in slots), 3342336)
        for invalid in (b'', b'\0'*32, table[:32]):
            with self.assertRaises(ValueError):
                report.ota_slots(invalid)

    def test_pinned_input_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp).resolve()/'proof';p.write_text('before')
            ref = report.reference(p)
            self.assertEqual(report.pin(ref), p)
            p.write_text('after')
            with self.assertRaises(ValueError):
                report.pin(ref)
        with self.assertRaises(ValueError):
            report.pin({'path':'relative','sha256':'0'*64})

    def test_existing_output_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp).resolve()/'report'
            report.reserve_output(out, [])
            marker = out/'keep';marker.write_text('original')
            with self.assertRaises(FileExistsError):
                report.reserve_output(out, [])
            self.assertEqual(marker.read_text(), 'original')

    def test_output_refuses_symlink_and_normalizes_parent(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve();inside = root/'sealed';inside.mkdir()
            outside = root/'outside';outside.mkdir()
            link = root/'link';link.symlink_to(outside, target_is_directory=True)
            for path in (inside/'new', outside/'..'/'sealed'/'new', link/'new'):
                with self.assertRaises(ValueError):
                    report.reserve_output(path, [inside])
            self.assertFalse((inside/'new').exists())

    def test_exported_snapshot_regression_output(self):
        with tempfile.TemporaryDirectory() as temp:
            campaign = Path(temp).resolve()/'campaign'
            source = campaign/'snapshot/source'
            script = source/'tools/test_resource_report.py'
            script.parent.mkdir(parents=True)
            script.touch()
            (campaign/'.git').mkdir()  # unrelated ancestor repository
            regression = campaign/'regression';regression.mkdir()
            self.assertEqual(output_forbidden_roots(script), [source])
            out = regression/'resource_report'
            self.assertEqual(report.reserve_output(out, output_forbidden_roots(script)), out)
            with self.assertRaises(ValueError):
                report.reserve_output(source/'bad-output', output_forbidden_roots(script))
            # A real firmware checkout's .git worktree marker protects the
            # whole checkout, rather than only its nested firmware source.
            checkout = campaign/'checkout'
            checkout_script = checkout/'Arduino/HALOMAIN_rev1p5_modular/tools/test_resource_report.py'
            checkout_script.parent.mkdir(parents=True)
            checkout_script.touch()
            (checkout/'.git').touch()
            self.assertEqual(output_forbidden_roots(checkout_script), [checkout])
            with self.assertRaises(ValueError):
                report.reserve_output(checkout/'inside-checkout', output_forbidden_roots(checkout_script))

    def test_comparison_and_profile_mismatch(self):
        old = dict(board='sense',version='211',build_id='old',fqbn='same',section_bytes={'dram':100},
                   application={'slot_bytes':1000,'bytes':700,'margin_bytes':300})
        new = {**old, 'version':'212', 'section_bytes':{'dram':116},
               'application':{'slot_bytes':1000,'bytes':720,'margin_bytes':280}}
        prior = {'schema':report.SCHEMA,'status':'PASS_STATIC_RESOURCE_REPORT','boards':{'sense':old}}
        delta = report.compare({'sense':new}, prior)['sense']
        self.assertEqual((delta['section_bytes_delta']['dram'],delta['application_bytes_delta'],delta['ota_margin_bytes_delta']), (16,20,-20))
        for invalid in ({**prior,'schema':999}, {**prior,'status':'FAIL'}):
            with self.assertRaises(ValueError):
                report.compare({'sense':new}, invalid)
        with self.assertRaises(ValueError):
            report.compare({'sense':{**new,'fqbn':'other'}}, prior)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', required=True, type=Path)
    a = p.parse_args()
    out = report.reserve_output(a.out, output_forbidden_roots(__file__))
    stream = io.StringIO()
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ResourceReportTests))
    (out/'tests.log').write_text(stream.getvalue())
    status = 'PASS' if result.wasSuccessful() else 'FAIL'
    (out/'RESULT.json').write_text(json.dumps({'status':status,'tests':result.testsRun,'failures':len(result.failures),'errors':len(result.errors)},indent=2)+'\n')
    print(stream.getvalue(), end='')
    print(status, result.testsRun, 'resource report tests')
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == '__main__':
    main()
