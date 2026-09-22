#!/usr/bin/env python3
"""Release verified prebuilt shipping images: local prepare, immutable stage, latest promotion."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time
from artifact_safety import validate_publishable_artifact
from publish_ota import generate_manifest

SLOTS = {'sense': 1966080, 'lcd': 2621440}
BUILDER = Path(__file__).resolve().parents[3] / 'tools/build_ota_policy_production.py'
_spec = importlib.util.spec_from_file_location('canonical_build', BUILDER)
canonical = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(canonical)
DESTINATIONS = {
    'production': ('halo-ota-prod', 'halo/ota/prod/'),
    'private-canary': ('halo-ota-dev', 'halo/ota/canary/production-release-20260909/dev/'),
}


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ref(path):
    path = Path(path).resolve()
    return {'path': str(path), 'sha256': sha(path.read_bytes())}


def pin(value):
    path = Path(value['path'])
    require(path.is_absolute() and ref(path) == value, 'Pinned release input changed')
    return path


def save(path, value):
    with Path(path).open('x') as f:
        json.dump(value, f, indent=2)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    return ref(path)


def validate_host_result(path, proofs):
    """Validate completed, source-bound host evidence; never run tests here."""
    result_ref = ref(path)
    result = json.loads(Path(path).read_text())
    require(result.get('schema_version') == 1 and result.get('status') == 'PASS' and
            result.get('scope') == 'full' and result.get('source_unchanged_after_tests') is True,
            'Complete passing host regression result required')
    materialization_ref = result.get('materialization')
    require(all(p['source_manifest'] == materialization_ref for p in proofs.values()),
            'Host result must match both artifact source manifests')
    materialization = json.loads(pin(materialization_ref).read_text())
    source = Path(materialization['source_root']).resolve()
    require(all(Path(p['source_root']).resolve() == source and
                p['build_id'] == materialization['build_id'] for p in proofs.values()),
            'Host/artifact source root or build identity differs')
    require(result.get('source_commit') == materialization['git_commit'] and
            result.get('build_id') == materialization['build_id'], 'Host source identity differs')
    hashes = materialization['source_snapshot']
    require(isinstance(hashes, dict) and hashes and result.get('source_files') == hashes,
            'Host result must cover the exact materialized source')
    actual = {}
    for file in source.rglob('*'):
        relative = file.relative_to(source)
        if any(part in ('.git', '__pycache__', '.DS_Store') for part in relative.parts) or file.suffix == '.pyc':
            continue
        require(not file.is_symlink(), 'Source symlink is not a frozen input')
        if file.is_file():
            actual[relative.as_posix()] = sha(file.read_bytes())
    require(actual == hashes, 'Materialized source changed after host tests')
    for name in hashes:
        relative = Path(name)
        require(not relative.is_absolute() and '..' not in relative.parts and
                source in (source / relative).resolve().parents, 'Invalid host source path')
    for key, relative in (('catalog', 'tools/regression_suite.json'),
                          ('runner', 'tools/run_regression_suite.py')):
        item = result.get(key)
        require(isinstance(item, dict) and pin(item) == source / relative and
                item['sha256'] == hashes.get(relative), 'Host ' + key + ' is not the snapshot version')
    catalog = json.loads(pin(result['catalog']).read_text())
    require(catalog.get('schema_version') == 1 and isinstance(catalog.get('suites'), list)
            and catalog['suites'], 'Invalid full regression catalog')
    suites, cases = catalog['suites'], result.get('cases')
    require(isinstance(cases, list) and len(cases) == len(suites), 'Missing host regression cases')
    required = {(s['id'], s['path']) for s in suites}
    require(len({s['id'] for s in suites}) == len(suites) and
            len({c['id'] for c in cases}) == len(cases) and
            {(c['id'], c['path']) for c in cases} == required,
            'Host case identities differ from full catalog')
    for case in cases:
        require(case.get('status') == 'PASS' and case.get('skipped', False) is False and
                type(case.get('exit_code')) is int and
                case['exit_code'] == 0, 'Failed, skipped or incomplete host case')
        require(case.get('test_sha256') == hashes.get(case['path']) and case['path'] in hashes,
                'Host case source hash differs')
        log = Path(case['log'])
        require(log.is_absolute() and sha(log.read_bytes()) == case.get('log_sha256'),
                'Host regression log changed')
    return result_ref


def shipping_proof(path, board, version, route='production'):
    require(route in DESTINATIONS, 'Unknown release route')
    p = json.loads(Path(path).read_text())
    require(p['status'] == 'PASS_LOCAL_CANONICAL_PRODUCTION_ARTIFACTS', 'Verified production artifact required')
    require(p.get('provisional_precommit_only') is not True and p.get('publishable') is not False, 'Provisional or held artifact cannot be packaged for publication')
    require(p['board'] == board and p['profile'] == 'shipping', 'Wrong shipping board/profile')
    require(p['version'] == version and p['slot_bytes'] == SLOTS[board], 'Wrong version or slot')
    for k in ('bench_profile', 'one_shot'):
        require(p[k] is False, 'Accelerated image cannot enter shipping release')
    require(p['private_route_override'] is (route == 'private-canary'), 'Compiled route does not match publication route')
    for k in ('artifact', 'elf', 'partition', 'configuration', 'flags', 'source_manifest', 'compiler_result'):
        pin(p[k])
    flags = json.loads(pin(p['flags']).read_text())
    require(flags['status'] == 'PASS_ACTUAL_PRODUCTION_FLAGS' and flags['command'] == p['configuration'], 'Actual compiled flag provenance required')
    expected = {'HALO_DURABLE_DIAGNOSTICS': '1', 'HALO_LCD_SLEEP_WITNESS': '1',
                'HALO_OTA_ONE_SHOT': '0', 'HALO_OTA_BENCH_PROFILE': '0', 'HALO_DIAG_AUTH_PROVISIONING': '0'}
    if board == 'sense':
        expected.update(HALO_DURABLE_OTA_POLICY='1', HALO_DIAGNOSTIC_ADMISSION='1',
                        HALO_IDLE_NETWORK_RECOVERY='1', HALO_IDLE_NETWORK_PROBE='0')
    require(all(flags['expected'].get(k) == v for k, v in expected.items()), 'Shipping flags mismatch')
    require(flags.get('fixture_override') is False, 'Fault fixture cannot be released')
    require(flags['private_route_override'] is (route == 'private-canary') and
            flags['one_shot'] is False and flags['bench_profile'] is False, 'Actual profile flags mismatch')
    command = json.loads(pin(p['configuration']).read_text())
    argv = command['argv']
    require(argv.count('--build-path') == 1, 'Ambiguous compiler command')
    expected_argv = canonical.command(board, Path(p['source_root']),
                                      argv[argv.index('--build-path') + 1], argv[0],
                                      route == 'private-canary')
    # Sense now has a second, exact linker property for camera DMA allocation.
    # Whole-command equality still rejects missing, extra or duplicate values.
    require(argv == expected_argv, 'Actual compiler command differs from complete canonical shipping profile')
    partition_csv = Path(p['source_root']) / ('halo_ota_demo/firmware/halo_' + board + '_prod/partitions.csv')
    require(sha(partition_csv.read_bytes()) == canonical.PARTITIONS[board], 'Changed canonical partition table')
    pin(flags['build_options'])
    for file, digest in flags['response_files'].items():
        pin({'path': file, 'sha256': digest})
    compiled = json.loads(pin(p['compiler_result']).read_text())
    require(compiled['exit_code'] == 0 and compiled['reaped'] and compiled['group_absent'] and not compiled['error'], 'Compiler ownership/result incomplete')
    binary = validate_publishable_artifact(pin(p['artifact']))
    require(0 < binary.stat().st_size == p['size'] <= SLOTS[board], 'Invalid app size')
    markers = re.findall(rb'HALO_FW_MARKER:[^\x00]+', binary.read_bytes())
    require(markers == [('HALO_FW_MARKER:' + version + '|BUILD_ID:' + p['build_id'] + '|BOARD:' + board).encode()], 'Exact unique BIN version/build/board marker mismatch')
    return p


def prepare(version, sense_proof, lcd_proof, baseline_sense, baseline_lcd, out, route='production', host_result=None):
    require(re.fullmatch(r'\d+\.\d+\.\d+', version), 'Explicit release version required')
    proofs = {b: shipping_proof(p, b, version, route) for b, p in [('sense', sense_proof), ('lcd', lcd_proof)]}
    require(host_result is not None, 'Full host regression result required')
    host_ref = validate_host_result(host_result, proofs)
    bucket, root_prefix = DESTINATIONS[route]
    old_paths = {b: Path(p) if p else None for b, p in [('sense', baseline_sense), ('lcd', baseline_lcd)]}
    for board, path in old_paths.items():
        if path is None:
            require(route == 'private-canary', 'Production requires exact previous latest bodies')
            continue
        old = json.loads(path.read_text())
        require(old['board'] == board, 'Wrong predecessor board')
        require(tuple(map(int, old['version'].split('.'))) < tuple(map(int, version.split('.'))), 'Release must advance predecessor')
    out = Path(out).resolve()
    out.mkdir(parents=True, exist_ok=False)
    objects, previous = [], {}
    for board in ('sense', 'lcd'):
        p = proofs[board]
        binary = out / (board + '.bin')
        shutil.copyfile(pin(p['artifact']), binary)
        proof = save(out / (board + '-proof.json'), p)
        prefix = root_prefix.rstrip('/') + ('/lcd' if board == 'lcd' else '')
        key = prefix + '/artifacts/' + board + '_' + version + '_' + p['artifact']['sha256'][:16] + '.bin'
        url = 'https://' + bucket + '.s3.us-east-1.amazonaws.com/' + key
        manifest = generate_manifest(version, url, p['artifact']['sha256'], p['size'], p['build_id'], version,
                                     min_version='0.0.0', max_slot_bytes=SLOTS[board], board=board)
        manifest_ref = save(out / (board + '-manifest.json'), manifest)
        objects.extend([{'board': board, 'role': 'artifact', 'key': key, 'file': ref(binary), 'proof': proof},
                        {'board': board, 'role': 'versioned_manifest', 'key': prefix + '/manifest_' + version + '.json',
                         'file': manifest_ref, 'proof': proof}])
        previous_file = None
        if old_paths[board] is not None:
            previous_path = out / (board + '-previous-latest.json')
            shutil.copyfile(old_paths[board], previous_path)
            previous_file = ref(previous_path)
        previous[board] = {'key': prefix + '/manifest_latest.json', 'file': previous_file}
    plan = {'schema': 1, 'version': version, 'route': route, 'bucket': bucket, 'prefix': root_prefix,
            'region': 'us-east-1', 'objects': objects, 'previous_latest': previous,
            'publication_order': ['lcd', 'sense'], 'automatic_retries': 0, 'compiler_actions': 0,
            'canonical_builder': ref(BUILDER), 'publisher': ref(__file__), 's3_guard': ref(Path(__file__).with_name('release_s3_guard.py')),
            'host_result': host_ref, 'created_epoch': time.time()}
    return save(out / 'release.json', plan)


def load_plan(path, expected_sha):
    path = Path(path).resolve()
    require(sha(path.read_bytes()) == expected_sha, 'Exact prepared release hash required')
    plan = json.loads(path.read_text())
    require(plan['route'] in DESTINATIONS and (plan['bucket'], plan['prefix']) == DESTINATIONS[plan['route']], 'Changed release destination')
    require(plan['schema'] == 1
            and plan['region'] == 'us-east-1', 'Changed production destination')
    require(plan['publication_order'] == ['lcd', 'sense'] and plan['automatic_retries'] == 0 and plan['compiler_actions'] == 0, 'Changed promotion/retry scope')
    require(pin(plan['canonical_builder']) == BUILDER, 'Canonical builder changed')
    require(pin(plan['publisher']) == Path(__file__).resolve() and pin(plan['s3_guard']) == Path(__file__).with_name('release_s3_guard.py').resolve(), 'Publisher source changed')
    require([(o['board'], o['role']) for o in plan['objects']] ==
            [('sense', 'artifact'), ('sense', 'versioned_manifest'), ('lcd', 'artifact'), ('lcd', 'versioned_manifest')], 'Exact paired resources required')
    source = None
    if 'bridge_source' in plan:
        require(plan['route'] == 'private-canary', 'Only production-to-canary manifest bridge permitted')
        source = load_plan(pin(plan['bridge_source']), plan['bridge_source']['sha256'])
        require(source['route'] == 'production' and source['version'] == plan['version'], 'Bridge must retain exact production release')
    proofs = {}
    for board in ('sense', 'lcd'):
        objects = [o for o in plan['objects'] if o['board'] == board]
        artifact, manifest = objects
        require('bucket' not in manifest, 'Manifest destination must match release route')
        p = shipping_proof(pin(artifact['proof']), board, plan['version'], 'production' if source else plan['route'])
        proofs[board] = p
        require(artifact['proof'] == manifest['proof'], 'Conflicting paired proof')
        pin(artifact['file']); pin(manifest['file'])
        previous = plan['previous_latest'][board]['file']
        if previous is not None:
            old = json.loads(pin(previous).read_text())
            require(old['board'] == board and tuple(map(int, old['version'].split('.'))) < tuple(map(int, plan['version'].split('.'))), 'Invalid predecessor identity')
        else:
            require(plan['route'] == 'private-canary', 'Production predecessor cannot be omitted')
        require(artifact['file']['sha256'] == p['artifact']['sha256'], 'Packaged BIN differs from verified build')
        m = json.loads(pin(manifest['file']).read_text())
        prefix = plan['prefix'].rstrip('/') + ('/lcd' if board == 'lcd' else '')
        if source:
            original_artifact, original_manifest = [o for o in source['objects'] if o['board'] == board]
            require(artifact == dict(original_artifact, bucket='halo-ota-prod'), 'Bridge must reference original production BIN, never copy or rename it')
            require(manifest['file']['sha256'] == original_manifest['file']['sha256'] and manifest['proof'] == original_manifest['proof'], 'Bridge manifest must be byte-identical to production versioned manifest')
        else:
            require('bucket' not in artifact and 'bucket' not in manifest, 'Unexpected per-object destination')
            require(artifact['key'] == prefix + '/artifacts/' + board + '_' + plan['version'] + '_' + p['artifact']['sha256'][:16] + '.bin', 'Unexpected artifact key')
        require(manifest['key'] == prefix + '/manifest_' + plan['version'] + '.json', 'Unexpected versioned key')
        require(plan['previous_latest'][board]['key'] == prefix + '/manifest_latest.json', 'Unexpected latest key')
        require(all(m.get(k) == v for k, v in {'version': plan['version'], 'artifact_fw_version': plan['version'],
                    'board': board, 'build_id': p['build_id'], 'sha256': p['artifact']['sha256'], 'size': p['size'],
                    'bin_url': 'https://' + ('halo-ota-prod' if source else plan['bucket']) + '.s3.us-east-1.amazonaws.com/' + artifact['key']}.items()), 'Manifest/BIN identity mismatch')
    require('host_result' in plan, 'Full host regression result required')
    validate_host_result(pin(plan['host_result']), proofs)
    return plan


def prepare_bridge(production, expected_sha, baseline_sense, baseline_lcd, out):
    """Copy production manifest bytes to canary; keep canonical production BIN references."""
    source = load_plan(production, expected_sha)
    require(source['route'] == 'production', 'Production source release required')
    old_paths = {'sense': Path(baseline_sense), 'lcd': Path(baseline_lcd)}
    for board, file in old_paths.items():
        old = json.loads(file.read_text())
        require(old['board'] == board and tuple(map(int, old['version'].split('.'))) < tuple(map(int, source['version'].split('.'))), 'Exact previous canary latest required')
    out = Path(out).resolve(); out.mkdir(parents=True, exist_ok=False)
    plan = dict(source, route='private-canary', bucket=DESTINATIONS['private-canary'][0], prefix=DESTINATIONS['private-canary'][1],
                bridge_source=ref(production), objects=[], previous_latest={}, created_epoch=time.time())
    for board in ('sense', 'lcd'):
        artifact, manifest = [o for o in source['objects'] if o['board'] == board]
        prefix = plan['prefix'].rstrip('/') + ('/lcd' if board == 'lcd' else '')
        file = out / (board + '-manifest.json'); shutil.copyfile(pin(manifest['file']), file)
        plan['objects'].extend([dict(artifact, bucket='halo-ota-prod'),
            dict(manifest, key=prefix + '/manifest_' + source['version'] + '.json', file=ref(file))])
        previous = out / (board + '-previous-latest.json'); shutil.copyfile(old_paths[board], previous)
        plan['previous_latest'][board] = {'key': prefix + '/manifest_latest.json', 'file': ref(previous)}
    return save(out / 'release.json', plan)


def close_owned_group(child):
    """Reap only this dedicated group, including descendants of an exited leader."""
    errors = []
    def present():
        child.poll()
        try:
            os.killpg(child.pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            if 'group_probe_permission' not in errors:
                errors.append('group_probe_permission')
            return True
    actions = []
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if not present():
            break
        try:
            os.killpg(child.pid, sig)
            actions.append(int(sig))
        except ProcessLookupError:
            pass
        except PermissionError:
            errors.append('signal_%d_permission' % int(sig))
        deadline = time.monotonic() + 2
        while present() and time.monotonic() < deadline:
            time.sleep(.02)
    return {'group_absent': not present(), 'cleanup_signals': actions, 'cleanup_errors': errors}


class Store:
    """Bounded owned commands; no bucket creation, policy changes or automatic retries."""
    def __init__(self, out, profile, aws_python, route='production', curl='/usr/bin/curl'):
        self.out, self.profile, self.aws_python, self.curl = Path(out), profile, aws_python, curl
        self.bucket, self.prefix = DESTINATIONS[route]
        self.deadline, self.sequence = time.monotonic() + 600, 0

    def command(self, argv, role):
        require(time.monotonic() + 5 < self.deadline, 'Release phase deadline expired')
        self.sequence += 1
        stem = self.out / ('%03d-%s' % (self.sequence, role))
        row = {'argv': argv, 'started_epoch': time.time(), 'automatic_retries': 0}
        save(str(stem) + '-intent.json', row)
        env = os.environ.copy()
        env.update(AWS_MAX_ATTEMPTS='1', AWS_RETRY_MODE='standard', AWS_EC2_METADATA_DISABLED='true',
                   AWS_PAGER='', AWS_CLI_AUTO_PROMPT='off', AWS_IGNORE_CONFIGURED_ENDPOINT_URLS='true')
        child, error = None, None
        with Path(str(stem) + '.stdout').open('xb') as stdout, Path(str(stem) + '.stderr').open('xb') as stderr:
            try:
                mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM, signal.SIGINT})
                try:
                    child = subprocess.Popen(argv, stdout=stdout, stderr=stderr, env=env, stdin=subprocess.DEVNULL, start_new_session=True)
                    row['pid'] = child.pid
                finally:
                    signal.pthread_sigmask(signal.SIG_SETMASK, mask)
                child.wait(timeout=min(40, max(.1, self.deadline - time.monotonic() - 5)))
            except BaseException as exc:
                error = exc
            finally:
                handlers = {s: signal.signal(s, signal.SIG_IGN) for s in (signal.SIGTERM, signal.SIGINT)}
                try:
                    if child is not None:
                        row.update(close_owned_group(child))
                        row.update(exit_code=child.poll(), reaped=child.poll() is not None)
                    row.update(finished_epoch=time.time(), error_type=type(error).__name__ if error else None)
                    save(str(stem) + '-reap.json', row)
                finally:
                    for s, handler in handlers.items():
                        signal.signal(s, handler)
        if error:
            raise error
        require(child.returncode == 0 and row['reaped'] and row['group_absent'], 'Release operation or cleanup failed; inspect ' + str(stem))
        return Path(str(stem) + '.stdout').read_bytes()

    def aws_args(self, operation):
        audit = self.out / ('s3-%03d.jsonl' % (self.sequence + 1))
        guard = Path(__file__).with_name('release_s3_guard.py')
        return [self.aws_python, str(guard), str(audit), '--profile', self.profile, '--region', 'us-east-1', '--no-cli-pager',
                '--cli-connect-timeout', '10', '--cli-read-timeout', '30', 's3api', operation,
                '--bucket', self.bucket]

    def version_absent(self, key):
        # Authenticated listing distinguishes an absent key from a public GET permission error.
        data = self.command(self.aws_args('list-objects-v2') + ['--prefix', key, '--no-paginate'], 'list')
        value = json.loads(data)
        require(value.get('IsTruncated') is False, 'Incomplete collision inventory')
        require(not any(o['Key'] == key for o in value.get('Contents', [])), 'Version already exists; do not overwrite')

    def get(self, key, bucket=None):
        bucket = bucket or self.bucket
        allowed = {b: prefix for b, prefix in DESTINATIONS.values()}
        require(bucket in allowed and key.startswith(allowed[bucket]) and '..' not in key, 'Out-of-scope object')
        stem = self.out / ('get-%03d' % (self.sequence + 1))
        body, headers = str(stem) + '.body', str(stem) + '.headers'
        code = self.command([self.curl, '--silent', '--show-error', '--proto', '=https', '--connect-timeout', '10',
                             '--max-time', '30', '--max-filesize', '4194304', '--dump-header', headers,
                             '--output', body, '--write-out', '%{http_code}',
                             'https://' + bucket + '.s3.us-east-1.amazonaws.com/' + key], 'get').decode()
        require(code == '200', 'Public object GET failed: ' + code)
        etags = re.findall(r'^etag:\s*(.+)$', Path(headers).read_text(), flags=re.I | re.M)
        require(len(etags) == 1, 'One actual S3 ETag required')
        return Path(body).read_bytes(), etags[0].strip()

    def put(self, key, file, etag=None):
        require(key.startswith(self.prefix) and '..' not in key, 'Out-of-scope write')
        args = self.aws_args('put-object') + ['--key', key, '--body', str(pin(file)), '--content-type',
                'application/json' if key.endswith('.json') else 'application/octet-stream', '--cache-control',
                'no-store, no-cache, max-age=0, must-revalidate' if key.endswith('/manifest_latest.json') else 'public, max-age=31536000, immutable']
        args += ['--if-match', etag] if etag else ['--if-none-match', '*']
        self.command(args, 'put')


def verify(store, obj):
    data, etag = store.get(obj['key'], bucket=obj.get('bucket'))
    require(sha(data) == obj['file']['sha256'], 'Served object differs from prepared release')
    return etag


def execute(plan, store, phase):
    require(phase in ('stage', 'promote'), 'Unknown release phase')
    etags = {}
    for board, obj in plan['previous_latest'].items():
        if obj['file'] is None:
            store.version_absent(obj['key'])
            etags[board] = None
        else:
            etags[board] = verify(store, obj)
    if 'bridge_source' in plan:
        source = load_plan(pin(plan['bridge_source']), plan['bridge_source']['sha256'])
        for obj in source['objects']:
            verify(store, dict(obj, bucket='halo-ota-prod'))
        save(store.out / 'bridge-production-verified.json', {'source': plan['bridge_source'], 'epoch': time.time(), 'four_served_resources_verified': True})
    if phase == 'stage':
        for obj in plan['objects']:
            if obj['role'] == 'versioned_manifest':
                store.version_absent(obj['key'])
        for obj in plan['objects']:
            if 'bridge_source' in plan and obj['role'] == 'artifact':
                continue
            store.put(obj['key'], obj['file'])
            verify(store, obj)
        return {'status': 'STAGED_PAIRED_IMMUTABLE_RESOURCES', 'latest_writes': 0,
                'immutable_writes': 2 if 'bridge_source' in plan else 4, 'binary_writes': 0 if 'bridge_source' in plan else 2}
    for obj in plan['objects']:
        verify(store, obj)
    promoted = []
    for board in plan['publication_order']:
        manifest = next(o for o in plan['objects'] if o['board'] == board and o['role'] == 'versioned_manifest')
        latest = {'key': plan['previous_latest'][board]['key'], 'file': manifest['file']}
        save(store.out / ('promotion-attempt-' + board + '.json'),
             {'board': board, 'latest': latest, 'prior_etag': etags[board], 'epoch': time.time(),
              'status': 'ATTEMPT_RECORDED_OUTCOME_UNKNOWN_UNTIL_VERIFIED', 'automatic_retries': 0})
        store.put(latest['key'], latest['file'], etag=etags[board])
        verify(store, latest)
        promoted.append(board)
        save(store.out / ('promoted-' + board + '.json'), {'board': board, 'latest': latest, 'epoch': time.time()})
    return {'status': 'PROMOTED_PAIRED_RELEASE', 'latest_writes': 2, 'promoted': promoted}


def main():
    os.umask(0o077)
    def interrupted(sig, frame):
        raise InterruptedError('Release phase interrupted')
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, interrupted)
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='phase', required=True)
    prep = sub.add_parser('prepare', help='Local only; validate and copy exact shipping artifacts')
    for field in ('version', 'sense-proof', 'lcd-proof', 'host-result', 'out'):
        prep.add_argument('--' + field, required=True)
    for field in ('baseline-sense', 'baseline-lcd'):
        prep.add_argument('--' + field)
    prep.add_argument('--route', choices=tuple(DESTINATIONS), default='production')
    bridge = sub.add_parser('prepare-bridge', help='Local only; clone exact production manifests into the fixed canary route')
    for field in ('release', 'release-sha256', 'baseline-sense', 'baseline-lcd', 'out'):
        bridge.add_argument('--' + field, required=True)
    for phase in ('stage', 'promote'):
        p = sub.add_parser(phase)
        p.add_argument('--release', required=True)
        p.add_argument('--release-sha256', required=True)
        p.add_argument('--profile', required=True)
        p.add_argument('--aws-cli-python', required=True, help='Python interpreter of the installed AWS CLI v2')
        p.add_argument('--out', required=True, help='New phase evidence directory')
    args = parser.parse_args()
    if args.phase == 'prepare-bridge':
        value = prepare_bridge(args.release, args.release_sha256, args.baseline_sense, args.baseline_lcd, args.out)
        load_plan(value['path'], value['sha256'])
        print(json.dumps({'prepared_bridge': value, 'network_actions': 0, 'compiler_actions': 0}))
        return 0
    if args.phase == 'prepare':
        value = prepare(args.version, args.sense_proof, args.lcd_proof, args.baseline_sense, args.baseline_lcd, args.out, args.route, args.host_result)
        print(json.dumps({'prepared_release': value, 'network_actions': 0, 'compiler_actions': 0}))
        return 0
    plan = load_plan(args.release, args.release_sha256)
    out = Path(args.out).resolve(); out.mkdir(parents=True, exist_ok=False)
    result = {'status': 'ATTENTION_NO_AUTOMATIC_RETRY', 'phase': args.phase, 'release': ref(args.release), 'started_epoch': time.time()}
    try:
        result.update(execute(plan, Store(out, args.profile, args.aws_cli_python, plan['route']), args.phase))
        return 0
    except BaseException as exc:
        result['error_type'] = type(exc).__name__
        raise
    finally:
        result['promotion_attempted'] = [b for b in plan['publication_order'] if (out / ('promotion-attempt-' + b + '.json')).exists()]
        result['promotion_verified'] = [b for b in plan['publication_order'] if (out / ('promoted-' + b + '.json')).exists()]
        result['finished_epoch'] = time.time()
        save(out / 'result.json', result)
        print(json.dumps(result))


if __name__ == '__main__':
    raise SystemExit(main())
