#!/usr/bin/env python3
"""Prepare a versioned source snapshot from one clean, committed firmware tree.

This does not build, publish, or change the checkout. Run the snapshot's canonical
builder afterwards. The commit identifies the original source; the receipt also
records the three generated headers and every resulting source-file hash.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import importlib.util

_guard_spec = importlib.util.spec_from_file_location('production_source_guard', Path(__file__).with_name('production_source_guard.py'))
source_guard = importlib.util.module_from_spec(_guard_spec)
_guard_spec.loader.exec_module(source_guard)

REQUIRED = 'tools/production_required_files.json'
HEADERS = source_guard.HEADERS
LOCAL_CREDENTIALS = (
    'halo_ota_demo/firmware/shared/MqttSecrets.local.h',
    'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.h',
    'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.cpp',
)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], timeout=60)


# Preparation and admission use exactly the same whole-header transformation.
metadata = source_guard.metadata
version_header = source_guard.version_header


def prepare(source, out, version, epoch):
    source, out = Path(source).resolve(), Path(out).resolve()
    repo = Path(git(source, 'rev-parse', '--show-toplevel').decode().strip())
    relative = source.relative_to(repo).as_posix()
    if relative == '.' or out == repo or repo in out.parents:
        raise ValueError('Build snapshot must be outside Git, and source must be the firmware subdirectory')
    if out.exists() or not out.parent.is_dir():
        raise ValueError('Output must be a new directory with an existing parent')
    dirty = git(repo, 'status', '--porcelain', '--untracked-files=all', '--', relative)
    if dirty:
        raise ValueError('Firmware scope contains uncommitted or untracked files')
    if any((source / p).exists() for p in LOCAL_CREDENTIALS):
        raise ValueError('Local MQTT credentials are not release source')
    commit = git(repo, 'rev-parse', 'HEAD').decode().strip()
    lineage = source_guard.committed_source(source, commit, version)
    tree = git(repo, 'rev-parse', commit + ':' + relative).decode().strip()
    values = metadata(version, epoch, commit)
    manifest_bytes = git(repo, 'show', commit + ':' + relative + '/' + REQUIRED)
    required = json.loads(manifest_bytes)['required_paths']
    if not isinstance(required, list) or not required or len(required) != len(set(required)):
        raise ValueError('Invalid required source manifest')
    archive = git(repo, 'archive', '--format=tar', commit, '--', relative)
    files, modes = {}, {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:') as tar:
        for member in tar.getmembers():
            if member.isdir():
                continue
            name = Path(member.name)
            if not member.isfile() or name.is_absolute() or '..' in name.parts:
                raise ValueError('Release archive must contain regular files only')
            name = name.relative_to(relative).as_posix()
            if name in files:
                raise ValueError('Duplicate archive path')
            files[name] = tar.extractfile(member).read()
            modes[name] = member.mode & 0o777
    if not set(required).issubset(files) or not set(HEADERS).issubset(files):
        raise ValueError('Committed tree is missing required production dependencies')
    if any(p in files for p in LOCAL_CREDENTIALS):
        raise ValueError('Committed local credentials are forbidden')
    original = {p: sha(raw) for p, raw in sorted(files.items())}
    for name, board in HEADERS.items():
        files[name] = version_header(files[name], board, values)
    resulting = {p: sha(raw) for p, raw in sorted(files.items())}
    # Everything is validated before the new output directory is created.
    out.mkdir()
    snapshot = out / 'source'
    snapshot.mkdir()
    for name, raw in files.items():
        path = snapshot / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(raw)
        path.chmod(modes[name])
    receipt = {'status': 'PREPARED_COMMITTED_RELEASE_SOURCE_NOT_BUILT',
               'source_root': str(snapshot), 'original_source_root': str(source),
               'git_commit': commit, 'git_firmware_tree': tree, 'version': version,
               'build_epoch_utc': epoch, 'build_id': values['BUILD_ID'],
               'required_manifest_sha256': sha(manifest_bytes),
               'original_source_hashes': original, 'source_snapshot': resulting,
               'generated_headers': list(HEADERS), 'uncommitted_scope_refused': True,
               'production_source_guard': lineage,
               'built': False, 'published': False}
    (out / 'materialization.json').write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', required=True)
    parser.add_argument('--epoch', type=int, required=True, help='Explicit UTC Unix build second')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    result = prepare(Path(__file__).resolve().parents[1], args.out, args.version, args.epoch)
    print(json.dumps({k: result[k] for k in ('source_root', 'git_commit', 'git_firmware_tree', 'version', 'build_id')}))


if __name__ == '__main__':
    main()
