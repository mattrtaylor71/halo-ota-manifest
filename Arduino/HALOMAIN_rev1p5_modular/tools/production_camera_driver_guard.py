"""Pin the reviewed ESP32-S3 camera driver before and after production builds.

The inactive-driver PSRAM-mode fallback relies on this archive's setter storing
the mode before returning ESP_ERR_INVALID_STATE, and its getter reading that
mode without an initialized camera. The package has no reliable camera version
label; exact bytes, compiled header dependencies and linker provenance bind it.
This helper never changes the SDK and never compiles firmware.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

SDK_RELATIVE = Path('packages/esp32/tools/esp32s3-libs/3.3.8')
PINS = {
    'archive': ('lib/libespressif__esp32-camera.a', 2041228,
                '9f25728226978bdf68fe92df6c6fe9cc6283efdc237a4c1e6927b3d13713992a'),
    'header': ('include/espressif__esp32-camera/driver/include/esp_camera.h', 9552,
               '8e295497b505b1a717919ee4773283d2acf1a283efa27ac1ea72823045152c0f'),
    'sdkconfig': ('qio_opi/include/sdkconfig.h', 85812,
                  'f7f482eccb2ab90187e18f51d475779d89f5dd8912504362d4f0373c936bb10f'),
}
SYMBOL_MEMBERS = {
    'esp_camera_set_psram_mode': 'esp_camera.c.obj',
    'esp_camera_get_psram_mode': 'esp_camera.c.obj',
    'cam_set_psram_mode': 'cam_hal.c.obj',
    'cam_get_psram_mode': 'cam_hal.c.obj',
}


def ref(path):
    path = Path(path).resolve()
    raw = path.read_bytes()
    return {'path': str(path), 'bytes': len(raw),
            'sha256': hashlib.sha256(raw).hexdigest()}


def save(path, value):
    with Path(path).open('x') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')


def pinned_files(sdk):
    result = {}
    for name, (relative, size, digest) in PINS.items():
        result[name] = ref(Path(sdk) / relative)
        if result[name]['bytes'] != size or result[name]['sha256'] != digest:
            raise ValueError('Unreviewed ESP32-S3 camera SDK ' + name)
    return result


def prepare(compiler, out):
    data = subprocess.check_output([str(compiler), 'config', 'get', 'directories.data'],
                                   text=True, timeout=10).strip()
    if not Path(data).is_absolute():
        raise ValueError('Arduino data directory must be absolute')
    sdk = (Path(data) / SDK_RELATIVE).resolve()
    receipt = {'status': 'PASS_PINNED_ESP32S3_CAMERA_BEFORE_COMPILE',
               'package': 'esp32s3-libs/3.3.8', 'sdk': str(sdk),
               'files': pinned_files(sdk), 'helper': ref(__file__)}
    save(Path(out) / 'sdk-camera-before.json', receipt)
    return receipt


def verify_compiled(receipt, build, board):
    if board not in ('sense', 'lcd'):
        raise ValueError('Unknown camera guard production board')
    current = pinned_files(receipt['sdk'])
    if current != receipt['files']:
        raise ValueError('Camera SDK changed during compilation')
    build = Path(build)
    map_path = build / ('halo_%s_prod.ino.map' % board)
    text = map_path.read_text()
    # Discarded input sections also name these APIs, but cannot prove linkage.
    marker = 'Linker script and memory map'
    if text.count(marker) != 1:
        raise ValueError('Missing canonical camera linker map section')
    linked = text.split(marker, 1)[1]
    loaded = {str(Path(value.strip()).resolve()) for value in
              re.findall(r'^LOAD (.*libespressif__esp32-camera\.a)\s*$', linked, re.M)}
    expected_archive = current['archive']['path']
    if loaded != {expected_archive}:
        raise ValueError('Linker loaded a different or missing camera archive')
    evidence = {'status': 'PASS_PINNED_ESP32S3_CAMERA_AFTER_COMPILE', 'board': board,
                'files': current, 'linker_map': ref(map_path),
                'linked_archive': current['archive'], 'linked_symbols': {},
                'helper': ref(__file__)}
    if board == 'sense':
        for symbol, member in SYMBOL_MEMBERS.items():
            pattern = (r'^\s*\.text\.' + re.escape(symbol) +
                       r'\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+([^\n]+\.a)\(([^)]+)\)')
            matches = re.findall(pattern, linked, re.M)
            if (len(matches) != 1 or not int(matches[0][0], 16) or not int(matches[0][1], 16)
                    or str(Path(matches[0][2].strip()).resolve()) != expected_archive
                    or matches[0][3] != member):
                raise ValueError('Camera API not linked from pinned archive: ' + symbol)
            evidence['linked_symbols'][symbol] = {'address': matches[0][0],
                                                  'bytes': int(matches[0][1], 16),
                                                  'member': member}
        dependency = build / 'sketch/halo_sense_prod.ino.cpp.d'
        for name, dependencies in (('header', [dependency]),
                                   ('sdkconfig', sorted((build / 'sketch').glob('*.d')))):
            lines = [line for path in dependencies for line in path.read_text().splitlines()]
            basename = Path(current[name]['path']).name
            paths = {str(Path(line.strip().rstrip('\\').strip().replace('\\ ', ' ')).resolve())
                     for line in lines if line.strip().rstrip('\\').strip().endswith('/' + basename)}
            if paths != {current[name]['path']}:
                raise ValueError('Compiler used a different or missing camera ' + name)
        evidence['header_dependencies'] = ref(dependency)
        wrapper = '__wrap_heap_caps_aligned_alloc'
        wrapper_object = (build / 'sketch/halo_sense_prod.ino.cpp.o').resolve()
        # IRAM_ATTR input section numbers vary with source order. Join the
        # named retained symbol to its containing section and defining object.
        sections = []
        pattern = r'^\s+(\.(?:text\.\S+|iram\S*))\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+([^\n]+\.o)[ \t]*$'
        for section in re.finditer(pattern, linked, re.M):
            if Path(section[4].strip()).resolve() != wrapper_object:
                continue
            for line in linked[section.end():].splitlines():
                if re.match(r'^\s+\.\S|^\S', line):
                    break
                symbol = re.match(r'^\s+(0x[0-9a-fA-F]+)\s+' + wrapper + r'\s*$', line)
                if symbol and 0 < int(section[2], 16) <= int(symbol[1], 16) < int(section[2], 16) + int(section[3], 16):
                    sections.append({'section': section[1], 'address': symbol[1]})
        if len(sections) != 1:
            raise ValueError('Camera aligned-allocation wrapper is not retained from the sketch')
        evidence['allocation_wrapper'] = {'symbol': wrapper, 'object': str(wrapper_object),
                                           **sections[0], 'camera_reference_verified': False}
        if 'Cross Reference Table' in linked:
            cross = linked.split('Cross Reference Table', 1)[1]
            match = re.search(r'^' + wrapper + r'\s+([^\n]+)\n((?:[ \t]+[^\n]*\n)*)', cross, re.M)
            member_suffix = '(cam_hal.c.obj)'
            references = {str(Path(line.strip()[:-len(member_suffix)]).resolve())
                          for line in match[2].splitlines()
                          if line.strip().endswith(member_suffix)} if match else set()
            if (not match or Path(match[1].strip()).resolve() != wrapper_object
                    or expected_archive not in references):
                raise ValueError('Camera allocator reference does not resolve to the wrapper')
            evidence['allocation_wrapper']['camera_reference_verified'] = True
    save(build.parent / 'sdk-camera-compiled.json', evidence)
    return evidence
