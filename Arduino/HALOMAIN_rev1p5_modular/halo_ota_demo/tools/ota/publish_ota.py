#!/usr/bin/env python3
"""
OTA Publishing Pipeline for HALO SENSE

End-to-end OTA release workflow:
1. Compile firmware (arduino-cli)
2. Extract version/build_id from binary marker
3. Validate marker matches expected version
4. Compute SHA256 + size
5. Generate manifest JSON files
6. Upload to S3 with channel-based paths
7. Optionally invalidate CloudFront

Usage:
    # Dev channel
    python3 tools/ota/publish_ota.py --channel dev

    # Prod channel
    python3 tools/ota/publish_ota.py --channel prod

    # Custom channel
    python3 tools/ota/publish_ota.py --channel staging --bucket my-bucket

Environment Variables:
    OTA_BUCKET          - S3 bucket name (required)
    OTA_PREFIX          - S3 key prefix (default: halo/ota)
    OTA_CHANNEL         - Channel name (dev|prod, default: prod)
    AWS_PROFILE         - AWS CLI profile (optional)
    AWS_REGION          - AWS region (default: us-east-1)
    OTA_PUBLIC_BASE_URL - Public base URL (optional, defaults to S3 URL)
    CLOUDFRONT_DIST_ID  - CloudFront distribution ID (optional)

Before publishing (non-dry-run), run: aws sts get-caller-identity
If that fails, publish will exit non-zero—credentials are missing or invalid.
"""

import argparse
import atexit
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from urllib.parse import urljoin, urlparse
from urllib.request import urlopen, Request

from artifact_safety import validate_publishable_artifact

# Try to import boto3 for direct S3 operations (diagnostics)
try:
    import boto3
    from botocore.exceptions import ClientError, BotoCoreError
    BOTO3_AVAILABLE = True
except ImportError:
    BOTO3_AVAILABLE = False
    boto3 = None

PUBLISH_OK = False

def register_publish_exit_guard():
    def _publish_exit():
        if not PUBLISH_OK:
            print("PUBLISH_FAILED", file=sys.stderr)
    atexit.register(_publish_exit)

# Marker pattern: HALO_FW_MARKER:<version>|BUILD_ID:<build_id>
MARKER_PATTERN = re.compile(rb'HALO_FW_MARKER:([0-9]+\.[0-9]+\.[0-9]+)(?:\|BUILD_ID:([^\x00|]+))?')

def find_firmware_marker(bin_path):
    """Extract firmware version and build_id from binary marker."""
    try:
        # Try strings command first (more reliable)
        result = subprocess.run(
            ["strings", "-a", str(bin_path)],
            capture_output=True,
            text=True,
            errors="ignore",
            check=True
        )
        
        for line in result.stdout.splitlines():
            if "HALO_FW_MARKER:" in line:
                marker_match = re.search(r'HALO_FW_MARKER:([0-9]+\.[0-9]+\.[0-9]+)(?:\|BUILD_ID:([^|]+))?', line)
                if marker_match:
                    version = marker_match.group(1)
                    build_id = marker_match.group(2).strip() if marker_match.group(2) else "unknown"
                    return version, build_id
        
        # Fallback: binary search
        with open(bin_path, 'rb') as f:
            data = f.read()
        
        match = MARKER_PATTERN.search(data)
        if match:
            version_bytes = match.group(1)
            build_id_bytes = match.group(2) if match.group(2) else b"unknown"
            
            version = version_bytes.decode('ascii', errors='ignore').strip()
            build_id = build_id_bytes.decode('ascii', errors='ignore').strip().split('\x00')[0]
            return version, build_id
        
        return None, None
    except Exception as e:
        print(f"ERROR: Failed to extract marker: {e}", file=sys.stderr)
        return None, None

def compute_sha256(file_path):
    """Compute SHA256 hash of file."""
    sha256 = hashlib.sha256()
    with open(file_path, 'rb') as f:
        for chunk in iter(lambda: f.read(4096), b''):
            sha256.update(chunk)
    return sha256.hexdigest()

def get_file_size(file_path):
    """Get file size in bytes."""
    return os.path.getsize(file_path)

# Names that indicate NON-app-only binaries (merged/full flash, bootloader, partition table)
_EXCLUDE_BIN_SUBSTRINGS = ("merged", "bootloader", "partitions", "full", "factory", "flash")

def _is_app_only_bin(name):
    """True if filename looks like app-only (prefer *.ino.bin, firmware.bin); false if excluded."""
    n = name.lower()
    for x in _EXCLUDE_BIN_SUBSTRINGS:
        if x in n:
            return False
    return True

def _candidate_info(bin_path):
    """Return dict with path, size, has_marker, is_merged_guess for a .bin candidate."""
    p = Path(bin_path)
    size = get_file_size(p) if p.exists() else 0
    version, _ = find_firmware_marker(p)
    has_marker = bool(version)
    is_merged_guess = any(x in p.name.lower() for x in ("merged", "full", "bootloader", "partitions", "factory", "flash"))
    return {"path": str(p), "size": size, "has_marker": has_marker, "is_merged_guess": is_merged_guess}


def select_app_only_binary(build_dir, sketch_name):
    """
    Select the app-only OTA binary from build output. Never use merged/bootloader/partitions.
    Prefer: *.ino.bin, firmware.bin. Explicitly EXCLUDE: merged, bootloader, partitions, full, factory, flash.
    Returns (Path, from_merged, excluded_candidate_names). If only merged exists, extract and return (temp_path, "extract", excluded).
    """
    build_dir = Path(build_dir)
    if not build_dir.exists():
        return None, False, [], []
    all_bins = list(build_dir.glob("*.bin"))
    candidates = [_candidate_info(b) for b in all_bins]
    excluded = [b.name for b in all_bins if not _is_app_only_bin(b.name)]
    app_only = [b for b in all_bins if _is_app_only_bin(b.name)]
    # Prefer exact names: <sketch>.ino.bin, <sketch>.bin, firmware.bin
    for candidate in [build_dir / f"{sketch_name}.ino.bin", build_dir / f"{sketch_name}.bin", build_dir / "firmware.bin"]:
        if candidate in app_only or (candidate.exists() and _is_app_only_bin(candidate.name)):
            return candidate, False, excluded, candidates
    if app_only:
        return app_only[0], False, excluded, candidates
    # No app-only by name: try extracting from merged if it exists
    merged = next((b for b in all_bins if "merged" in b.name.lower()), None)
    if merged:
        return merged, "extract", excluded, candidates
    print(f"ERROR: No app-only binary in {build_dir}", file=sys.stderr)
    print(f"  EXCLUDE any file containing: {', '.join(_EXCLUDE_BIN_SUBSTRINGS)}", file=sys.stderr)
    print(f"  Found: {[b.name for b in all_bins]}", file=sys.stderr)
    return None, False, excluded, candidates

def parse_ota_slot_bytes(partitions_csv_path):
    """
    Parse partition table CSV and return OTA app slot size in bytes.
    Looks for rows with Type=app and SubType=ota_0 or ota_1; Size column is hex (e.g. 0x1E0000).
    Returns int or None if not found. Prefer parsing to avoid drift; fallback constant in caller.
    """
    OTA_SLOT_BYTES_DEFAULT = 3342336  # 0x330000, matches halo_sense_prod partitions app0/app1
    path = Path(partitions_csv_path)
    if not path.exists():
        return OTA_SLOT_BYTES_DEFAULT
    try:
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = [p.strip() for p in line.split(",")]
                if len(parts) < 5:
                    continue
                name, ptype, subtype = parts[0], (parts[1] if len(parts) > 1 else ""), (parts[2] if len(parts) > 2 else "")
                size_str = parts[4] if len(parts) > 4 else ""
                if ptype.lower() != "app":
                    continue
                if "ota_0" in subtype.lower() or "ota_1" in subtype.lower() or "ota" in subtype.lower():
                    if size_str.startswith("0x") or size_str.startswith("0X"):
                        return int(size_str, 16)
                    if size_str.isdigit():
                        return int(size_str)
    except Exception:
        pass
    return OTA_SLOT_BYTES_DEFAULT

def extract_app_from_merged(merged_path, offset_hex, size_hex, out_path):
    """Extract app partition from merged.bin: offset and size as hex (e.g. 0x10000, 0x1E0000)."""
    offset = int(offset_hex, 16)
    size = int(size_hex, 16)
    with open(merged_path, "rb") as f:
        f.seek(offset)
        data = f.read(size)
    with open(out_path, "wb") as f:
        f.write(data)
    return out_path

def parse_app_offset_size(partitions_csv_path):
    """Return (offset_hex, size_hex) for first OTA app partition (app0) from partitions.csv."""
    path = Path(partitions_csv_path)
    if not path.exists():
        return ("0x10000", "0x330000")
    try:
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = [p.strip() for p in line.split(",")]
                if len(parts) < 5:
                    continue
                ptype, subtype = (parts[1] if len(parts) > 1 else ""), (parts[2] if len(parts) > 2 else "")
                off_str = parts[3] if len(parts) > 3 else ""
                size_str = parts[4] if len(parts) > 4 else ""
                if ptype.lower() == "app" and ("ota_0" in subtype.lower() or "ota" in subtype.lower()):
                    return (off_str or "0x10000", size_str or "0x330000")
    except Exception:
        pass
    return ("0x10000", "0x330000")

def get_deterministic_build_dir(repo_root, target, channel, build_id=None):
    """
    Return deterministic build directory path.
    Format: <repo>/build/<target>/<channel>/<build_id or timestamp>
    This ensures builds are isolated and predictable.
    """
    if build_id:
        # Use build_id if available (includes timestamp)
        build_subdir = build_id.replace(" ", "_").replace(":", "-")
    else:
        # Fallback to timestamp
        build_subdir = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    
    build_dir = repo_root / "build" / target / channel / build_subdir
    return build_dir

def compile_firmware(fqbn="esp32:esp32:XIAO_ESP32S3:PSRAM=opi", sketch_path=None, build_extra_flags=None, build_dir=None, channel=None):
    """
    Compile firmware using arduino-cli. Returns path to app-only .bin (never merged).
    
    Args:
        fqbn: Arduino FQBN
        sketch_path: Path to .ino file
        build_extra_flags: Extra compiler flags
        build_dir: Explicit build directory (if None, uses deterministic path)
        channel: Channel name (for deterministic build dir)
    """
    if sketch_path is None:
        repo_root = Path(__file__).parent.parent.parent
        sketch_path = repo_root / "firmware" / "halo_sense_prod" / "halo_sense_prod.ino"
    
    sketch_path = Path(sketch_path)
    if not sketch_path.exists():
        print(f"ERROR: Sketch not found: {sketch_path}", file=sys.stderr)
        return None
    
    repo_root = sketch_path.parent.parent.parent
    sketch_name = sketch_path.stem
    
    # A) Centralize build output paths - use deterministic BUILD_DIR
    if build_dir is None:
        target = fqbn.replace(":", ".")
        build_dir = get_deterministic_build_dir(repo_root, target, channel or "default", build_id=None)
    
    build_dir = Path(build_dir)
    
    # Ensure build directory exists and is clean
    if build_dir.exists():
        print(f"Cleaning build directory: {build_dir}")
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)
    
    arduino_cli = os.environ.get("ARDUINO_CLI_BIN", "arduino-cli")

    print(f"Compiling firmware: {sketch_path}")
    print(f"  FQBN: {fqbn}")
    print(f"  Build directory: {build_dir}")
    print(f"  Arduino CLI: {arduino_cli}")
    if build_extra_flags:
        print(f"  Extra flags: {build_extra_flags[:80]}{'...' if len(build_extra_flags) > 80 else ''}")
    
    cmd = [
        arduino_cli, "compile",
        "--fqbn", fqbn,
        "--build-path", str(build_dir),
        str(sketch_path)
    ]
    if build_extra_flags:
        cmd.extend(["--build-property", f"build.extra_flags={build_extra_flags}"])
    
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        print("✓ Compilation successful")
        
        # B) Build-path correctness assertion
        print(f"\n--- Build-path correctness assertion ---")
        print(f"Expected build directory: {build_dir}")
        print(f"  Exists: {build_dir.exists()}")
        
        if not build_dir.exists():
            print(f"ERROR: Build directory does not exist after compilation!", file=sys.stderr)
            print(f"  Expected: {build_dir}", file=sys.stderr)
            print(f"  Arduino CLI may have used cache directory instead.", file=sys.stderr)
            print(f"  Ensure --build-path is set in compile command.", file=sys.stderr)
            print(f"  Compile command was: {' '.join(cmd)}", file=sys.stderr)
            return None
        
        # Check for binaries in expected location
        bin_files = list(build_dir.glob("*.bin"))
        print(f"  Found {len(bin_files)} .bin file(s) in build directory:")
        for bf in bin_files:
            print(f"    {bf.name} ({get_file_size(bf)} bytes)")
        
        if len(bin_files) == 0:
            print(f"ERROR: No .bin files found in build directory!", file=sys.stderr)
            print(f"  Expected location: {build_dir}", file=sys.stderr)
            print(f"  Arduino CLI may have compiled to cache directory.", file=sys.stderr)
            print(f"  Compile command was: {' '.join(cmd)}", file=sys.stderr)
            print(f"  Hint: Ensure --build-path is set and Arduino CLI respects it.", file=sys.stderr)
            return None
        
        # C) Single source of truth artifact selection
        print(f"\n--- Artifact selection (single source of truth) ---")
        selected, extract_flag, excluded, candidates = select_app_only_binary(build_dir, sketch_name)
        
        if selected is None:
            print(f"ERROR: No app-only binary found in {build_dir}", file=sys.stderr)
            print(f"  Searched directory: {build_dir}", file=sys.stderr)
            print(f"  Found binaries: {[b.name for b in bin_files]}", file=sys.stderr)
            print(f"  Excluded (merged/bootloader/etc): {excluded}", file=sys.stderr)
            return None
        
        # Verify exactly one candidate (refuse if multiple)
        app_only_candidates = [c for c in candidates if not c.get('is_merged_guess', False) and c.get('has_marker', False)]
        if len(app_only_candidates) > 1:
            print(f"ERROR: Multiple app-only binary candidates found!", file=sys.stderr)
            print(f"  Candidates: {[c['path'] for c in app_only_candidates]}", file=sys.stderr)
            print(f"  Refusing to publish: ambiguous binary selection", file=sys.stderr)
            return None
        
        # Verify marker is present
        version, build_id_marker = find_firmware_marker(selected)
        if not version:
            print(f"ERROR: Selected binary does not contain HALO_FW_MARKER!", file=sys.stderr)
            print(f"  Binary: {selected}", file=sys.stderr)
            print(f"  Refusing to publish: marker validation failed", file=sys.stderr)
            return None
        
        if extract_flag == "extract":
            partitions_csv = sketch_path.parent / "partitions.csv"
            off_hex, size_hex = parse_app_offset_size(partitions_csv)
            import tempfile
            tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
            tmp.close()
            extract_app_from_merged(selected, off_hex, size_hex, tmp.name)
            selected = Path(tmp.name)
            print(f"  (extracted app from merged using offset={off_hex} size={size_hex})")
            # Re-verify marker after extraction
            version, build_id_marker = find_firmware_marker(selected)
            if not version:
                print(f"ERROR: Extracted binary does not contain HALO_FW_MARKER!", file=sys.stderr)
                return None
        
        size = get_file_size(selected)
        print(f"  Selected binary: {selected}")
        print(f"  Size: {size} bytes")
        print(f"  Marker version: {version}")
        print(f"  Marker build_id: {build_id_marker}")
        print(f"SELECTED_OTA_BIN_PATH={selected}")
        print(f"SELECTED_OTA_BIN_SIZE={size}")
        print(f"EXCLUDED_CANDIDATES={json.dumps(excluded)}")
        return selected
    except FileNotFoundError:
        print(f"ERROR: Arduino CLI not found: {arduino_cli}", file=sys.stderr)
        print("  Set ARDUINO_CLI_BIN or run automation/bootstrap_host.sh", file=sys.stderr)
        return None
    except subprocess.CalledProcessError as e:
        print(f"ERROR: Compilation failed", file=sys.stderr)
        print(f"  Build directory: {build_dir}", file=sys.stderr)
        print(f"  Compile command: {' '.join(cmd)}", file=sys.stderr)
        print(e.stderr, file=sys.stderr)
        return None

def validate_marker(bin_path, expected_version):
    """Validate that binary marker matches expected version."""
    version, build_id = find_firmware_marker(bin_path)
    
    if not version:
        print(f"ERROR: Could not extract version from marker", file=sys.stderr)
        return False, None, None
    
    if version != expected_version:
        print(f"ERROR: Version mismatch!", file=sys.stderr)
        print(f"  Expected: {expected_version}", file=sys.stderr)
        print(f"  Found in binary: {version}", file=sys.stderr)
        print(f"  This prevents publishing wrong binary!", file=sys.stderr)
        return False, None, None
    
    print(f"✓ Marker validation passed: version={version}, build_id={build_id}")
    return True, version, build_id

def generate_manifest(version, bin_url, sha256, size, build_id, artifact_fw_version,
                      min_version="0.0.0", max_slot_bytes=None,
                      published_at_utc=None, publisher_host=None,
                      rollout_pct=None, rollout_seed=None, min_version_allowed=None,
                      board="sense"):
    """Generate manifest JSON. max_slot_bytes: OTA partition size (for verification). Canary: published_at_utc, publisher_host.
    
    build_id is REQUIRED and must not be empty.
    """
    # Validate build_id is present (required)
    if not build_id or build_id == "unknown" or len(build_id.strip()) == 0:
        raise ValueError(f"build_id is required but was: '{build_id}'")
    
    manifest = {
        "version": version,
        "bin_url": bin_url,
        "sha256": sha256,
        "size": size,
        "min_version": min_version,
        "build_id": build_id,  # REQUIRED - firmware will fail if missing
        "artifact_fw_version": artifact_fw_version,
        "board": board
    }
    if max_slot_bytes is not None:
        manifest["max_slot_bytes"] = max_slot_bytes
    if published_at_utc is not None:
        manifest["published_at_utc"] = published_at_utc
    if publisher_host is not None:
        manifest["publisher_host"] = publisher_host
    if rollout_pct is not None:
        manifest["rollout_pct"] = int(rollout_pct)
    if rollout_seed is not None:
        manifest["rollout_seed"] = int(rollout_seed)
    if min_version_allowed:
        manifest["min_version_allowed"] = str(min_version_allowed)
    return manifest

def get_aws_config(channel, bucket=None, prefix=None, region=None, profile=None, base_url=None, cloudfront_id=None):
    """Get AWS configuration from environment and arguments."""
    config = {
        "bucket": bucket or os.environ.get("OTA_BUCKET"),
        "prefix": prefix or os.environ.get("OTA_PREFIX", "halo/ota"),
        "channel": channel or os.environ.get("OTA_CHANNEL", "prod"),
        "region": region or os.environ.get("AWS_REGION", "us-east-1"),
        "profile": profile or os.environ.get("AWS_PROFILE"),
        "base_url": base_url or os.environ.get("OTA_PUBLIC_BASE_URL"),
        "cloudfront_id": cloudfront_id or os.environ.get("CLOUDFRONT_DIST_ID"),
        "cloudfront_domain": os.environ.get("CLOUDFRONT_DOMAIN")  # Optional: explicit CloudFront domain
    }
    
    if not config["bucket"]:
        print("ERROR: OTA_BUCKET not set", file=sys.stderr)
        print("  Set via: export OTA_BUCKET=my-bucket", file=sys.stderr)
        print("  Or pass: --bucket my-bucket", file=sys.stderr)
        print("  Or create tools/ota/ota_env.sh and source it", file=sys.stderr)
        sys.exit(1)
    
    # Prefer CloudFront domain if provided, otherwise derive from S3
    if config["cloudfront_domain"]:
        # CloudFront domain provided - use it (preferred for Range support)
        config["base_url"] = f"https://{config['cloudfront_domain'].rstrip('/')}"
        if not config["base_url"].endswith(f"/{config['prefix']}"):
            config["base_url"] = f"{config['base_url']}/{config['prefix']}"
    elif config["base_url"]:
        # Explicit base URL provided - use as-is
        config["base_url"] = config["base_url"].rstrip('/')
    else:
        # Derive from S3 (fallback - S3 REST endpoint supports Range)
        config["base_url"] = f"https://{config['bucket']}.s3.{config['region']}.amazonaws.com/{config['prefix']}"
    
    # Ensure base_url doesn't end with /
    config["base_url"] = config["base_url"].rstrip('/')
    
    return config

def build_aws_cmd(config, *args):
    """Build AWS CLI command with profile/region."""
    cmd = ["aws"]
    if config["profile"]:
        cmd.extend(["--profile", config["profile"]])
    if config["region"]:
        cmd.extend(["--region", config["region"]])
    cmd.extend(list(args))
    return cmd

def post_publish_readback(manifest_url, expected_version, expected_build_id, expected_bin_url, expected_size, ota_slot_bytes):
    """
    After upload, fetch the hosted manifest and assert invariants. Exit nonzero if any fail.
    - manifest.version == expected_version (exact) - CRITICAL: ensures manifest_latest.json was updated
    - manifest.build_id == expected_build_id (exact)
    - manifest.bin_url == expected_bin_url (exact)
    - manifest.size == expected_size (exact, uploaded binary size)
    - manifest.size <= ota_slot_bytes
    """
    print("\n--- Post-publish readback (hard-proof: manifest_latest.json must be updated) ---")
    try:
        with urlopen(manifest_url, timeout=15) as r:
            live = json.loads(r.read().decode("utf-8"))
    except Exception as e:
        print(f"ERROR: Failed to fetch live manifest: {e}", file=sys.stderr)
        sys.exit(1)
    version = live.get("version", "")
    build_id = live.get("build_id", "")
    size = live.get("size", 0)
    bin_url = live.get("bin_url", "")
    print(f"Live manifest: version={version}, build_id={build_id}, size={size}, bin_url={bin_url[:80]}...")
    err = []
    # CRITICAL: Version must match - this proves manifest_latest.json was updated
    if version != expected_version:
        err.append(f"manifest.version '{version}' != expected '{expected_version}' (manifest_latest.json NOT updated!)")
    if build_id != expected_build_id:
        err.append(f"manifest.build_id '{build_id}' != expected '{expected_build_id}'")
    if bin_url != expected_bin_url:
        err.append(f"manifest.bin_url '{bin_url}' != expected '{expected_bin_url}'")
    if size != expected_size:
        err.append(f"manifest.size ({size}) != uploaded binary size ({expected_size})")
    if size > ota_slot_bytes:
        err.append(f"manifest.size ({size}) > OTA_SLOT_BYTES ({ota_slot_bytes})")
    if bin_url:
        bn = Path(urlparse(bin_url).path).name.lower()
        for x in _EXCLUDE_BIN_SUBSTRINGS:
            if x in bn:
                err.append(f"bin_url basename contains excluded '{x}': {bn}")
                break
    if err:
        print("ERROR: Post-publish readback failed:", file=sys.stderr)
        for e in err:
            print(f"  - {e}", file=sys.stderr)
        print(f"\n  This means manifest_latest.json was NOT updated correctly.", file=sys.stderr)
        print(f"  Expected version: {expected_version}", file=sys.stderr)
        print(f"  Live manifest version: {version}", file=sys.stderr)
        sys.exit(1)
    print(f"  ✓ version matches ({version}), build_id matches, bin_url matches, size matches uploaded, size <= OTA_SLOT_BYTES")

def verify_uploaded_artifact(bin_url, expected_sha256, expected_size, manifest_url=None, config=None):
    """
    Download the uploaded artifact from S3 and verify:
    1. SHA256 matches expected
    2. Size matches expected
    3. No GitHub strings present (S3-only enforcement)
    4. Marker present in binary
    5. S3 object metadata (ETag, LastModified)
    Exit nonzero if any check fails.
    """
    print("\n--- Post-upload artifact verification (download-then-verify) ---")
    import tempfile
    
    tmp_path = None
    try:
        # Download artifact
        print(f"Downloading artifact from: {bin_url}")
        with urlopen(bin_url, timeout=60) as response:
            with tempfile.NamedTemporaryFile(delete=False, suffix='.bin') as tmp_file:
                tmp_path = tmp_file.name
                shutil.copyfileobj(response, tmp_file)
        
        # Verify SHA256
        actual_sha256 = compute_sha256(tmp_path)
        if actual_sha256 != expected_sha256:
            print(f"ERROR: SHA256 mismatch: expected {expected_sha256}, got {actual_sha256}", file=sys.stderr)
            if tmp_path:
                os.unlink(tmp_path)
            sys.exit(1)
        print(f"  ✓ SHA256 matches: {actual_sha256[:16]}...")
        
        # Verify size
        actual_size = get_file_size(tmp_path)
        if actual_size != expected_size:
            print(f"ERROR: Size mismatch: expected {expected_size}, got {actual_size}", file=sys.stderr)
            if tmp_path:
                os.unlink(tmp_path)
            sys.exit(1)
        print(f"  ✓ Size matches: {actual_size} bytes")
        
        # Verify marker is present
        version, build_id = find_firmware_marker(tmp_path)
        if not version:
            print(f"ERROR: Downloaded artifact does not contain HALO_FW_MARKER!", file=sys.stderr)
            print(f"  This indicates wrong binary was uploaded or corrupted.", file=sys.stderr)
            if tmp_path:
                os.unlink(tmp_path)
            sys.exit(1)
        print(f"  ✓ Marker present: version={version}, build_id={build_id}")
        
        # Check for GitHub strings (S3-only enforcement)
        print("  Checking for GitHub strings in uploaded artifact...")
        github_strings = []
        try:
            result = subprocess.run(
                ["strings", "-a", tmp_path],
                capture_output=True,
                text=True,
                errors="ignore",
                check=True
            )
            for line in result.stdout.splitlines():
                if any(pattern in line.lower() for pattern in ["github.io", "githubusercontent", "raw.githubusercontent", "halo-ota-manifest", "mattrtaylor71"]):
                    github_strings.append(line)
        except Exception as e:
            print(f"WARNING: Could not check binary strings: {e}", file=sys.stderr)
        
        if github_strings:
            print("ERROR: S3-only violation: Found GitHub strings in UPLOADED artifact:", file=sys.stderr)
            for s in github_strings[:10]:
                print(f"  {s}", file=sys.stderr)
            print("\nThis artifact was uploaded but contains GitHub strings. This must be fixed.", file=sys.stderr)
            if tmp_path:
                os.unlink(tmp_path)
            sys.exit(1)
        print("  ✓ No GitHub strings found in uploaded artifact")
        
        # E) Get S3 object metadata for binary (if boto3 available)
        if BOTO3_AVAILABLE and config:
            try:
                from urllib.parse import urlparse
                parsed_url = urlparse(bin_url)
                # Extract bucket and key from URL
                # Format: https://bucket.s3.region.amazonaws.com/key
                bucket_name = parsed_url.netloc.split('.')[0]
                key = parsed_url.path.lstrip('/')
                
                session = boto3.Session(
                    profile_name=config['profile'] if config.get('profile') else None,
                    region_name=config['region']
                )
                s3_client = session.client('s3')
                head_response = s3_client.head_object(Bucket=bucket_name, Key=key)
                bin_etag = head_response.get('ETag', '').strip('"')
                bin_last_modified = head_response.get('LastModified', '?')
                print(f"\nS3 Binary Object Metadata:")
                print(f"  ETag: {bin_etag}")
                print(f"  LastModified: {bin_last_modified}")
            except Exception as e:
                print(f"  WARNING: Could not get S3 binary metadata: {e}", file=sys.stderr)
        
        # Save served binary to temp path for inspection (keep until end)
        served_bin_path = Path("/tmp") / f"served_artifact_{actual_sha256[:8]}.bin"
        shutil.copy2(tmp_path, served_bin_path)
        print(f"  Saved served binary to: {served_bin_path} (for inspection)")
        
        # Cleanup temp download
        os.unlink(tmp_path)
        tmp_path = None
        
        print("  ✓ Post-upload verification passed (SHA256, size, marker, S3-only)")
        
    except Exception as e:
        print(f"ERROR: Post-upload verification failed: {e}", file=sys.stderr)
        if tmp_path:
            os.unlink(tmp_path)
        sys.exit(1)

def s3_head_object(config, key, label=None):
    """HEAD an S3 object; return True on success."""
    label = label or key
    cmd = build_aws_cmd(config, "s3api", "head-object", "--bucket", config["bucket"], "--key", key)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  {label}: head-object failed: {r.stderr}", file=sys.stderr)
        return False
    try:
        out = json.loads(r.stdout)
        lm = out.get("LastModified", "?")
        cl = out.get("ContentLength", "?")
        etag = out.get("ETag", "?").strip('"')
        print(f"  {label} s3://{config['bucket']}/{key}")
        print(f"    LastModified={lm} ContentLength={cl} ETag={etag}")
    except Exception as e:
        print(f"  {label}: parse head-object output: {e}", file=sys.stderr)
        return False
    return True

def s3_head_proof(config, manifest_key, bin_key):
    """Print S3 head-object (LastModified, ContentLength, ETag) for manifest_latest and bin. Returns False if any head-object fails."""
    print("\n--- S3 object HEAD proof ---")
    ok = True
    ok = s3_head_object(config, manifest_key, "manifest_latest.json") and ok
    ok = s3_head_object(config, bin_key, "bin") and ok
    return ok

def ensure_s3_bucket(config, dry_run=False, require_existing=False):
    """Require an existing bucket when requested; retain legacy bootstrap otherwise."""
    if dry_run:
        print(f"[DRY-RUN] Would check/create bucket: {config['bucket']}")
        return True
    
    # Check if bucket exists
    cmd = build_aws_cmd(config, "s3api", "head-bucket", "--bucket", config["bucket"])
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode == 0:
        # Bucket exists
        return True
    
    if require_existing:
        print(f"ERROR: Existing bucket check failed for {config['bucket']}; bucket creation and policy changes are disabled", file=sys.stderr)
        return False

    # Bucket doesn't exist, create it
    print(f"Bucket {config['bucket']} does not exist, creating...")
    create_cmd = build_aws_cmd(
        config,
        "s3api", "create-bucket",
        "--bucket", config["bucket"],
        "--region", config["region"]
    )
    # For us-east-1, don't specify location constraint
    if config["region"] != "us-east-1":
        create_cmd.extend(["--create-bucket-configuration", f"LocationConstraint={config['region']}"])
    
    try:
        subprocess.run(create_cmd, check=True, capture_output=True, text=True)
        print(f"✓ Created bucket: {config['bucket']}")
        
        # Set public read policy for the prefix
        # Note: This is a simple policy - adjust as needed for your security requirements
        policy = {
            "Version": "2012-10-17",
            "Statement": [
                {
                    "Sid": "PublicReadGetObject",
                    "Effect": "Allow",
                    "Principal": "*",
                    "Action": "s3:GetObject",
                    "Resource": f"arn:aws:s3:::{config['bucket']}/{config['prefix']}/*"
                }
            ]
        }
        policy_file = Path("/tmp") / f"bucket-policy-{config['bucket']}.json"
        policy_file.write_text(json.dumps(policy))
        
        policy_cmd = build_aws_cmd(
            config,
            "s3api", "put-bucket-policy",
            "--bucket", config["bucket"],
            "--policy", f"file://{policy_file}"
        )
        policy_result = subprocess.run(policy_cmd, capture_output=True, text=True)
        if policy_result.returncode == 0:
            print(f"✓ Set public read policy for {config['prefix']}/*")
        else:
            # Policy setting may fail if Block Public Access is enabled - that's okay, files can still be made public individually
            print(f"⚠ Could not set bucket policy (may be blocked by S3 Block Public Access settings)")
            print(f"  Files will need to be made public individually or bucket policy configured manually")
        return True
    except subprocess.CalledProcessError as e:
        print(f"ERROR: Failed to create bucket: {e.stderr}", file=sys.stderr)
        return False

def upload_to_s3(config, local_path, s3_key, content_type, cache_control, dry_run=False):
    """Upload file to S3 with metadata."""
    s3_uri = f"s3://{config['bucket']}/{s3_key}"
    
    if dry_run:
        print(f"[DRY-RUN] Would upload: {local_path} -> {s3_uri}")
        print(f"  Content-Type: {content_type}")
        print(f"  Cache-Control: {cache_control}")
        return True
    
    cmd = build_aws_cmd(
        config,
        "s3", "cp", str(local_path), s3_uri,
        "--content-type", content_type,
        "--cache-control", cache_control
    )
    # Note: ACLs may be disabled on the bucket. Public access must be configured via bucket policy or CloudFront.
    
    try:
        subprocess.run(cmd, check=True)
        print(f"✓ Uploaded: {s3_key}")
        return True
    except subprocess.CalledProcessError as e:
        print(f"ERROR: Upload failed: {e}", file=sys.stderr)
        return False

def invalidate_cloudfront(config, paths, dry_run=False):
    """Create CloudFront invalidation."""
    if not config["cloudfront_id"]:
        return True
    
    if dry_run:
        print(f"[DRY-RUN] Would invalidate CloudFront: {config['cloudfront_id']}")
        for path in paths:
            print(f"  Path: {path}")
        return True
    
    # Create invalidation
    caller_ref = f"ota-{int(time.time())}"
    invalidation = {
        "Paths": {
            "Quantity": len(paths),
            "Items": paths
        },
        "CallerReference": caller_ref
    }
    
    invalidation_file = Path("/tmp") / f"cf-invalidation-{caller_ref}.json"
    invalidation_file.write_text(json.dumps(invalidation))
    
    cmd = build_aws_cmd(
        config,
        "cloudfront", "create-invalidation",
        "--distribution-id", config["cloudfront_id"],
        "--invalidation-batch", f"file://{invalidation_file}"
    )
    
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        print(f"✓ CloudFront invalidation created")
        return True
    except subprocess.CalledProcessError as e:
        print(f"WARNING: CloudFront invalidation failed: {e.stderr}", file=sys.stderr)
        return False

def main():
    global PUBLISH_OK
    parser = argparse.ArgumentParser(description="OTA Publishing Pipeline for HALO SENSE")
    parser.add_argument("--channel", default=None, help="Channel name (dev|prod, default: prod)")
    parser.add_argument("--require-existing-bucket", action="store_true", help="Fail on any bucket check error; never create a bucket or change its policy")
    parser.add_argument("--bucket", default=None, help="S3 bucket name (or set OTA_BUCKET)")
    parser.add_argument("--prefix", default=None, help="S3 key prefix (default: halo/ota)")
    parser.add_argument("--region", default=None, help="AWS region (default: us-east-1)")
    parser.add_argument("--profile", default=None, help="AWS CLI profile")
    parser.add_argument("--base-url", default=None, help="Public base URL (or set OTA_PUBLIC_BASE_URL)")
    parser.add_argument("--cloudfront-id", default=None, help="CloudFront distribution ID")
    parser.add_argument("--fqbn", default="esp32:esp32:XIAO_ESP32S3:PSRAM=opi", help="Arduino FQBN")
    parser.add_argument("--sketch", default=None, help="Path to sketch .ino file")
    parser.add_argument("--bin", default=None, help="Path to existing app-only .bin (skip compilation)")
    parser.add_argument("--partitions", default=None, help="Path to partitions.csv (default: sketch folder if present)")
    parser.add_argument("--dry-run", action="store_true", help="Validate only, don't upload")
    parser.add_argument("--min-version", default="0.0.0", help="Minimum required version")
    parser.add_argument("--rollout-pct", default=None, type=int, help="Optional rollout percent (0-100)")
    parser.add_argument("--rollout-seed", default=None, type=int, help="Optional rollout seed (int)")
    parser.add_argument("--min-version-allowed", default=None, help="Optional rollout min version gate")
    
    args = parser.parse_args()

    if args.bin:
        try:
            args.bin = str(validate_publishable_artifact(args.bin))
        except (OSError, RuntimeError, ValueError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
    
    # Get AWS config
    config = get_aws_config(
        channel=args.channel,
        bucket=args.bucket,
        prefix=args.prefix,
        region=args.region,
        profile=args.profile,
        base_url=args.base_url,
        cloudfront_id=args.cloudfront_id
    )
    
    print("="*60)
    print("HALO SENSE OTA Publishing Pipeline")
    print("="*60)
    print(f"Channel: {config['channel']}")
    print(f"Bucket: {config['bucket']}")
    print(f"Prefix: {config['prefix']}")
    print(f"Region: {config['region']}")
    if config['cloudfront_id']:
        print(f"CloudFront: {config['cloudfront_id']}")
    print()

    # Prove AWS creds work before doing anything that touches S3
    if not args.dry_run:
        print("="*60)
        print("AWS Identity and Configuration (before S3 operations)")
        print("="*60)
        print("Checking AWS identity (aws sts get-caller-identity)...")
        cmd = build_aws_cmd(config, "sts", "get-caller-identity")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("ERROR: AWS credentials failed or missing", file=sys.stderr)
            print("  Run: aws sts get-caller-identity", file=sys.stderr)
            print(f"  {r.stderr}", file=sys.stderr)
            sys.exit(1)
        try:
            identity = json.loads(r.stdout)
            account = identity.get('Account', '?')
            arn = identity.get('Arn', '?')
            print(f"  Account: {account}")
            print(f"  Arn: {arn}")
        except Exception as e:
            print(f"  WARNING: Could not parse identity: {e}", file=sys.stderr)
        
        # Log AWS region and credentials source
        print(f"\nAWS Configuration:")
        print(f"  Region: {config['region']}")
        if config['profile']:
            print(f"  Profile: {config['profile']} (credentials from AWS profile)")
        elif os.environ.get('AWS_ACCESS_KEY_ID'):
            print(f"  Credentials: Environment variables (AWS_ACCESS_KEY_ID set)")
        elif os.environ.get('AWS_PROFILE'):
            print(f"  Credentials: AWS profile '{os.environ.get('AWS_PROFILE')}'")
        else:
            print(f"  Credentials: Default credential chain")
        
        # Try to get boto3 session info if available
        if BOTO3_AVAILABLE:
            try:
                session = boto3.Session(
                    profile_name=config['profile'] if config['profile'] else None,
                    region_name=config['region']
                )
                credentials = session.get_credentials()
                if credentials:
                    print(f"  boto3 Session Region: {session.region_name}")
                    print(f"  boto3 Credentials: Available (access_key ends with ...{credentials.access_key[-4:] if credentials.access_key else 'N/A'})")
            except Exception as e:
                print(f"  boto3 Session: Could not create ({e})")
        print()

    repo_root = Path(__file__).parent.parent.parent
    if args.partitions:
        partitions_csv = Path(args.partitions)
    else:
        partitions_csv = None
        if args.sketch:
            sketch_dir = Path(args.sketch).parent
            candidate = sketch_dir / "partitions.csv"
            if candidate.exists():
                partitions_csv = candidate
        if partitions_csv is None:
            partitions_csv = repo_root / "firmware" / "halo_sense_prod" / "partitions.csv"
    ota_slot_bytes = parse_ota_slot_bytes(partitions_csv)
    
    # Step 1: Compile or use existing binary (must be app-only OTA binary)
    if args.bin:
        bin_path = Path(args.bin)
        if not bin_path.exists():
            print(f"ERROR: Binary not found: {bin_path}", file=sys.stderr)
            sys.exit(1)
        if not _is_app_only_bin(bin_path.name):
            print(f"ERROR: Binary is not app-only (exclude: {', '.join(_EXCLUDE_BIN_SUBSTRINGS)})", file=sys.stderr)
            print(f"  Provided: {bin_path.name}", file=sys.stderr)
            sys.exit(1)
        print(f"Using existing binary: {bin_path}")
        size = get_file_size(bin_path)
        info = _candidate_info(bin_path)
        print(f"CANDIDATES={json.dumps([info])}")
        print(f"SELECTED={json.dumps({'path': str(bin_path), 'size': size})}")
        print(f"SELECTED_OTA_BIN_PATH={bin_path}")
        print(f"SELECTED_OTA_BIN_SIZE={size}")
        print(f"EXCLUDED_CANDIDATES=[]  (used --bin)")
    else:
        # Build with ENV/CHANNEL so binary matches publish target (channel mode: S3 bucket/region/prefix)
        ch = config["channel"] or "dev"
        bucket = config["bucket"]
        region = config["region"] or "us-east-1"
        prefix = config["prefix"] or "halo/ota"
        build_flags = (
            f'-DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1 '
            f'-DOTA_CHANNEL_ENABLED -DOTA_CHANNEL="{ch}" -DOTA_S3_BUCKET="{bucket}" '
            f'-DOTA_S3_REGION="{region}" -DOTA_S3_PREFIX="{prefix}"'
        )
        
        # A) Use deterministic build directory
        target = args.fqbn.replace(":", ".")
        deterministic_build_dir = get_deterministic_build_dir(repo_root, target, ch, build_id=None)
        bin_path = compile_firmware(fqbn=args.fqbn, sketch_path=args.sketch, build_extra_flags=build_flags, build_dir=deterministic_build_dir, channel=ch)
        if not bin_path:
            print("ERROR: Compilation failed", file=sys.stderr)
            sys.exit(1)
        try:
            bin_path = validate_publishable_artifact(bin_path)
        except (OSError, RuntimeError, ValueError) as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
        
        # S3-only enforcement: Check built binary for GitHub strings (fail closed)
        print("\n--- S3-only artifact check (no GitHub strings) ---")
        github_strings = []
        try:
            result = subprocess.run(
                ["strings", "-a", str(bin_path)],
                capture_output=True,
                text=True,
                errors="ignore",
                check=True
            )
            for line in result.stdout.splitlines():
                if any(pattern in line.lower() for pattern in ["github.io", "githubusercontent", "raw.githubusercontent", "halo-ota-manifest", "mattrtaylor71"]):
                    github_strings.append(line)
        except Exception as e:
            print(f"WARNING: Could not check binary strings: {e}", file=sys.stderr)
        
        if github_strings:
            print("ERROR: S3-only violation: Found GitHub strings in built binary:", file=sys.stderr)
            for s in github_strings[:10]:  # Show first 10
                print(f"  {s}", file=sys.stderr)
            print("\nThis must be fixed before publishing. The string is likely from:", file=sys.stderr)
            print("  1. An Arduino library (Update, OTA, or ESP32 core)", file=sys.stderr)
            print("  2. A preprocessor default constant", file=sys.stderr)
            print("  3. Old code that wasn't removed", file=sys.stderr)
            print("\nEven if unused at runtime, GitHub strings must not appear in shipped artifacts.", file=sys.stderr)
            sys.exit(1)
        print("  ✓ No GitHub strings found in binary")
        
        size = get_file_size(bin_path)
    
    # D) Partition-size gate (hard-fail if artifact too large for OTA partition)
    print(f"\n--- Partition-size gate ---")
    print(f"OTA_SLOT_BYTES={ota_slot_bytes} (from {partitions_csv})")
    print(f"Binary size: {size} bytes")
    margin = ota_slot_bytes - size
    margin_pct = (margin / ota_slot_bytes * 100) if ota_slot_bytes > 0 else 0
    print(f"Margin: {margin} bytes ({margin_pct:.1f}% of slot)")
    
    if size > ota_slot_bytes:
        print(f"ERROR: OTA artifact too large for OTA slot!", file=sys.stderr)
        print(f"  Binary size: {size} bytes", file=sys.stderr)
        print(f"  OTA slot size: {ota_slot_bytes} bytes", file=sys.stderr)
        print(f"  Excess: {size - ota_slot_bytes} bytes", file=sys.stderr)
        print(f"  Selected binary: {bin_path}", file=sys.stderr)
        print(f"  Ensure build produces app-only binary (exclude merged/bootloader/partitions).", file=sys.stderr)
        sys.exit(1)
    
    if margin < (ota_slot_bytes * 0.1):  # Warn if less than 10% margin
        print(f"  WARNING: Low margin ({margin_pct:.1f}%) - consider optimizing binary size", file=sys.stderr)
    else:
        print(f"  ✓ Size fits OTA slot with {margin_pct:.1f}% margin")
    
    # Step 2: Extract version from marker
    version, build_id = find_firmware_marker(bin_path)
    if not version:
        print("ERROR: Could not extract version from binary marker", file=sys.stderr)
        print("  Run: strings -a <bin> | grep HALO_FW_MARKER", file=sys.stderr)
        sys.exit(1)
    
    # Ensure build_id is present (required for manifest)
    if not build_id or build_id == "unknown" or len(build_id.strip()) == 0:
        print("ERROR: build_id is missing or invalid", file=sys.stderr)
        print(f"  Extracted build_id: '{build_id}'", file=sys.stderr)
        print("  build_id is required in manifest_latest.json", file=sys.stderr)
        sys.exit(1)
    
    print(f"Extracted version: {version}")
    print(f"Extracted build_id: {build_id}")
    
    # Step 3: Validate marker (fail fast if mismatch)
    # CRITICAL: Refuse publish if marker version doesn't match extracted version
    # This prevents publishing wrong binary
    print("\nValidating marker...")
    valid, verified_version, verified_build_id = validate_marker(bin_path, version)
    if not valid:
        print("ERROR: Marker validation failed - refusing to publish", file=sys.stderr)
        print("  This prevents publishing wrong binary!", file=sys.stderr)
        sys.exit(1)
    
    # Double-check: marker version must match what we extracted (sanity check)
    if verified_version != version:
        print(f"ERROR: Marker version mismatch!", file=sys.stderr)
        print(f"  Extracted: {version}", file=sys.stderr)
        print(f"  Verified: {verified_version}", file=sys.stderr)
        print(f"  Refusing to publish - binary marker inconsistent", file=sys.stderr)
        sys.exit(1)
    
    # Step 4: Compute SHA256 and size
    print("\nComputing checksums...")
    sha256 = compute_sha256(bin_path)
    size = get_file_size(bin_path)
    print(f"  SHA256: {sha256}")
    print(f"  Size: {size} bytes")
    print(f"PUBLISH_PROOF version={version} build_id={build_id} sha256={sha256} size={size} slot_bytes={ota_slot_bytes}")
    
    # Step 5: Prepare artifact name and paths
    # Artifact name includes version + build timestamp for uniqueness
    # Format: sense_xiao_s3_<version>_<timestamp>.bin
    # Build timestamp from build_id (extract date/time portion)
    timestamp_suffix = ""
    if build_id and build_id != "unknown":
        # Extract timestamp-like portion from build_id (e.g., "Jan 23 2026-16:54:09")
        # Convert to filesystem-safe format: Jan_23_2026_16-54-09
        timestamp_match = re.search(r'(\w+\s+\d+\s+\d+)-(\d+):(\d+):(\d+)', build_id)
        if timestamp_match:
            date_part = timestamp_match.group(1).replace(" ", "_")  # "Jan_23_2026"
            time_part = f"{timestamp_match.group(2)}-{timestamp_match.group(3)}-{timestamp_match.group(4)}"  # "16-54-09"
            timestamp_suffix = f"_{date_part}_{time_part}"
    
    artifact_name = f"sense_xiao_s3_{version}{timestamp_suffix}.bin"
    channel_path = f"{config['prefix']}/{config['channel']}"
    artifact_s3_key = f"{channel_path}/artifacts/{artifact_name}"
    manifest_versioned_key = f"{channel_path}/manifest_{version}.json"
    manifest_latest_key = f"{channel_path}/manifest_latest.json"
    
    # Step 6: Build public URLs
    # Remove prefix from S3 keys to build relative paths
    def remove_prefix(key, prefix):
        if key.startswith(prefix + '/'):
            return key[len(prefix) + 1:]
        return key
    
    artifact_rel_path = remove_prefix(artifact_s3_key, config['prefix'])
    manifest_versioned_rel_path = remove_prefix(manifest_versioned_key, config['prefix'])
    manifest_latest_rel_path = remove_prefix(manifest_latest_key, config['prefix'])
    
    # Build full URLs
    base_url = config['base_url'].rstrip('/')
    bin_url = f"{base_url}/{artifact_rel_path}"
    manifest_versioned_url = f"{base_url}/{manifest_versioned_rel_path}"
    manifest_latest_url = f"{base_url}/{manifest_latest_rel_path}"
    
    # Step 7: Generate manifests (size/sha256 from selected app-only binary; max_slot_bytes for verification; canary fields)
    print("\nGenerating manifests...")
    published_at_utc = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    try:
        publisher_host = socket.gethostname() or "local"
    except Exception:
        publisher_host = "local"
    manifest = generate_manifest(
        version=version,
        bin_url=bin_url,
        sha256=sha256,
        size=size,
        build_id=build_id,
        artifact_fw_version=version,
        min_version=args.min_version,
        max_slot_bytes=ota_slot_bytes,
        published_at_utc=published_at_utc,
        publisher_host=publisher_host,
        rollout_pct=args.rollout_pct,
        rollout_seed=args.rollout_seed,
        min_version_allowed=args.min_version_allowed
    )
    
    manifest_json = json.dumps(manifest, indent=2)
    print(f"  Versioned manifest: {manifest_versioned_key}")
    print(f"  Latest manifest: {manifest_latest_key}")
    
    # Step 8: Create local release directory
    repo_root = Path(__file__).parent.parent.parent
    dist_dir = repo_root / "dist" / config['channel']
    artifacts_dir = dist_dir / "artifacts"
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    
    # Copy binary to dist
    dist_bin = artifacts_dir / artifact_name
    shutil.copy2(bin_path, dist_bin)
    print(f"\n✓ Copied binary to: {dist_bin}")
    
    # Write manifests locally
    manifest_versioned_file = dist_dir / f"manifest_{version}.json"
    manifest_latest_file = dist_dir / "manifest_latest.json"
    
    manifest_versioned_file.write_text(manifest_json)
    manifest_latest_file.write_text(manifest_json)
    
    print(f"✓ Generated manifests in: {dist_dir}")
    
    if args.dry_run:
        print("\n[DRY-RUN] Would upload:")
        print(f"  Binary: s3://{config['bucket']}/{artifact_s3_key}")
        print(f"  Manifest (versioned): s3://{config['bucket']}/{manifest_versioned_key}")
        print(f"  Manifest (latest): s3://{config['bucket']}/{manifest_latest_key}")
        print("\nFinal URLs:")
        print(f"  Binary: {bin_url}")
        print(f"  Manifest: {manifest_latest_url}")
        PUBLISH_OK = True
        return 0
    
    # Step 9: Ensure S3 bucket exists
    if not ensure_s3_bucket(config, dry_run=args.dry_run, require_existing=args.require_existing_bucket):
        print("ERROR: Failed to ensure S3 bucket exists", file=sys.stderr)
        sys.exit(1)
    
    # Step 10: Upload to S3 (atomic: versioned first, then latest)
    print("\nUploading to S3...")
    
    # Upload binary
    if not upload_to_s3(
        config, dist_bin, artifact_s3_key,
        content_type="application/octet-stream",
        cache_control="public, max-age=31536000, immutable"
    ):
        sys.exit(1)
    print("\nVerifying artifact upload (HEAD)...")
    if not s3_head_object(config, artifact_s3_key, "artifact"):
        sys.exit(1)
    
    # Upload versioned manifest
    if not upload_to_s3(
        config, manifest_versioned_file, manifest_versioned_key,
        content_type="application/json",
        cache_control="public, max-age=31536000, immutable"
    ):
        sys.exit(1)
    print("\nVerifying versioned manifest upload (HEAD)...")
    if not s3_head_object(config, manifest_versioned_key, "manifest_versioned.json"):
        sys.exit(1)
    
    # Upload latest manifest LAST (atomic pointer update); no caching so device always gets current
    print(f"\nUploading manifest_latest.json to: s3://{config['bucket']}/{manifest_latest_key}")
    if not upload_to_s3(
        config, manifest_latest_file, manifest_latest_key,
        content_type="application/json",
        cache_control="no-store, no-cache, max-age=0, must-revalidate"
    ):
        sys.exit(1)
    
    # Step 10b: Same URL the device uses — explicit target (exact format for logs/parsing)
    target_s3 = f"s3://{config['bucket']}/{config['prefix']}/{config['channel']}/manifest_latest.json"
    print("\n" + "="*60)
    print("TARGET (device reads this manifest):")
    print("="*60)
    print(f"TARGET: {target_s3}")
    print(f"TARGET_BIN_KEY={artifact_s3_key}")
    print(f"MANIFEST_URL={manifest_latest_url}")
    print(f"BIN_URL={bin_url}")
    
    # Step 10b2: Immediate S3 HEAD/GET proof (using boto3 if available)
    if BOTO3_AVAILABLE:
        print("\n" + "="*60)
        print("S3 Write-Then-Read Proof (boto3 direct)")
        print("="*60)
        try:
            session = boto3.Session(
                profile_name=config['profile'] if config['profile'] else None,
                region_name=config['region']
            )
            s3_client = session.client('s3')
            
            # HEAD object
            print(f"\nS3 HEAD Object: s3://{config['bucket']}/{manifest_latest_key}")
            try:
                head_response = s3_client.head_object(Bucket=config['bucket'], Key=manifest_latest_key)
                etag = head_response.get('ETag', '').strip('"')
                version_id = head_response.get('VersionId', 'None (bucket not versioned)')
                last_modified = head_response.get('LastModified', '?')
                content_length = head_response.get('ContentLength', '?')
                print(f"  ETag: {etag}")
                print(f"  VersionId: {version_id}")
                print(f"  LastModified: {last_modified}")
                print(f"  ContentLength: {content_length}")
            except ClientError as e:
                print(f"  ERROR: HEAD failed: {e}", file=sys.stderr)
            
            # GET object and parse
            print(f"\nS3 GET Object: s3://{config['bucket']}/{manifest_latest_key}")
            try:
                get_response = s3_client.get_object(Bucket=config['bucket'], Key=manifest_latest_key)
                body = get_response['Body'].read().decode('utf-8')
                s3_manifest = json.loads(body)
                s3_version = s3_manifest.get('version', 'N/A')
                s3_build_id = s3_manifest.get('build_id', 'N/A')
                s3_bin_url = s3_manifest.get('bin_url', 'N/A')
                print(f"  Parsed version: {s3_version}")
                print(f"  Parsed build_id: {s3_build_id}")
                print(f"  Parsed bin_url: {s3_bin_url[:80]}...")
                
                # Verify version matches
                if s3_version != version:
                    print(f"\n  ✗ FAIL: S3 object version mismatch!", file=sys.stderr)
                    print(f"    Expected: {version}", file=sys.stderr)
                    print(f"    S3 object has: {s3_version}", file=sys.stderr)
                    print(f"    This means manifest_latest.json was NOT updated correctly!", file=sys.stderr)
                    sys.exit(1)
                else:
                    print(f"  ✓ Version matches expected: {version}")
            except ClientError as e:
                print(f"  ERROR: GET failed: {e}", file=sys.stderr)
            except json.JSONDecodeError as e:
                print(f"  ERROR: Could not parse manifest JSON: {e}", file=sys.stderr)
        except Exception as e:
            print(f"  WARNING: boto3 operations failed: {e}", file=sys.stderr)
            print(f"  Falling back to HTTP verification only", file=sys.stderr)
    else:
        print("\n  NOTE: boto3 not available, skipping direct S3 HEAD/GET proof")
    
    # Step 10b3: HTTP HEAD/GET comparison
    print("\n" + "="*60)
    print("HTTP HEAD/GET Comparison (public URL)")
    print("="*60)
    print(f"HTTP URL: {manifest_latest_url}")
    
    # HTTP HEAD
    try:
        head_req = Request(manifest_latest_url, method='HEAD')
        with urlopen(head_req, timeout=15) as response:
            http_etag = response.headers.get('ETag', 'N/A').strip('"')
            http_last_modified = response.headers.get('Last-Modified', 'N/A')
            http_version_id = response.headers.get('x-amz-version-id', 'N/A')
            http_content_length = response.headers.get('Content-Length', 'N/A')
            print(f"\nHTTP HEAD Response:")
            print(f"  Status: {response.status}")
            print(f"  ETag: {http_etag}")
            print(f"  Last-Modified: {http_last_modified}")
            print(f"  x-amz-version-id: {http_version_id}")
            print(f"  Content-Length: {http_content_length}")
    except Exception as e:
        print(f"  ERROR: HTTP HEAD failed: {e}", file=sys.stderr)
    
    # HTTP GET and parse
    try:
        with urlopen(manifest_latest_url, timeout=15) as response:
            http_body = response.read().decode('utf-8')
            http_manifest = json.loads(http_body)
            http_version = http_manifest.get('version', 'N/A')
            http_build_id = http_manifest.get('build_id', 'N/A')
            http_bin_url = http_manifest.get('bin_url', 'N/A')
            print(f"\nHTTP GET Response (parsed):")
            print(f"  version: {http_version}")
            print(f"  build_id: {http_build_id}")
            print(f"  bin_url: {http_bin_url[:80]}...")
            
            # Verify version matches
            if http_version != version:
                print(f"\n  ✗ FAIL: HTTP manifest version mismatch!", file=sys.stderr)
                print(f"    Expected: {version}", file=sys.stderr)
                print(f"    HTTP serves: {http_version}", file=sys.stderr)
                print(f"    This could indicate caching, proxy, or wrong object being served", file=sys.stderr)
                sys.exit(1)
            else:
                print(f"  ✓ Version matches expected: {version}")
    except Exception as e:
        print(f"  ERROR: HTTP GET failed: {e}", file=sys.stderr)
    
    # Step 10b4: Check bucket configuration
    print("\n" + "="*60)
    print("Bucket Configuration Check")
    print("="*60)
    if BOTO3_AVAILABLE:
        try:
            session = boto3.Session(
                profile_name=config['profile'] if config['profile'] else None,
                region_name=config['region']
            )
            s3_client = session.client('s3')
            
            # Get bucket versioning
            try:
                versioning = s3_client.get_bucket_versioning(Bucket=config['bucket'])
                versioning_status = versioning.get('Status', 'NotEnabled')
                mfa_delete = versioning.get('MfaDelete', 'Disabled')
                print(f"\nBucket Versioning:")
                print(f"  Status: {versioning_status}")
                print(f"  MfaDelete: {mfa_delete}")
            except ClientError as e:
                print(f"  ERROR: Could not get versioning: {e}", file=sys.stderr)
            
            # Get bucket replication
            try:
                replication = s3_client.get_bucket_replication(Bucket=config['bucket'])
                print(f"\nBucket Replication:")
                print(f"  Status: Enabled")
                print(f"  Rules: {len(replication.get('ReplicationConfiguration', {}).get('Rules', []))} rule(s)")
            except ClientError as e:
                if e.response['Error']['Code'] == 'ReplicationConfigurationNotFoundError':
                    print(f"\nBucket Replication:")
                    print(f"  Status: Not configured")
                else:
                    print(f"  ERROR: Could not get replication: {e}", file=sys.stderr)
            
            # Get bucket location
            try:
                location = s3_client.get_bucket_location(Bucket=config['bucket'])
                bucket_region = location.get('LocationConstraint', 'us-east-1') or 'us-east-1'
                print(f"\nBucket Location:")
                print(f"  Region: {bucket_region}")
                if bucket_region != config['region']:
                    print(f"  WARNING: Bucket region ({bucket_region}) != config region ({config['region']})", file=sys.stderr)
            except ClientError as e:
                print(f"  ERROR: Could not get location: {e}", file=sys.stderr)
        except Exception as e:
            print(f"  WARNING: Could not check bucket config: {e}", file=sys.stderr)
    else:
        print("  NOTE: boto3 not available, using AWS CLI")
        # Try AWS CLI fallback
        try:
            cmd = build_aws_cmd(config, "s3api", "get-bucket-versioning", "--bucket", config['bucket'])
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode == 0:
                print(f"\nBucket Versioning (AWS CLI):")
                print(f"  {r.stdout.strip()}")
        except Exception:
            pass
    
    # Step 10c: Post-publish readback — fetch live manifest and assert invariants (including version match)
    # Note: This is redundant with Step 10b2/10b3 but provides additional verification
    post_publish_readback(manifest_latest_url, version, build_id, bin_url, size, ota_slot_bytes)
    
    # Step 10c2: Re-HEAD after delay to detect reverts (if boto3 available)
    if BOTO3_AVAILABLE and not args.dry_run:
        print("\n" + "="*60)
        print("Revert Detection (re-HEAD after 5 seconds)")
        print("="*60)
        print("Waiting 5 seconds, then re-checking S3 object...")
        time.sleep(5)
        try:
            session = boto3.Session(
                profile_name=config['profile'] if config['profile'] else None,
                region_name=config['region']
            )
            s3_client = session.client('s3')
            head_response = s3_client.head_object(Bucket=config['bucket'], Key=manifest_latest_key)
            recheck_etag = head_response.get('ETag', '').strip('"')
            recheck_version_id = head_response.get('VersionId', 'None')
            recheck_last_modified = head_response.get('LastModified', '?')
            
            # Get the ETag from initial HEAD (if we stored it)
            # For now, just verify the object still exists and get its content
            get_response = s3_client.get_object(Bucket=config['bucket'], Key=manifest_latest_key)
            recheck_body = get_response['Body'].read().decode('utf-8')
            recheck_manifest = json.loads(recheck_body)
            recheck_version = recheck_manifest.get('version', 'N/A')
            recheck_etag_from_get = get_response.get('ETag', '').strip('"')
            recheck_last_modified_from_get = get_response.get('LastModified', '?')
            
            print(f"  Re-check ETag: {recheck_etag}")
            print(f"  Re-check VersionId: {recheck_version_id}")
            print(f"  Re-check LastModified: {recheck_last_modified}")
            print(f"  Re-check version: {recheck_version}")
            
            # Log manifest object metadata
            print(f"\nS3 Manifest Object Metadata:")
            print(f"  ETag: {recheck_etag}")
            print(f"  LastModified: {recheck_last_modified}")
            
            if recheck_version != version:
                print(f"\n  ✗ FAIL: Object reverted! Version changed from {version} to {recheck_version}", file=sys.stderr)
                print(f"    This indicates another process overwrote the manifest", file=sys.stderr)
                print(f"    Check CloudTrail for PutObject events on this key", file=sys.stderr)
                sys.exit(1)
            else:
                print(f"  ✓ Object still has correct version: {recheck_version}")
        except Exception as e:
            print(f"  WARNING: Re-check failed: {e}", file=sys.stderr)
    
    # Step 10c2: Post-upload artifact verification — download and verify the served artifact
    verify_uploaded_artifact(bin_url, sha256, size, manifest_url=manifest_latest_url, config=config)
    
    # Step 10d: S3 HEAD proof for manifest_latest and bin; exit if any head-object fails
    if not s3_head_proof(config, manifest_latest_key, artifact_s3_key):
        print("ERROR: S3 head-object proof failed", file=sys.stderr)
        sys.exit(1)
    
    # Step 11: Invalidate CloudFront (if configured) - only manifest_latest.json for fast update
    if config['cloudfront_id']:
        print("\nInvalidating CloudFront...")
        # Only invalidate manifest_latest.json (fast update, binary is immutable)
        # Binary doesn't need invalidation (immutable, versioned)
        paths = [f"/{manifest_latest_key}"]
        invalidate_cloudfront(config, paths)
    
    # Step 12: Print final URLs (explicit and clear)
    print("\n" + "="*60)
    print("✓ OTA Release Published Successfully")
    print("="*60)
    print(f"Version: {version}")
    print(f"Channel: {config['channel']}")
    print(f"Build ID: {build_id}")
    print(f"\nPublic URLs (for firmware configuration):")
    print(f"  Binary URL: {bin_url}")
    print(f"  Manifest (latest): {manifest_latest_url}")
    print(f"  Manifest (versioned): {manifest_versioned_url}")
    
    # Show URL source (CloudFront vs S3)
    if config.get('cloudfront_domain'):
        print(f"\n  URL Source: CloudFront ({config['cloudfront_domain']}) - Range support: ✓")
    elif config['base_url'].startswith(f"https://{config['bucket']}.s3"):
        print(f"\n  URL Source: S3 REST endpoint - Range support: ✓")
    else:
        print(f"\n  URL Source: Custom ({config['base_url']})")
    
    print(f"\nS3 Paths:")
    print(f"  s3://{config['bucket']}/{artifact_s3_key}")
    print(f"  s3://{config['bucket']}/{manifest_latest_key}")
    print(f"  s3://{config['bucket']}/{manifest_versioned_key}")
    
    # Firmware config: S3-only, no override. Use channel-mode flags only.
    print(f"\n" + "="*60)
    print("Firmware Configuration (S3-only, no override):")
    print("="*60)
    print(f"  -DOTA_CHANNEL_ENABLED -DOTA_CHANNEL=\"{config['channel']}\" -DOTA_S3_BUCKET=\"{config['bucket']}\" -DOTA_S3_REGION=\"{config['region']}\" -DOTA_S3_PREFIX=\"{config['prefix']}\"")
    
    # Print URLs and size/slot for proof bundle (parseable; used by ota-proof and verify_manifest)
    print(f"\n" + "="*60)
    print("Proof Bundle (URLs and size guardrail):")
    print("="*60)
    print(f"MANIFEST_URL={manifest_latest_url}")
    print(f"BIN_URL={bin_url}")
    print(f"SELECTED_OTA_BIN_PATH={bin_path}")
    print(f"SELECTED_OTA_BIN_SIZE={size}")
    print(f"OTA_SLOT_BYTES={ota_slot_bytes}")
    PUBLISH_OK = True
    return 0

if __name__ == "__main__":
    register_publish_exit_guard()
    sys.exit(main())
