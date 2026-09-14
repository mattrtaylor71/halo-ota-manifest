#!/usr/bin/env python3
"""Read-only source ancestry and frozen release-byte preflight. No device access."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def verify_file(path, expected, size=None):
    require(path.is_file(), "Missing retained file: {}".format(path))
    if size is not None:
        require(path.stat().st_size == size, "Size mismatch: {}".format(path))
    require(digest(path) == expected, "SHA-256 mismatch: {}".format(path))


def git(root, *args):
    result = subprocess.run(["git", "-C", str(root)] + list(args),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True)
    require(result.returncode == 0, "Git check failed: {}".format(result.stderr.strip()))
    return result.stdout.strip()


def verify_package(package, baseline):
    recorded = baseline["frozen_package"]
    verify_file(package / "MANIFEST.json", recorded["manifest"]["sha256"])
    verify_file(package / "SHA256SUMS", recorded["checksums"]["sha256"])
    seen = set()
    for line in (package / "SHA256SUMS").read_text().splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        require(match is not None, "Malformed package checksum line")
        expected, name = match.groups()
        relative = PurePosixPath(name)
        require(not relative.is_absolute() and ".." not in relative.parts,
                "Unsafe package member: {}".format(name))
        require(name not in seen, "Duplicate package member: {}".format(name))
        seen.add(name)
        path = package.joinpath(*relative.parts).resolve()
        require(package in path.parents, "Package member escapes root: {}".format(name))
        verify_file(path, expected)
    require(bool(seen), "Empty package checksums")
    inventory = json.loads((package / "MANIFEST.json").read_text())
    require(inventory["version"] == baseline["version"], "Package version mismatch")
    require(inventory["source_commit"] == baseline["artifact_source_commit"],
            "Package source mismatch")
    return len(seen)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-ref", default="HEAD",
                        help="Committed source to check; default HEAD. Does not check dirty edits.")
    parser.add_argument("--package", type=Path,
                        help="Verify a frozen package and use its applications instead of build files.")
    args = parser.parse_args()
    firmware = Path(__file__).resolve().parents[1]
    record = json.loads((firmware / "RELEASE_BASELINE.json").read_text())
    baseline = record["current_baseline"]
    repo = Path(git(firmware, "rev-parse", "--show-toplevel"))
    require(not args.source_ref.startswith("-"), "Source ref must not be a Git option")
    tag = "refs/tags/" + baseline["release_tag"]
    commit = git(repo, "rev-parse", "--verify", tag + "^{commit}")
    require(commit == baseline["artifact_source_commit"], "Frozen tag target changed")
    require(git(repo, "rev-parse", "--verify", tag) == baseline["release_tag_object"],
            "Frozen annotated tag changed")
    tree = git(repo, "rev-parse", commit + ":" + firmware.relative_to(repo).as_posix())
    require(tree == baseline["artifact_source_firmware_tree"], "Frozen firmware tree mismatch")
    candidate = git(repo, "rev-parse", "--verify", args.source_ref + "^{commit}")
    ancestry = subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor",
                               commit, candidate])
    require(ancestry.returncode == 0,
            "Source {} does not descend from frozen {} ({})".format(
                args.source_ref, baseline["version"], commit))
    require(record["development_baseline"]["version"] == baseline["version"],
            "Current/development baseline versions disagree")
    checked = 0
    package = args.package.resolve() if args.package else None
    if package:
        checked = verify_package(package, baseline)
    for board, artifact in baseline["applications"].items():
        path = package / artifact["package_path"] if package else Path(artifact["path"])
        verify_file(path, artifact["sha256"], artifact["bytes"])
    print(json.dumps({"status": "PASS_FROZEN_BASELINE_PREFLIGHT",
                      "version": baseline["version"], "source_ref": args.source_ref,
                      "source_commit": candidate, "applications_verified": 2,
                      "package_files_verified": checked,
                      "scope": "Committed ancestry and retained bytes only; no dirty-source or device acceptance."},
                     indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("REFUSED: {}".format(error), file=sys.stderr)
        sys.exit(1)
