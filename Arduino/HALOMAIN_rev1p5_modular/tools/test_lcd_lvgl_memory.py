#!/usr/bin/env python3
"""Run actual LVGL allocator/buffer OOM paths; host-only, bounded child failures."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
LVGL = Path.home() / 'Documents/Arduino/libraries/lvgl'
CONFIG = ROOT / 'LCD_Minimal/lv_conf.h'
SOURCES = [LVGL / 'src/misc' / name for name in ('lv_mem.c', 'lv_tlsf.c', 'lv_gc.c')]

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/misc/lv_mem.h"
#include "src/misc/lv_gc.h"

int main(int argc, char **argv) {
    assert(argc == 2);
    setvbuf(stdout, NULL, _IONBF, 0);
    lv_mem_init();
    _lv_gc_clear_roots();
    lv_mem_monitor_t initial, after;
    lv_mem_monitor(&initial);
    printf("configured=%u initial_free=%u biggest=%u\n", (unsigned)LV_MEM_SIZE,
           (unsigned)initial.free_size, (unsigned)initial.free_biggest_size);
    if (strcmp(argv[1], "pressure") == 0) {
        // Representative aggregate object pressure, not a replay of a whole UI.
        // 64KiB live data exceeds the old 48KiB total pool before 324B scratch.
        for (unsigned cycle = 0; cycle < 500; ++cycle) {
            void *blocks[64] = {0};
            for (unsigned i = 0; i < 64; ++i) {
                blocks[i] = lv_mem_alloc(1024);
                if (!blocks[i]) {
                    printf("PRESSURE_UNAVAILABLE cycle=%u allocated=%u requested=65536\n", cycle, i * 1024);
                    return 42;
                }
                memset(blocks[i], (int)i, 1024);
            }
            void *scratch = lv_mem_buf_get(324);
            assert(scratch);
            memset(scratch, 0x5a, 324);
            lv_mem_buf_release(scratch);
            // Free alternately to exercise coalescing, then release cached scratch.
            for (unsigned parity = 0; parity < 2; ++parity)
                for (unsigned i = parity; i < 64; i += 2) lv_mem_free(blocks[i]);
            lv_mem_buf_free_all();
            assert(lv_mem_test() == LV_RES_OK);
            lv_mem_monitor(&after);
            assert(after.free_size == initial.free_size);
            assert(after.free_biggest_size == initial.free_biggest_size);
        }
        puts("PASS 500 cycles: 64KiB live pressure + 324B scratch, integrity and full free-space recovery");
        return 0;
    }
    assert(strcmp(argv[1], "oom") == 0);
    // Leave less than the actual failing scratch request; use the production
    // lv_mem_buf_get -> lv_mem_realloc -> LV_ASSERT_MSG path unchanged.
    unsigned allocations = 0;
    while (lv_mem_alloc(256)) ++allocations;
    while (lv_mem_alloc(16)) ++allocations;
    assert(allocations > 0);
    lv_mem_monitor(&after);
    assert(after.free_biggest_size < 324);
    printf("ENTER_BUFFER_OOM requested=324 remaining_biggest=%u\n", (unsigned)after.free_biggest_size);
    (void)lv_mem_buf_get(324);
    puts("ERROR OOM assertion returned");
    return 43;
}
'''


def ref(path):
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def child_limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def execute(binary, mode, timeout_s):
    started = time.monotonic()
    try:
        p = subprocess.run([str(binary), mode], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=timeout_s,
                           preexec_fn=child_limits)
        return {'mode': mode, 'returncode': p.returncode, 'timed_out': False,
                'output': p.stdout, 'duration_s': time.monotonic() - started}
    except subprocess.TimeoutExpired as exc:
        text = exc.stdout or b''
        if isinstance(text, bytes): text = text.decode(errors='replace')
        return {'mode': mode, 'returncode': None, 'timed_out': True,
                'output': text, 'duration_s': time.monotonic() - started}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, help='Optional JSON evidence receipt')
    args = ap.parse_args()
    config = CONFIG.read_text()
    assert re.search(r'#define\s+LV_MEM_SIZE\s+\(96U \* 1024U\)', config)
    assert re.search(r'#define\s+LV_ASSERT_HANDLER\s+abort\(\);', config)
    # A private copied baseline keeps every unrelated production setting intact.
    baseline, count = re.subn(r'(#define\s+LV_MEM_SIZE\s+)\(96U \* 1024U\)', r'\1(48U * 1024U)', config)
    assert count == 1
    baseline, count = re.subn(r'(#define\s+LV_ASSERT_HANDLER\s+)abort\(\);', r'\1while(1);', baseline)
    assert count == 1
    result = {'status': 'STARTED', 'scope': 'Actual LVGL allocator and scratch-buffer assertion only; synthetic aggregate pressure, not a full UI replay or hardware qualification.',
              'config': ref(CONFIG), 'sources': [ref(p) for p in SOURCES],
              'baseline': 'Private current-config copy with only pool=48KiB and fatal assertion=while(1); no library/config mutations.',
              'cases': []}
    try:
        with tempfile.TemporaryDirectory(prefix='halo-lvgl-memory-') as temp:
            temp = Path(temp)
            harness = temp / 'harness.c'; harness.write_text(HARNESS)
            for name, content in [('baseline', baseline), ('current', config)]:
                conf = temp / (name + '.h'); conf.write_text(content)
                binary = temp / name
                cmd = ['clang', '-std=c11', '-O0', '-DLV_CONF_PATH=' + str(conf),
                       '-I' + str(LVGL), str(harness)] + [str(p) for p in SOURCES] + ['-o', str(binary)]
                compile_result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
                entry = {'configuration': name, 'compile_returncode': compile_result.returncode, 'compile_output': compile_result.stdout}
                result['cases'].append(entry)
                assert compile_result.returncode == 0, compile_result.stdout
                pressure = execute(binary, 'pressure', 5)
                oom = execute(binary, 'oom', 1)
                entry.update(pressure=pressure, oom=oom)
                assert not pressure['timed_out'] and pressure['returncode'] == (42 if name == 'baseline' else 0)
                assert 'ENTER_BUFFER_OOM requested=324' in oom['output'], oom
                if name == 'baseline': assert oom['timed_out'], oom
                else: assert not oom['timed_out'] and oom['returncode'] == -signal.SIGABRT, oom
                print(name + ': ' + ('PASS old-pool pressure exhaustion and bounded reproduction of infinite OOM assertion' if name == 'baseline' else 'PASS 500 stable pressure cycles; forced324B OOM exits SIGABRT'))
        result['status'] = 'PASS'
    except Exception as exc:
        result.update(status='FAIL', error=type(exc).__name__, detail=str(exc))
        raise
    finally:
        if args.out:
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__': main()
