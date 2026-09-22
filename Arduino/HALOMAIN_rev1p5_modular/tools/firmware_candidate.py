#!/usr/bin/env python3
"""Local candidate preparation/verification and read-only production status.

This tool has no flash, cloud-write, stage or promote command. The canonical
builder and regression runner remain the only build/test implementations.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import sys
import urllib.request

import prepare_production_release as prepare_tool

ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path('/Users/MattTaylor/halo-provision-memory202-20260921/service203-prep/check_release_artifacts_203.py')
CHECKER_SHA = '4e3791533f26fcf04b2d458322bb39ed0582e34c4d547b0f6bc1f9feddd8331d'
ORIGIN = 'https://halo-ota-prod.s3.us-east-1.amazonaws.com/halo/ota/prod/'


def need(ok, reason):
    if not ok:
        raise ValueError(reason)


def read(path):
    return json.loads(Path(path).read_text())


def reference(path):
    path = Path(path).resolve()
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def pinned(ref):
    need(reference(ref['path']) == ref, 'Candidate evidence changed')
    return Path(ref['path'])


def commands(source, out, version, epoch, route):
    source, out = Path(source).resolve(), Path(out).resolve()
    prepare_tool.source_guard.version(version)
    prepare_tool.source_guard.metadata(version, epoch, '0' * 40)
    policy = prepare_tool.source_guard.policy(source)
    need(prepare_tool.source_guard.version(version) >=
         prepare_tool.source_guard.version(policy['minimum_new_version']), 'Candidate version is below the production floor')
    need(route in ('production', 'private-canary'), 'Unknown candidate route')
    snapshot = out / 'snapshot/source'
    python = [sys.executable, '-B']
    build = python + [str(snapshot / 'tools/build_ota_policy_production.py'), '--out', str(out / 'build')]
    if route == 'private-canary':
        build.append('--private-canary')
    return {
        'prepare': python + [str(source / 'tools/firmware_candidate.py'), 'prepare', '--version', version,
                             '--epoch', str(epoch), '--route', route, '--out', str(out)],
        'test': python + [str(snapshot / 'tools/run_regression_suite.py'), '--out', str(out / 'regression')],
        'build': build,
        'check_artifacts': python + [str(CHECKER), '--build-root', str(out / 'build'),
                                     '--materialization', str(out / 'snapshot/materialization.json'), '--boards', 'sense', 'lcd'],
        'verify': python + [str(source / 'tools/firmware_candidate.py'), 'verify', '--candidate', str(out)],
    }


def prepare(source, out, version, epoch, route):
    source, out = Path(source).resolve(), Path(out).resolve()
    steps = commands(source, out, version, epoch, route)
    repo = Path(prepare_tool.git(source, 'rev-parse', '--show-toplevel').decode().strip())
    need(out != repo and repo not in out.parents, 'Candidate directory must be outside Git')
    need(not out.exists() and out.parent.is_dir(), 'Candidate output must be new with an existing parent')
    need(reference(CHECKER)['sha256'] == CHECKER_SHA, 'Qualified artifact checker changed')
    # A failed preparation retains its directory as evidence; never overwrite it.
    out.mkdir(mode=0o700)
    mat = prepare_tool.prepare(source, out / 'snapshot', version, epoch)
    policy = read(source / 'PRODUCTION_BASELINE.json')
    record = {
        'schema': 1, 'kind': 'LOCAL_FIRMWARE_CANDIDATE', 'version': version,
        'route': route, 'source_commit': mat['git_commit'], 'build_id': mat['build_id'],
        'materialization': reference(out / 'snapshot/materialization.json'),
        'production_at_preparation': policy.get('public_ota_version', policy['artifact_source_version']),
        'production_policy': reference(source / 'PRODUCTION_BASELINE.json'),
        'artifact_checker': reference(CHECKER), 'commands': steps,
        'published': False, 'device_acceptance': 'NOT_TESTED',
    }
    (out / 'CANDIDATE.json').write_text(json.dumps(record, indent=2) + '\n')
    text = '# Local firmware candidate ' + version + '\n\n'
    text += 'Preparation did not build, flash, or publish. Production is unchanged.\n\n'
    text += 'Run these separate local steps in order (no publisher is invoked):\n\n'
    for key in ('test', 'build', 'check_artifacts', 'verify'):
        text += '```sh\n' + shlex.join(steps[key]) + '\n```\n\n'
    text += 'Then follow docs/BENCH_CANDIDATE_FLASH.md for the selected physical unit.\n'
    text += 'Record actual device results separately; a host pass is not device acceptance.\n'
    text += 'Only a later explicit user release request authorizes production publication.\n'
    (out / 'NEXT_STEPS.md').write_text(text)
    return record


def verify(out):
    out = Path(out).resolve()
    candidate = read(out / 'CANDIDATE.json')
    need(candidate.get('schema') == 1 and candidate.get('kind') == 'LOCAL_FIRMWARE_CANDIDATE', 'Not a candidate workspace')
    mat_path = pinned(candidate['materialization'])
    need(mat_path == out / 'snapshot/materialization.json', 'Candidate snapshot moved')
    mat = read(mat_path)
    need((mat['version'], mat['git_commit'], mat['build_id']) ==
         (candidate['version'], candidate['source_commit'], candidate['build_id']), 'Candidate identity differs')
    snapshot = out / 'snapshot/source'
    # Check all source bytes/history before importing the snapshot validator.
    prepare_tool.source_guard.materialized_source(snapshot)
    publisher = snapshot / 'halo_ota_demo/tools/ota/publish_pair.py'
    sys.path.insert(0, str(publisher.parent))
    try:
        spec = importlib.util.spec_from_file_location('candidate_publisher', publisher)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        proofs = {b: module.shipping_proof(out / 'build' / b / 'artifacts/verified.json', b,
                                          candidate['version'], candidate['route']) for b in ('sense', 'lcd')}
        need(all(p['source_manifest'] == candidate['materialization'] for p in proofs.values()), 'Candidate/build snapshot differs')
        host_ref = module.validate_host_result(out / 'regression/RESULT.json', proofs)
    finally:
        sys.path.pop(0)
    check_path = out / 'build/artifact-result-v2-sense-lcd.json'
    check = read(check_path)
    need(check['status'] == 'PASS_COMMITTED_SHIPPING_RELEASE_ARTIFACTS' and
         set(check['boards']) == {'sense', 'lcd'}, 'Paired artifact check required')
    need(check['profiles'] == {'shipping_' + b: reference(out / 'build' / b / 'artifacts/verified.json')
                              for b in ('sense', 'lcd')}, 'Artifact checker proofs differ')
    need(check['checker'] == candidate['artifact_checker'] and
         reference(pinned(candidate['artifact_checker']))['sha256'] == CHECKER_SHA,
         'Qualified artifact checker differs')
    return {'status': 'READY_FOR_BENCH_TEST_NOT_PRODUCTION_APPROVAL', 'version': candidate['version'],
            'route': candidate['route'], 'candidate': reference(out / 'CANDIDATE.json'),
            'host_result': host_ref, 'artifact_check': reference(check_path),
            'artifacts': {b: proofs[b]['artifact'] for b in proofs},
            'device_acceptance': 'CONSULT_SEPARATE_DEVICE_EVIDENCE', 'cloud_writes': 0, 'hardware_actions': 0}


def fetch(url):
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            raise ValueError('Production manifest redirect refused')
    opener = urllib.request.build_opener(NoRedirect())
    with opener.open(urllib.request.Request(url, headers={'Cache-Control': 'no-cache'}), timeout=15) as response:
        need(response.status == 200 and response.geturl() == url, 'Unexpected production response')
        raw = response.read(65537)
        need(len(raw) <= 65536, 'Production manifest too large')
        return raw


def production_status(source, getter=fetch):
    policy = read(Path(source) / 'PRODUCTION_BASELINE.json')
    expected = policy['public_readback']['boards']
    boards = {}
    for board in ('sense', 'lcd'):
        url = ORIGIN + ('lcd/' if board == 'lcd' else '') + 'manifest_latest.json'
        raw = getter(url)
        need(hashlib.sha256(raw).hexdigest() == expected[board]['manifest_sha256'],
             'Production latest differs from recorded release: ' + board)
        manifest = json.loads(raw)
        need(manifest['board'] == board and manifest['version'] == policy['public_ota_version'] and
             manifest['sha256'] == expected[board]['sha256'] and manifest['size'] == expected[board]['bytes'],
             'Production identity differs: ' + board)
        boards[board] = {'version': manifest['version'], 'sha256': manifest['sha256'], 'url': url}
    return {'status': 'PASS_RECORDED_PRODUCTION_LATEST', 'boards': boards,
            'cloud_writes': 0, 'hardware_actions': 0}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    for name in ('plan', 'prepare'):
        p = sub.add_parser(name)
        p.add_argument('--version', required=True)
        p.add_argument('--epoch', required=True, type=int)
        p.add_argument('--out', required=True, type=Path)
        p.add_argument('--route', choices=('production', 'private-canary'), default='production',
                       help='Compiled routing only; neither choice publishes')
    sub.add_parser('production-status', help='Read only: verify public latest still matches the recorded release')
    sub.add_parser('verify', help='Validate built local candidate; does not approve a release').add_argument('--candidate', type=Path, required=True)
    args = parser.parse_args()
    if args.action == 'production-status':
        result = production_status(ROOT)
    elif args.action == 'verify':
        result = verify(args.candidate)
    elif args.action == 'plan':
        result = {'commands': commands(ROOT, args.out, args.version, args.epoch, args.route),
                  'cloud_writes': 0, 'hardware_actions': 0, 'local_writes': 0}
    else:
        result = prepare(ROOT, args.out, args.version, args.epoch, args.route)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
