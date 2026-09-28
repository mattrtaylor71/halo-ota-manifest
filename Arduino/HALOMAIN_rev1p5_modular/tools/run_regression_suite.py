#!/usr/bin/env python3
"""Run every reviewed offline HALO regression; never drive a device or publish.

Use the runner inside a materialized release snapshot for publication evidence.
A working-tree run is useful during development but is not release evidence.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
from host_paths import arduino_data, arduino_user, mbedtls_prefix, negative_source_root
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
CATALOG = 'tools/regression_suite.json'
HISTORY_CASES = {'test_lcd_main_render_guard', 'test_media_retry_corner_cases'}
IGNORED = {'.git', '__pycache__', '.DS_Store'}
RUNNING = set()
RUNNING_LOCK = threading.Lock()
STOPPING = threading.Event()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reference(path):
    return {'path': str(Path(path).resolve()), 'sha256': sha(path)}


def clean_environment():
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('GIT_', 'HALO_', 'PYTHON'))
           and k != '__PYVENV_LAUNCHER__'}
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return env


def discover(root):
    # Intentionally excludes wifi_coldstart_test, automation and factory tools.
    return {p.relative_to(root).as_posix() for pattern in
            ('tools/test_*.py', 'tools/stress_media_*.py',
             'halo_ota_demo/tools/ota/test_*.py') for p in root.glob(pattern)}


def load_catalog(root):
    catalog = json.loads((root / CATALOG).read_text())
    if catalog.get('schema_version') != 1:
        raise ValueError('Unsupported regression catalog')
    suites = catalog['suites']
    paths, ids = set(), set()
    for case in suites:
        path, name = case['path'], case['id']
        if path in paths or name in ids or not re.fullmatch(r'[a-z0-9_]+', name):
            raise ValueError('Duplicate/invalid catalog entry: ' + name)
        if Path(path).is_absolute() or '..' in Path(path).parts:
            raise ValueError('Test path must stay inside source')
        if Path(path).stem != name or not (root / path).is_file():
            raise ValueError('Missing/mismatched test: ' + path)
        if not isinstance(case.get('args', []), list) or not all(
                isinstance(arg, str) for arg in case.get('args', [])):
            raise ValueError('Test arguments must be strings')
        if not 1 <= case.get('timeout_seconds', 180) <= 600:
            raise ValueError('Test timeout must be 1..600 seconds')
        paths.add(path); ids.add(name)
    found = discover(root)
    if not suites or found != paths:
        raise ValueError('Catalog drift: unlisted=' + repr(sorted(found - paths))
                         + ' missing_or_unsafe=' + repr(sorted(paths - found)))
    return suites


def fingerprint(root):
    """Include all source/test files, including untracked additions; ignore caches."""
    result = {}
    for path in sorted(root.rglob('*')):
        relative = path.relative_to(root)
        if any(part in IGNORED for part in relative.parts) or path.suffix == '.pyc':
            continue
        if path.is_symlink():
            raise ValueError('Source symlink is not a frozen input: ' + str(relative))
        if path.is_file():
            result[relative.as_posix()] = sha(path)
    return result


def materialized_source(root, path, files):
    if path is None:
        candidate = root.parent / 'materialization.json'
        path = candidate if candidate.is_file() else None
    if path is None:
        return None, None
    data = json.loads(path.read_text())
    if Path(data['source_root']).resolve() != root or data['source_snapshot'] != files:
        raise ValueError('Materialization does not match the complete tested source')
    return data, reference(path)


def preflight(root, suites, history_root, negative_root):
    """Fail visibly on missing prerequisites; a skipped test is never a pass."""
    if sys.platform != 'darwin' or sys.version_info < (3, 10) or not __debug__:
        raise ValueError('Use macOS Python 3.10+ without -O; native tests use CommonCrypto')
    binaries = ('clang++', 'c++', 'cc', 'cmake', 'git', 'arduino-cli')
    missing = [name for name in binaries if shutil.which(name) is None]
    required = [
        arduino_user() / 'libraries/ArduinoJson/src/ArduinoJson.h',
        arduino_user() / 'libraries/lvgl/lvgl.h',
        arduino_data() / 'packages/esp32/hardware/esp32/3.3.8/libraries/Preferences/src/Preferences.cpp',
        mbedtls_prefix() / 'include/mbedtls/md.h',
        negative_root / 'LCD_Minimal/lcd_voice_spool.h',
    ]
    missing += [str(p) for p in required if not p.is_file()]
    if missing:
        raise ValueError('Missing host prerequisites: ' + ', '.join(missing))
    env = clean_environment()
    git_dir = subprocess.check_output(['git', '-C', str(history_root),
        'rev-parse', '--absolute-git-dir'], env=env, text=True, timeout=10).strip()
    subprocess.run(['git', '--git-dir', git_dir, 'cat-file', '-e',
        '730d3c952de223c000e40a6e6a0c3a9e73c3f1a7^{commit}'],
        env=env, check=True, stdout=subprocess.DEVNULL, timeout=10)
    return {'commands': {name: shutil.which(name) for name in binaries},
            'required_files': [reference(p) for p in required],
            'history_git_dir': git_dir, 'history_root': str(history_root),
            'negative_source_root': str(negative_root)}


def terminate_group(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    # The parent may have exited while a compiler/grandchild remains.
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def run_owned(command, *, cwd, env, log, timeout):
    """Bound and reap the whole test/compiler process group."""
    with Path(log).open('x') as stream:
        with RUNNING_LOCK:
            if STOPPING.is_set():
                raise RuntimeError('Regression run is stopping; refusing new child')
            process = subprocess.Popen(command, cwd=cwd, env=env,
                stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT,
                start_new_session=True)
            RUNNING.add(process)
        try:
            try:
                return process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                stream.write('\nREGRESSION RUNNER: TIMEOUT\n'); stream.flush()
                terminate_group(process)
                return 124
        finally:
            try:
                # An exited test can leave a compiler in its owned process group.
                terminate_group(process)
            finally:
                with RUNNING_LOCK:
                    RUNNING.discard(process)


def run_case(root, out, case, dependencies):
    began = time.time()
    log = out / (case['id'] + '.log')
    values = {'case_out': str(out / case['id']),
              'negative_source_root': dependencies['negative_source_root']}
    argv = [sys.executable, '-B', str(root / case['path'])]
    argv += [arg.format(**values) for arg in case.get('args', [])]
    env = clean_environment()
    if case['id'] in HISTORY_CASES:
        env['GIT_DIR'] = dependencies['history_git_dir']
        env['GIT_WORK_TREE'] = dependencies['history_root']
    error = None
    try:
        code = run_owned(argv, cwd=root, env=env, log=log,
                         timeout=case.get('timeout_seconds', 180))
    except Exception as exc:
        code, error = 125, str(exc)
        if not log.exists():
            log.write_text(error + '\n')
    output = log.read_text(errors='replace')
    skipped = bool(re.search(r'\bOK \(skipped=|\bSkipTest\b|^SKIP(?:PED)?\b',
                             output, re.M))
    status = 'PASS' if code == 0 and not skipped else 'FAIL'
    result = {'id': case['id'], 'path': case['path'],
        'test_sha256': sha(root / case['path']), 'argv': argv, 'exit_code': code,
        'status': status, 'skipped': skipped, 'error': error,
        'log': str(log), 'log_sha256': sha(log), 'started_epoch': began,
        'elapsed_seconds': round(time.time() - began, 3)}
    print(f"{status} {case['id']} ({result['elapsed_seconds']:.1f}s)", flush=True)
    return result


def main():
    STOPPING.clear()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=ROOT)
    parser.add_argument('--out', type=Path, help='New output directory outside source')
    parser.add_argument('--jobs', type=int, choices=range(1, 5), default=2)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--materialization', type=Path)
    parser.add_argument('--history-repo', type=Path)
    parser.add_argument('--negative-source-root', type=Path, default=negative_source_root())
    args = parser.parse_args()
    root = args.source_root.resolve()
    suites = load_catalog(root)
    if args.list:
        for case in suites:
            print(case['id'])
        print(f'{len(suites)} host suites; hardware and cloud operations excluded')
        return 0
    if args.out is None:
        parser.error('--out is required unless --list is used')
    out = args.out.resolve()
    if out == root or root in out.parents or out.exists():
        parser.error('--out must be a new directory outside the source tree')
    out.mkdir(parents=True)
    result = {'schema_version': 1, 'status': 'FAIL', 'scope': 'full',
        'source_root': str(root), 'started_epoch': time.time(), 'cases': [],
        'source_unchanged_after_tests': False, 'materialization': None,
        'physical_validation': False, 'hardware_commands': 0,
        'limitations': ['Host coverage is finite and uses SDK/RTOS boundary doubles.',
                       'Real radio, display/touch, power and cloud behavior require device tests.']}
    pool = None
    try:
        before = fingerprint(root)
        materialization, receipt = materialized_source(root, args.materialization, before)
        result.update(source_files=before, materialization=receipt,
                      catalog=reference(root / CATALOG), runner=reference(Path(__file__)))
        history = (args.history_repo or (Path(materialization['original_source_root'])
                   if materialization else root)).resolve()
        deps = preflight(root, suites, history, args.negative_source_root.resolve())
        result['dependencies'] = deps
        result['source_commit'] = (materialization['git_commit'] if materialization else
            subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'],
                env=clean_environment(), text=True, timeout=10).strip())
        result['build_id'] = materialization['build_id'] if materialization else None
        print(f'Running {len(suites)} offline suites with {args.jobs} workers', flush=True)
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs)
        pending = {pool.submit(run_case, root, out, case, deps): case for case in suites}
        for future in concurrent.futures.as_completed(pending):
            result['cases'].append(future.result())
        result['cases'].sort(key=lambda case: case['id'])
        after = fingerprint(root)
        result['source_unchanged_after_tests'] = before == after
        if before != after:
            result['changed_paths'] = sorted(k for k in before.keys() | after.keys()
                                             if before.get(k) != after.get(k))
        result['status'] = ('PASS' if before == after and len(result['cases']) == len(suites)
                            and all(case['status'] == 'PASS' for case in result['cases']) else 'FAIL')
    except (Exception, KeyboardInterrupt) as exc:
        result['error'] = type(exc).__name__ + ': ' + str(exc)
        print(result['error'], file=sys.stderr)
    finally:
        if pool:
            with RUNNING_LOCK:
                STOPPING.set()
                running = list(RUNNING)
            pool.shutdown(wait=False, cancel_futures=True)
            cleanup_errors = []
            for process in running:
                try:
                    terminate_group(process)
                except Exception as exc:
                    cleanup_errors.append(str(exc))
            pool.shutdown(wait=True, cancel_futures=True)
            if cleanup_errors:
                result['status'] = 'FAIL'
                result['cleanup_errors'] = cleanup_errors
        STOPPING.clear()
        result['finished_epoch'] = time.time()
        result['suite_count'] = len(suites)
        (out / 'RESULT.json').write_text(json.dumps(result, indent=2) + '\n')
    failed = [case['id'] for case in result['cases'] if case['status'] != 'PASS']
    print(json.dumps({'status': result['status'], 'suites': len(result['cases']),
                      'failed': failed, 'result': str(out / 'RESULT.json')}))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    raise SystemExit(main())
