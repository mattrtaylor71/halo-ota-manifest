"""Read-only production source floor; retained release binaries are unaffected."""
import datetime
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile

POLICY = 'PRODUCTION_BASELINE.json'
FIELDS = ('schema_version', 'minimum_source_commit', 'minimum_source_firmware_tree',
          'minimum_new_version', 'artifact_source_version')
HEADERS = {'halo_ota_demo/firmware/shared/Version.h': 'sense',
           'halo_ota_demo/firmware/halo_sense_prod/Version.h': 'sense',
           'halo_ota_demo/firmware/halo_lcd_prod/Version.h': 'lcd'}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(root, *args):
    try:
        return subprocess.check_output(['git', '-C', str(root), *args],
                                       stderr=subprocess.PIPE, timeout=30)
    except (OSError, subprocess.SubprocessError) as error:
        raise ValueError('Production source history unavailable or invalid: ' + str(root)) from error


def version(value):
    require(isinstance(value, str) and re.fullmatch(r'(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)', value),
            'Production version must be canonical MAJOR.MINOR.PATCH')
    result = tuple(map(int, value.split('.')))
    require(all(n <= 65535 for n in result), 'Production version exceeds uint16')
    return result


def metadata(release_version, epoch, commit):
    version(release_version)
    require(type(epoch) is int and 1577836800 <= epoch <= 4294967295,
            'Build epoch must be an explicit UTC Unix second in 2020..2106')
    require(isinstance(commit, str) and re.fullmatch(r'[0-9a-f]{40}', commit),
            'Expected exact Git commit SHA1')
    moment = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc)
    date, clock = moment.strftime('%Y-%m-%d'), moment.strftime('%H:%M:%S')
    return {'FIRMWARE_VERSION': release_version, 'BUILD_DATE': date, 'BUILD_TIME': clock,
            'BUILD_DATE_TIME_STR': date + ' ' + clock + ' UTC',
            'BUILD_GIT_HASH': commit,
            'BUILD_ID': release_version + '-' + moment.strftime('%Y%m%dT%H%M%SZ') + '-' + commit[:12]}


def version_header(original, board, values):
    """The only permitted transformation of a committed Version.h."""
    text = original.decode('utf-8')
    replacements = dict(values)
    replacements['FW_EMBED_MARKER'] = ('HALO_FW_MARKER:' + values['FIRMWARE_VERSION'] +
                                     '|BUILD_ID:' + values['BUILD_ID'] + '|BOARD:' + board)
    for key, value in replacements.items():
        text, count = re.subn(r'^#define ' + key + r' "[^"\n]*"$',
                             '#define ' + key + ' ' + json.dumps(value), text, flags=re.M)
        require(count == 1, 'Expected exactly one metadata macro: ' + key)
    text = re.sub(r'^// Generated:.*$', '// Generated from committed source; explicit build time ' +
                  values['BUILD_DATE_TIME_STR'], text, flags=re.M)
    text = re.sub(r'^// Source snapshot \(precommit, not a Git commit\):.*$',
                  '// Source commit: ' + values['BUILD_GIT_HASH'], text, flags=re.M)
    text = text.replace('tools/generate_version_header.py', 'tools/prepare_production_release.py')
    return text.encode('utf-8')


def policy(source):
    path = Path(source) / POLICY
    require(path.is_file() and not path.is_symlink(), 'Production baseline policy missing: ' + str(path))
    data = json.loads(path.read_text())
    require(type(data.get('schema_version')) is int and data['schema_version'] == 1,
            'Unsupported production baseline policy schema')
    for field in ('minimum_source_commit', 'minimum_source_firmware_tree'):
        require(isinstance(data.get(field), str) and re.fullmatch(r'[0-9a-f]{40}', data[field]),
                'Invalid production baseline ' + field)
    require(version(data['minimum_new_version']) > version(data['artifact_source_version']),
            'New production versions must follow the retained artifact version')
    return data


def committed_source(source, source_ref='HEAD', new_version=None, expected_policy=None):
    source = Path(source).resolve()
    rules = policy(source)
    if expected_policy is not None:
        require(all(rules[k] == expected_policy[k] for k in FIELDS),
                'Snapshot production baseline differs from current source policy')
    require(isinstance(source_ref, str) and source_ref and not source_ref.startswith('-'),
            'Invalid production source ref')
    repo = Path(git(source, 'rev-parse', '--show-toplevel').decode().strip())
    relative = source.relative_to(repo).as_posix()
    require(relative != '.', 'Production source must be a firmware subdirectory')
    commit = git(repo, 'rev-parse', '--verify', source_ref + '^{commit}').decode().strip()
    floor = rules['minimum_source_commit']
    floor_tree = git(repo, 'rev-parse', '--verify', floor + ':' + relative).decode().strip()
    require(floor_tree == rules['minimum_source_firmware_tree'], 'Production baseline firmware tree mismatch')
    ancestry = subprocess.run(['git', '-C', str(repo), 'merge-base', '--is-ancestor', floor, commit],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    require(ancestry.returncode == 0, 'Source does not descend from current production baseline ' + floor)
    if new_version is not None:
        require(version(new_version) >= version(rules['minimum_new_version']),
                'New production version is below minimum ' + rules['minimum_new_version'])
    return {'status': 'PASS_CURRENT_PRODUCTION_SOURCE', 'source_commit': commit,
            'source_firmware_tree': git(repo, 'rev-parse', commit + ':' + relative).decode().strip(),
            'minimum_source_commit': floor, 'minimum_new_version': rules['minimum_new_version'],
            'policy': {'path': str(source / POLICY), 'sha256': sha((source / POLICY).read_bytes())},
            'history_repo': str(repo), 'firmware_relative_path': relative}


def materialized_source(source):
    """Require the preparer's receipt, its real Git tree, and unchanged complete source."""
    source = Path(source).resolve()
    receipt = source.parent / 'materialization.json'
    require(receipt.is_file() and not receipt.is_symlink(),
            'Canonical production builds require a prepared materialization.json snapshot')
    data = json.loads(receipt.read_text())
    require(data.get('status') == 'PREPARED_COMMITTED_RELEASE_SOURCE_NOT_BUILT' and
            Path(data.get('source_root', '')).resolve() == source,
            'Materialization source identity mismatch')
    original = Path(data['original_source_root'])
    require(original.is_absolute() and original.is_dir(),
            'Original production source history is missing; restore its recorded repository')
    rules = policy(source)
    checked = committed_source(original, data['git_commit'], data['version'], rules)
    require(checked['source_commit'] == data['git_commit'] and
            checked['source_firmware_tree'] == data['git_firmware_tree'],
            'Materialization Git identity mismatch')
    archive = git(checked['history_repo'], 'archive', '--format=tar',
                  data['git_commit'], '--', checked['firmware_relative_path'])
    originals, committed_headers = {}, {}
    with tarfile.open(fileobj=io.BytesIO(archive), mode='r:') as tar:
        for item in tar:
            if item.isdir():
                continue
            name = Path(item.name)
            require(item.isfile() and not name.is_absolute() and '..' not in name.parts,
                    'Invalid committed production source member')
            name = name.relative_to(checked['firmware_relative_path']).as_posix()
            require(name not in originals, 'Duplicate committed source member')
            raw = tar.extractfile(item).read()
            originals[name] = sha(raw)
            if name in HEADERS:
                committed_headers[name] = raw
    require(originals == data.get('original_source_hashes'), 'Materialization original Git source mismatch')
    actual = {}
    for path in source.rglob('*'):
        name = path.relative_to(source)
        if any(part in ('.git', '__pycache__', '.DS_Store') for part in name.parts) or path.suffix == '.pyc':
            continue
        require(not path.is_symlink(), 'Materialized production source contains a symlink')
        if path.is_file():
            actual[name.as_posix()] = sha(path.read_bytes())
    require(actual == data.get('source_snapshot'), 'Materialized production source changed')
    require(set(actual) == set(originals) and set(data.get('generated_headers', [])) == set(HEADERS),
            'Materialization generated source scope mismatch')
    require(all(actual[p] == originals[p] for p in actual if p not in HEADERS),
            'Materialization modified non-generated production source')
    values = metadata(data['version'], data['build_epoch_utc'], data['git_commit'])
    require(values['BUILD_ID'] == data['build_id'], 'Materialization generated build identity mismatch')
    for name, board in HEADERS.items():
        require(name in committed_headers and
                (source / name).read_bytes() == version_header(committed_headers[name], board, values),
                'Materialization generated header bytes mismatch: ' + name)
    checked['materialization'] = {'path': str(receipt), 'sha256': sha(receipt.read_bytes())}
    checked['source_files_verified'] = len(actual)
    return checked
