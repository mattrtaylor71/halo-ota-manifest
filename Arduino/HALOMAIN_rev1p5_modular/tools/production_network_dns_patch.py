"""Exact Arduino ESP32 3.3.8 DNS-cache lock correction and build provenance.

Only --apply changes the installed SDK. Build admission is read-only there.
Never hold the TCPIP lock around the subsequent blocking getaddrinfo calls.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

SDK_RELATIVE = Path('packages/esp32/hardware/esp32/3.3.8/libraries/Network/src/NetworkManager.cpp')
ORIGINAL_SHA = 'a4c0c9c35fa185884d5acbc3c9a3e029d0fb9e4bf824d313e81b5dd4c6d5328a'
PATCHED_SHA = '1ccd9f9db9a11aa767ab8cb3c9cea22af80fee684d8b111a306cab67b5ed0f67'
EDITS = ((b'#include "lwip/dns.h"\n', b'#include "lwip/dns.h"\n#include "lwip/tcpip.h"\n'),
         (b'    dns_clear_cache();\n', b'    LOCK_TCPIP_CORE();\n    dns_clear_cache();\n    UNLOCK_TCPIP_CORE();\n'))


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def ref(path):
    path = Path(path).resolve()
    return {'path': str(path), 'sha256': sha(path.read_bytes())}


def save(path, value):
    with Path(path).open('x') as f:
        json.dump(value, f, indent=2)
        f.write('\n')


def corrected(raw):
    if sha(raw) == PATCHED_SHA:
        return raw
    if sha(raw) != ORIGINAL_SHA:
        raise ValueError('Unknown NetworkManager.cpp; refusing SDK patch')
    for before, after in EDITS:
        if raw.count(before) != 1:
            raise ValueError('Unexpected DNS patch context')
        raw = raw.replace(before, after)
    if sha(raw) != PATCHED_SHA:
        raise ValueError('Unexpected patched SDK hash')
    return raw


def apply(source, out):
    source, out = Path(source).resolve(), Path(out).resolve()
    raw = source.read_bytes()
    patched = corrected(raw)  # No output or mutation for an unknown SDK.
    out.mkdir(parents=True, exist_ok=False)
    (out / 'NetworkManager.before.cpp').write_bytes(raw)
    (out / 'NetworkManager.patched.cpp').write_bytes(patched)
    changed = raw != patched
    if changed:
        # Complete a same-directory file before replacing the exact preimage.
        fd, temporary = tempfile.mkstemp(prefix='.halo-dns-', dir=source.parent)
        try:
            with os.fdopen(fd, 'wb') as f:
                f.write(patched)
                f.flush()
                os.fsync(f.fileno())
            os.chmod(temporary, source.stat().st_mode & 0o777)
            if source.read_bytes() != raw:
                raise ValueError('SDK changed during patch preparation')
            os.replace(temporary, source)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    if source.read_bytes() != patched:
        raise ValueError('SDK patch readback differs')
    receipt = {'status': 'PASS_EXACT_ARDUINO338_DNS_CACHE_LOCK_PATCH',
               'changed': changed, 'source': ref(source), 'before': ref(out / 'NetworkManager.before.cpp'),
               'patched': ref(out / 'NetworkManager.patched.cpp'), 'helper': ref(__file__),
               'original_sha256': ORIGINAL_SHA, 'patched_sha256': PATCHED_SHA}
    save(out / 'result.json', receipt)
    return receipt


def prepare(compiler, out):
    data = subprocess.check_output([str(compiler), 'config', 'get', 'directories.data'],
                                   text=True, timeout=10).strip()
    if not Path(data).is_absolute():
        raise ValueError('Arduino data directory must be absolute')
    source = (Path(data) / SDK_RELATIVE).resolve()
    raw = source.read_bytes()
    if sha(raw) != PATCHED_SHA:
        raise ValueError('Canonical build requires reviewed production_network_dns_patch.py --apply first')
    snapshot = Path(out) / 'NetworkManager.compiled-source.cpp'
    snapshot.write_bytes(raw)
    receipt = {'status': 'PASS_PINNED_ARDUINO338_DNS_PATCH_BEFORE_COMPILE', 'source': ref(source),
               'snapshot': ref(snapshot), 'helper': ref(__file__), 'patched_sha256': PATCHED_SHA}
    save(Path(out) / 'sdk-dns-patch.json', receipt)
    return receipt


def verify_compiled(receipt, build):
    source = Path(receipt['source']['path'])
    if ref(source) != receipt['source'] or receipt['source']['sha256'] != PATCHED_SHA:
        raise ValueError('SDK source changed during compilation')
    commands = Path(build) / 'compile_commands.json'
    matches = [row for row in json.loads(commands.read_text())
               if Path(row['file']).name == 'NetworkManager.cpp']
    if len(matches) != 1 or Path(matches[0]['file']).resolve() != source:
        raise ValueError('Compiler used a different NetworkManager.cpp')
    row = matches[0]
    args = row['arguments']
    obj = Path(args[args.index('-o') + 1])
    if not obj.is_absolute():
        obj = Path(row['directory']) / obj
    result = {'status': 'PASS_ACTUAL_COMPILED_ARDUINO338_DNS_PATCH', 'source': ref(source),
              'snapshot': receipt['snapshot'], 'compile_commands': ref(commands),
              'translation_unit_command': row, 'object': ref(obj), 'helper': ref(__file__)}
    save(Path(build).parent / 'sdk-dns-compiled.json', result)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', required=True)
    parser.add_argument('--sdk-source', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(apply(args.sdk_source, args.out)))
