#!/usr/bin/env python3
"""Offline static ESP32-S3 resource report from canonical verified.json files.

Uses pinned build nm/size logs and bounded local size/readelf commands. No
compiler, network or device access. --out must be a new directory outside the
checkout and supplied release workspaces. This is not a release validator.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess

SCHEMA = 1
RAM = ('dram', 'iram', 'rtc_fast', 'rtc_slow', 'psram_static')
LIMITS = [
    'Dynamic heap/DMA free, minimum and largest block are unknown; no runtime measurement was performed.',
    'Task stack allocation, high-water use and whole-call-chain peaks are unknown.',
    'Section totals are separate address-space observations, not an available-memory budget; do not add dummy aliases or debug data.',
    'Static PSRAM sections do not measure dynamic PSRAM allocation or prove physical PSRAM availability.',
    'SDK configuration contains compiled defaults; application initialization and scoped allocation may override runtime behavior.',
    'Symbols may alias or leave padding unnamed; section totals, not symbol sums, are authoritative.',
]


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def regular(path):
    path = Path(path).absolute()
    require(path.is_file() and not any(p.is_symlink() for p in (path, *path.parents)), 'Missing regular, nonsymlink input: ' + str(path))
    return path


def reference(path):
    p = regular(path)
    return {'path': str(p), 'sha256': sha(p), 'bytes': p.stat().st_size}


def pin(value):
    require(isinstance(value, dict) and isinstance(value.get('path'), str) and Path(value['path']).is_absolute(), 'Malformed pinned input')
    p = regular(value['path'])
    require(sha(p) == value.get('sha256'), 'Input hash mismatch: ' + str(p))
    return p


def load(path):
    return json.loads(regular(path).read_text())


def parse_size(text):
    sections = {}
    for line in text.splitlines():
        if not line.startswith('.'):
            continue
        match = re.fullmatch(r'(\S+)\s+(\d+)\s+(\d+)\s*', line)
        require(match is not None, 'Malformed size section: ' + line)
        name, size, address = match.groups()
        require(name not in sections, 'Duplicate size section: ' + name)
        sections[name] = (int(size), int(address))
    require(sections, 'No size sections')
    return sections


def classify(name, address, flags):
    if name.endswith('.dummy') or name == '.flash_rodata_dummy':
        return 'dummy_alias'
    if 'A' not in flags:
        return 'nonallocated'
    if name.startswith('.dram') or name == '.noinit':
        return 'dram'
    if name.startswith('.iram'):
        return 'iram'
    if name.startswith(('.rtc.', '.rtc_', '.rtc_reserved')):
        if 0x50000000 <= address < 0x50002000:
            return 'rtc_slow'
        if 0x600FE000 <= address < 0x60100000:
            return 'rtc_fast'
        return 'other_allocated'
    if name.startswith('.ext_ram.'):
        return 'psram_static'
    if name.startswith('.flash.'):
        return 'flash_mapped'
    return 'other_allocated'


def parse_readelf(text):
    rows = []
    pattern = r'^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s*(.*?)\s+\d+\s+\d+\s+\d+\s*$'
    for line in text.splitlines():
        if not re.match(r'^\s*\[\s*\d+\]', line) or re.match(r'^\s*\[\s*0\]', line):
            continue
        match = re.fullmatch(pattern, line)
        require(match is not None, 'Malformed readelf section: ' + line)
        _, name, kind, address, size, flags = match.groups()
        require(not any(r['name'] == name for r in rows), 'Duplicate ELF section')
        address, size = int(address, 16), int(size, 16)
        rows.append(dict(name=name, type=kind, address=address, bytes=size, flags=flags,
                         category=classify(name, address, flags)))
    require(rows, 'No ELF sections')
    return rows


def ram_symbols(text, sections, limit=20):
    result = []
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r'([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+(\S)\s+(.+)', line)
        require(match is not None, 'Malformed size-sorted nm line')
        address, size, kind, name = match.groups()
        if kind not in 'bBdDrRsSvVgG':
            continue
        address, size = int(address, 16), int(size, 16)
        matches = [r for r in sections if r['category'] in RAM and size > 0 and
                   r['address'] <= address and address + size <= r['address'] + r['bytes']]
        require(len(matches) <= 1, 'Ambiguous RAM symbol section: ' + name)
        if matches:
            row = matches[0]
            result.append(dict(name=name, bytes=size, address=address, nm_type=kind,
                               section=row['name'], category=row['category']))
    return sorted(result, key=lambda r: (-r['bytes'], r['name'], r['address']))[:limit]


def ota_slots(data):
    slots = []
    for offset in range(0, len(data) - 31, 32):
        magic, kind, subtype, start, size, name, _ = struct.unpack('<HBBII16sI', data[offset:offset + 32])
        if magic in (0xFFFF, 0xEBEB):
            break
        require(magic == 0x50AA, 'Malformed partition table')
        if kind == 0 and subtype in (0x10, 0x11):
            require(size > 0, 'Empty OTA slot')
            slots.append(dict(label=name.split(b'\0')[0].decode('ascii'), subtype=subtype, offset=start, bytes=size))
    require({r['subtype'] for r in slots} == {0x10, 0x11} and len(slots) == 2, 'Expected two OTA application slots')
    return slots


def reserve_output(path, forbidden):
    path = Path(path).absolute()
    require(not any(p.is_symlink() for p in (path, *path.parents)), 'Symlink output path')
    path = path.resolve()
    require(not any(path.is_relative_to(Path(p).resolve()) for p in forbidden), 'Output must be outside checkout and input release workspaces')
    require(path.parent.is_dir(), 'Output parent must already exist')
    path.mkdir(exist_ok=False)
    return path


def run_tool(tool, arguments, out, name):
    log, err = out / (name + '.log'), out / (name + '.stderr')
    before = reference(tool)
    with log.open('x') as stdout, err.open('x') as stderr:
        result = subprocess.run([str(tool), *arguments], stdout=stdout, stderr=stderr,
                                timeout=30, check=False, env={**os.environ, 'LC_ALL': 'C'})
    require(result.returncode == 0, 'Artifact utility failed; see ' + str(err))
    require(reference(tool) == before, 'Artifact utility changed during execution')
    return log.read_text(), dict(tool=before, argv=[str(tool), *arguments], exit_code=0,
                                 stdout=reference(log), stderr=reference(err))


def inspect_board(path, board, out):
    proof = load(path)
    require(proof.get('status') == 'PASS_LOCAL_CANONICAL_PRODUCTION_ARTIFACTS' and proof.get('board') == board, 'Wrong canonical board proof')
    inputs = {'proof': reference(path)}

    def bound(key, value):
        p = pin(value)
        inputs[key] = reference(p)
        return p

    elf = bound('elf', proof['elf'])
    header = elf.open('rb')
    with header:
        first = header.read(20)
    require(first[:6] == b'\x7fELF\x01\x01' and int.from_bytes(first[18:20], 'little') == 94, 'Only ELF32 little-endian Xtensa artifacts supported')
    binary = bound('binary', proof['artifact'])
    partition = bound('partition', proof['partition'])
    slots = ota_slots(partition.read_bytes())
    slot_bytes = min(r['bytes'] for r in slots)
    require(slot_bytes == proof['slot_bytes'] and binary.stat().st_size == proof['size'] <= slot_bytes, 'Application size/actual OTA partition proof differs')
    memory = load(bound('link_memory', proof['link_memory']))
    symbols = bound('symbols', memory['symbols']).read_text()
    prior_sections = parse_size(bound('sections', memory['sections']).read_text())
    stacks = load(bound('stack_metadata', proof['stack_usage']))
    config = load(bound('configuration', proof['configuration']))
    argv = config['argv']
    fqbn = argv[argv.index('--fqbn') + 1]
    compilers = {str(Path(u['command']['arguments'][0]).absolute()) for u in stacks['units']}
    require(compilers and all(re.fullmatch(r'xtensa-esp32s3-elf-(g\+\+|gcc)', Path(c).name) for c in compilers), 'Unrecognized recorded compiler')
    parents = {str(Path(c).parent) for c in compilers}
    require(len(parents) == 1, 'Mixed toolchain directories')
    tools = Path(next(iter(parents)))
    size_text, size_receipt = run_tool(regular(tools/'xtensa-esp32s3-elf-size'), ['-A', str(elf)], out, board+'-size')
    require(parse_size(size_text) == prior_sections, 'Current ELF sections differ from pinned canonical size log')
    section_text, section_receipt = run_tool(regular(tools/'xtensa-esp32s3-elf-readelf'), ['-SW', str(elf)], out, board+'-readelf')
    sections = parse_readelf(section_text)
    require({r['name']:(r['bytes'],r['address']) for r in sections if r['name'] in prior_sections} == prior_sections, 'ELF section headers disagree with size output')
    totals = {k:sum(r['bytes'] for r in sections if r['category'] == k) for k in (*RAM, 'flash_mapped', 'other_allocated')}
    require(totals['dram'] == memory['static_ram_bytes'], 'Canonical static DRAM summary differs')
    driver = load(bound('compiled_sdk_proof', proof['camera_driver']['after_compile']))
    sdk = bound('sdkconfig', driver['files']['sdkconfig'])
    map_path = regular(elf.with_suffix('.map'))
    require(sha(map_path) == driver['linker_map']['sha256'], 'Canonical linker map differs from compiled proof')
    inputs['map'] = reference(map_path)
    relevant = re.compile(r'CONFIG_(?:SPIRAM(?:_.*)?|MBEDTLS_.*(?:ALLOC|CONTENT_LEN|DYNAMIC.*)|ESP_WIFI_(?:STATIC|DYNAMIC).*|FREERTOS_TASK_CREATE_ALLOW_EXT_MEM)$')
    settings = {}
    for line in sdk.read_text().splitlines():
        m = re.match(r'^#define\s+(CONFIG_\w+)\s+(.+)$', line)
        if m and relevant.fullmatch(m[1]):
            settings[m[1]] = m[2]
    for item in inputs.values():
        pin(item)
    return dict(board=board, version=proof['version'], build_id=proof['build_id'], source_commit=proof['git_commit'],
                fqbn=fqbn, inputs=inputs, tools=[size_receipt, section_receipt], sections=sections,
                section_bytes=totals, largest_ram_symbols=ram_symbols(symbols, sections),
                application=dict(bytes=proof['size'], slot_bytes=slot_bytes, margin_bytes=slot_bytes-proof['size'],
                                 margin_percent=round(100*(slot_bytes-proof['size'])/slot_bytes, 4), ota_slots=slots),
                compiled_sdk_settings=settings, runtime_heap='unknown', runtime_stack_peak='unknown')


def compare(current, previous):
    require(previous.get('schema') == SCHEMA and previous.get('status') == 'PASS_STATIC_RESOURCE_REPORT', 'Invalid prior resource report')
    result = {}
    for board, new in current.items():
        old = previous['boards'][board]
        require(old['board'] == board and old['fqbn'] == new['fqbn'] and old['application']['slot_bytes'] == new['application']['slot_bytes'], 'Comparison requires matching board/FQBN/OTA slot')
        result[board] = dict(previous_version=old['version'], previous_build_id=old['build_id'],
                            section_bytes_delta={k:new['section_bytes'][k]-old['section_bytes'][k] for k in new['section_bytes']},
                            application_bytes_delta=new['application']['bytes']-old['application']['bytes'],
                            ota_margin_bytes_delta=new['application']['margin_bytes']-old['application']['margin_bytes'])
    return result


def markdown(report):
    lines = ['# Static firmware resource report', '', 'These are artifact measurements, not runtime free-memory or stack measurements.', '',
             '| Board/version | DRAM data+BSS/noinit | IRAM sections | RTC fast | RTC slow | Static PSRAM | Application / actual OTA slot | Slot margin |',
             '|---|---:|---:|---:|---:|---:|---:|---:|']
    for board, r in report['boards'].items():
        s, a = r['section_bytes'], r['application']
        lines.append(f"| {board} {r['version']} | {s['dram']:,} | {s['iram']:,} | {s['rtc_fast']:,} | {s['rtc_slow']:,} | {s['psram_static']:,} | {a['bytes']:,} / {a['slot_bytes']:,} | {a['margin_bytes']:,} ({a['margin_percent']:.2f}%) |")
    for board, r in report['boards'].items():
        lines += ['', f'## {board}: largest RAM symbols', '', f"Build `{r['build_id']}`; source `{r['source_commit']}`.", '', '| Symbol | Bytes | ELF section |', '|---|---:|---|']
        for sym in r['largest_ram_symbols']:
            lines.append(f"| `{sym['name'].replace('|', '&#124;')}` | {sym['bytes']:,} | `{sym['section']}` |")
        lines += ['', 'Compiled SDK settings (defaults, not runtime allocation measurements):', '', '```text']
        lines += [f'{k}={v}' for k,v in sorted(r['compiled_sdk_settings'].items())]
        lines += ['```']
    if 'comparison' in report:
        lines += ['', '## Static changes from previous report', '']
        for board, delta in report['comparison'].items():
            lines.append(f"- {board} vs {delta['previous_version']}: application {delta['application_bytes_delta']:+,} bytes; OTA margin {delta['ota_margin_bytes_delta']:+,} bytes; section changes {json.dumps(delta['section_bytes_delta'], sort_keys=True)}.")
    lines += ['', '## Limits', '', *['- '+s for s in LIMITS], '', 'All input paths/hashes, individual sections and bounded tool receipts are in `REPORT.json`.', '']
    return '\n'.join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sense-proof', required=True, type=Path)
    p.add_argument('--lcd-proof', required=True, type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--compare', type=Path, help='Previous REPORT.json; compare static sizes only')
    a = p.parse_args()
    proof_paths = {'sense':regular(a.sense_proof), 'lcd':regular(a.lcd_proof)}
    forbidden = [Path(__file__).resolve().parents[3]]
    for path in proof_paths.values():
        proof = load(path)
        forbidden += [Path(proof['source_root']), Path(proof['source_manifest']['path']).parent.parent]
    out = reserve_output(a.out, forbidden)
    try:
        boards = {board:inspect_board(path, board, out) for board,path in proof_paths.items()}
        require(len({(r['version'],r['build_id'],r['source_commit']) for r in boards.values()}) == 1, 'Paired proof identities differ')
        report = dict(schema=SCHEMA, status='PASS_STATIC_RESOURCE_REPORT', created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                      generator=reference(__file__), boards=boards, limits=LIMITS)
        if a.compare:
            report['compared_report'] = reference(a.compare)
            report['comparison'] = compare(boards, load(a.compare))
        (out/'REPORT.md').write_text(markdown(report))
        (out/'REPORT.json').write_text(json.dumps(report, indent=2)+'\n')
        print(json.dumps(dict(status=report['status'], report=reference(out/'REPORT.json'), markdown=reference(out/'REPORT.md'))))
    except Exception as error:
        (out/'FAILURE.json').write_text(json.dumps({'status':'FAIL_STATIC_RESOURCE_REPORT','error':str(error)},indent=2)+'\n')
        raise


if __name__ == '__main__':
    main()
