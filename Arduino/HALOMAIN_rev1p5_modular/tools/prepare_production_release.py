#!/usr/bin/env python3
"""Prepare a versioned source snapshot from one clean, committed firmware tree.

This does not build, publish, or change the checkout. Run the snapshot's canonical
builder afterwards. The commit identifies the original source; the receipt also
records the three generated headers and every resulting source-file hash.
"""
import argparse
import datetime
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile

REQUIRED = 'tools/production_required_files.json'
HEADERS = {
    'halo_ota_demo/firmware/shared/Version.h': 'sense',
    'halo_ota_demo/firmware/halo_sense_prod/Version.h': 'sense',
    'halo_ota_demo/firmware/halo_lcd_prod/Version.h': 'lcd',
}
LOCAL_CREDENTIALS = (
    'halo_ota_demo/firmware/shared/MqttSecrets.local.h',
    'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.h',
    'halo_ota_demo/firmware/halo_sense_prod/MqttSecrets.local.cpp',
)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], timeout=60)


def metadata(version, epoch, commit):
    if not re.fullmatch(r'\d+\.\d+\.\d+', version) or any(
            len(p) > 1 and p.startswith('0') or int(p) > 65535 for p in version.split('.')):
        raise ValueError('Version must be canonical MAJOR.MINOR.PATCH with uint16 components')
    if not isinstance(epoch, int) or isinstance(epoch, bool) or not 1577836800 <= epoch <= 4294967295:
        raise ValueError('Build epoch must be an explicit UTC Unix second in 2020..2106')
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected exact Git commit SHA1')
    moment = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    # Numeric UTC format is independent of the host's locale and timezone.
    date, clock = moment.strftime('%Y-%m-%d'), moment.strftime('%H:%M:%S')
    return {'FIRMWARE_VERSION': version, 'BUILD_DATE': date, 'BUILD_TIME': clock,
            'BUILD_DATE_TIME_STR': date + ' ' + clock + ' UTC',
            'BUILD_GIT_HASH': commit,
            'BUILD_ID': version + '-' + moment.strftime('%Y%m%dT%H%M%SZ') + '-' + commit[:12]}


def version_header(original, board, values):
    text = original.decode('utf-8')
    replacements = dict(values)
    replacements['FW_EMBED_MARKER'] = ('HALO_FW_MARKER:' + values['FIRMWARE_VERSION'] +
                                     '|BUILD_ID:' + values['BUILD_ID'] + '|BOARD:' + board)
    for key, value in replacements.items():
        text, count = re.subn(r'^#define ' + key + r' "[^"\n]*"$',
                             '#define ' + key + ' ' + json.dumps(value), text, flags=re.M)
        if count != 1:
            raise ValueError('Expected exactly one metadata macro: ' + key)
    text = re.sub(r'^// Generated:.*$', '// Generated from committed source; explicit build time ' +
                  values['BUILD_DATE_TIME_STR'], text, flags=re.M)
    text = re.sub(r'^// Source snapshot \(precommit, not a Git commit\):.*$',
                  '// Source commit: ' + values['BUILD_GIT_HASH'], text, flags=re.M)
    text = text.replace('tools/generate_version_header.py', 'tools/prepare_production_release.py')
    return text.encode('utf-8')


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
