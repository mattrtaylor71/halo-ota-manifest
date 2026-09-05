#!/usr/bin/env python3
"""
LCD OTA Publishing – publish a prebuilt LCD binary to S3. NO COMPILATION.

This script never compiles. It only validates the provided BIN and uploads to S3.
Build the LCD firmware in Arduino IDE (or your build system) and pass the .bin path.

Usage:
    python3 tools/ota/publish_lcd_ota.py --channel dev --version 6.0.3 --bin /abs/path/to/lcd.bin
    make ota-publish-lcd CHANNEL=dev VERSION=6.0.3 BIN=/abs/path/to/lcd.bin

Inputs:
    --channel dev|prod   Channel name (default: dev)
    --version X.Y.Z      Expected version (must match binary marker exactly)
    --bin /path/to/bin   Path to prebuilt LCD app .bin (required; must exist and be a file)
    --bucket             S3 bucket (default: halo-ota-<channel>)
    --region             AWS region (default: us-east-1)
    --prefix             S3 prefix (default: halo/ota)
    --profile            AWS profile

Validation (fail fast):
    - BIN exists and is a file
    - Marker contains HALO_FW_MARKER:<VERSION> (exact match with --version)
    - Marker contains BUILD_ID: and BOARD:lcd
    - size <= both OTA slots from firmware/halo_lcd_prod/partitions.csv
    - SHA256 computed and placed into manifest

Upload order:
    1) BIN to halo/ota/<channel>/lcd/artifacts/<artifact_name>.bin
    2) manifest_<version>.json to halo/ota/<channel>/lcd/
    3) manifest_latest.json to halo/ota/<channel>/lcd/ LAST with Cache-Control: no-store, no-cache, max-age=0, must-revalidate

Manifest base URL: https://<bucket>.s3.<region>.amazonaws.com/halo/ota/<channel>/lcd/
"""

import argparse
import atexit
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from datetime import datetime, timezone

from artifact_safety import validate_publishable_artifact

# Reuse helpers from publish_ota (same repo)
try:
    from publish_ota import (
        compute_sha256,
        get_file_size,
        get_aws_config,
        build_aws_cmd,
        ensure_s3_bucket,
        generate_manifest,
        upload_to_s3 as _upload_to_s3,
        post_publish_readback,
        verify_uploaded_artifact,
        s3_head_proof,
        s3_head_object,
        BOTO3_AVAILABLE,
        boto3,
    )

    def upload_to_s3(config, local_path, s3_key, content_type="application/octet-stream", cache_control=None):
        return _upload_to_s3(config, local_path, s3_key, content_type, cache_control or "", dry_run=False)
except ImportError:
    # Standalone fallback
    def compute_sha256(file_path):
        h = hashlib.sha256()
        with open(file_path, "rb") as f:
            for chunk in iter(lambda: f.read(4096), b""):
                h.update(chunk)
        return h.hexdigest()

    def get_file_size(file_path):
        return os.path.getsize(file_path)

    def get_aws_config(channel, bucket=None, prefix=None, region=None, profile=None, **kwargs):
        config = {
            "bucket": bucket or os.environ.get("OTA_BUCKET") or ("halo-ota-prod" if channel == "prod" else "halo-ota-dev"),
            "prefix": prefix or os.environ.get("OTA_PREFIX", "halo/ota"),
            "channel": channel or "dev",
            "region": region or os.environ.get("AWS_REGION", "us-east-1"),
            "profile": profile or os.environ.get("AWS_PROFILE"),
            "base_url": None,
        }
        config["base_url"] = f"https://{config['bucket']}.s3.{config['region']}.amazonaws.com/{config['prefix']}"
        return config

    def build_aws_cmd(config, *args):
        cmd = ["aws"]
        if config.get("profile"):
            cmd.extend(["--profile", config["profile"]])
        if config.get("region"):
            cmd.extend(["--region", config["region"]])
        cmd.extend(list(args))
        return cmd

    def ensure_s3_bucket(config, dry_run=False):
        return True

    def generate_manifest(version, bin_url, sha256, size, build_id, artifact_fw_version,
                          min_version="0.0.0", max_slot_bytes=None,
                          published_at_utc=None, publisher_host=None,
                          rollout_pct=None, rollout_seed=None, min_version_allowed=None):
        m = {"version": version, "bin_url": bin_url, "sha256": sha256, "size": size, "min_version": min_version, "build_id": build_id, "artifact_fw_version": artifact_fw_version, "board": "lcd"}
        if max_slot_bytes is not None:
            m["max_slot_bytes"] = max_slot_bytes
        if published_at_utc:
            m["published_at_utc"] = published_at_utc
        if publisher_host:
            m["publisher_host"] = publisher_host
        if rollout_pct is not None:
            m["rollout_pct"] = int(rollout_pct)
        if rollout_seed is not None:
            m["rollout_seed"] = int(rollout_seed)
        if min_version_allowed:
            m["min_version_allowed"] = str(min_version_allowed)
        return m

    BOTO3_AVAILABLE = False
    boto3 = None

    def post_publish_readback(*args, **kwargs):
        raise RuntimeError("publish_ota helpers missing: post_publish_readback")

    def verify_uploaded_artifact(*args, **kwargs):
        raise RuntimeError("publish_ota helpers missing: verify_uploaded_artifact")

    def s3_head_proof(*args, **kwargs):
        raise RuntimeError("publish_ota helpers missing: s3_head_proof")

    def s3_head_object(*args, **kwargs):
        raise RuntimeError("publish_ota helpers missing: s3_head_object")

PUBLISH_OK = False

def register_publish_exit_guard():
    def _publish_exit():
        if not PUBLISH_OK:
            print("PUBLISH_FAILED", file=sys.stderr)
    atexit.register(_publish_exit)


def parse_ota_slot_bytes(partitions_csv_path):
    """Require both production OTA slots and use the smaller size (bytes)."""
    path = Path(partitions_csv_path)
    slots = {}
    try:
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = [p.strip() for p in line.split(",")]
                if len(parts) < 5:
                    continue
                ptype, subtype = parts[1].lower(), parts[2].lower()
                size_str = parts[4] if len(parts) > 4 else ""
                if ptype != "app" or subtype not in ("ota_0", "ota_1"):
                    continue
                size = int(size_str, 16 if size_str.lower().startswith("0x") else 10)
                if size <= 0 or subtype in slots:
                    raise ValueError(f"invalid or duplicate {subtype} slot")
                slots[subtype] = size
    except (OSError, ValueError) as exc:
        raise ValueError(f"Cannot read LCD OTA slots from {path}: {exc}") from exc
    if set(slots) != {"ota_0", "ota_1"}:
        raise ValueError(f"LCD partition table must define both ota_0 and ota_1: {path}")
    return min(slots.values())


def get_marker_strings(bin_path):
    """Return raw marker-related strings from binary (for strict validation)."""
    bin_path = Path(bin_path)
    if not bin_path.exists():
        return ""
    try:
        result = subprocess.run(
            ["strings", "-a", str(bin_path)],
            capture_output=True,
            text=True,
            errors="ignore",
            check=True,
        )
        return result.stdout
    except Exception:
        return ""


def find_lcd_marker(bin_path):
    """
    Extract version and build_id from LCD binary marker.
    Expects HALO_FW_MARKER:X.Y.Z|BUILD_ID:...|BOARD:lcd (all required for publish).
    Returns (version, build_id, has_board_lcd) or (None, None, False).
    """
    bin_path = Path(bin_path)
    if not bin_path.exists():
        return None, None, False
    raw = get_marker_strings(bin_path)
    has_board_lcd = "BOARD:lcd" in raw
    has_build_id = "BUILD_ID:" in raw
    try:
        for line in raw.splitlines():
            if "HALO_FW_MARKER:" not in line:
                continue
            m = re.search(r"HALO_FW_MARKER:([0-9]+\.[0-9]+\.[0-9]+)(?:\|BUILD_ID:([^|]+))?", line)
            if m:
                version = m.group(1)
                build_id = (m.group(2).strip() if m.group(2) else "unknown")
                return version, build_id, has_board_lcd
        with open(bin_path, "rb") as f:
            data = f.read()
        pattern = re.compile(rb"HALO_FW_MARKER:([0-9]+\.[0-9]+\.[0-9]+)(?:\|BUILD_ID:([^\x00|]+))?")
        match = pattern.search(data)
        if match:
            ver = match.group(1).decode("ascii", errors="ignore").strip()
            bid = match.group(2).decode("ascii", errors="ignore").strip().split("\x00")[0] if match.group(2) else "unknown"
            return ver, bid, has_board_lcd
        return None, None, has_board_lcd
    except Exception as e:
        print(f"ERROR: Failed to extract LCD marker: {e}", file=sys.stderr)
        return None, None, False


def validate_lcd_marker(bin_path, expected_version):
    """
    Fail closed unless marker contains ALL:
    - HALO_FW_MARKER:<version> (must match expected_version)
    - BUILD_ID:
    - BOARD:lcd
    """
    version, build_id, has_board_lcd = find_lcd_marker(bin_path)
    if not version:
        print("ERROR: No HALO_FW_MARKER found in binary", file=sys.stderr)
        print("  Run: strings -a <bin> | grep HALO_FW_MARKER", file=sys.stderr)
        return False, None, None
    if version != expected_version:
        print(f"ERROR: Version mismatch: expected {expected_version}, found {version}", file=sys.stderr)
        return False, None, None
    if "BUILD_ID:" not in get_marker_strings(bin_path):
        print("ERROR: Marker must contain BUILD_ID: (e.g. HALO_FW_MARKER:X.Y.Z|BUILD_ID:...|BOARD:lcd)", file=sys.stderr)
        return False, None, None
    if not has_board_lcd:
        print("ERROR: Marker must contain BOARD:lcd (required for LCD OTA publish)", file=sys.stderr)
        print("  Build LCD with: python3 tools/generate_version_header.py --version X.Y.Z --board lcd", file=sys.stderr)
        return False, None, None
    print(f"✓ Marker validated: version={version}, build_id={build_id}, BOARD:lcd present")
    return True, version, build_id


def upload_to_s3(config, local_path, s3_key, content_type="application/octet-stream", cache_control=None):
    """Upload file to S3. Uses boto3 if available else aws cli."""
    local_path = Path(local_path)
    if not local_path.exists():
        print(f"ERROR: Local file not found: {local_path}", file=sys.stderr)
        return False
    if BOTO3_AVAILABLE and boto3:
        try:
            session = boto3.Session(
                profile_name=config.get("profile"),
                region_name=config.get("region", "us-east-1"),
            )
            s3 = session.client("s3")
            extra = {}
            if cache_control:
                extra["CacheControl"] = cache_control
            s3.upload_file(str(local_path), config["bucket"], s3_key, ExtraArgs={"ContentType": content_type, **extra})
            print(f"✓ Uploaded: {s3_key}")
            return True
        except Exception as e:
            print(f"ERROR: Upload failed: {e}", file=sys.stderr)
            return False
    # Fallback: aws s3 cp
    cmd = build_aws_cmd(config, "s3", "cp", str(local_path), f"s3://{config['bucket']}/{s3_key}", "--content-type", content_type)
    if cache_control:
        cmd.extend(["--cache-control", cache_control])
    r = subprocess.run(cmd)
    if r.returncode != 0:
        print(f"ERROR: aws s3 cp failed", file=sys.stderr)
        return False
    print(f"✓ Uploaded: {s3_key}")
    return True


def main():
    global PUBLISH_OK
    parser = argparse.ArgumentParser(description="Publish LCD OTA from prebuilt binary (no compile)")
    parser.add_argument("--channel", default="dev", help="Channel: dev|prod")
    parser.add_argument("--version", required=True, help="Version X.Y.Z (must match binary marker)")
    parser.add_argument("--bin", required=True, help="Absolute path to LCD app .bin")
    parser.add_argument("--bucket", default=None, help="S3 bucket (default: halo-ota-<channel>)")
    parser.add_argument("--region", default="us-east-1", help="AWS region")
    parser.add_argument("--prefix", default="halo/ota", help="S3 prefix")
    parser.add_argument("--profile", default=None, help="AWS profile")
    parser.add_argument("--dry-run", action="store_true", help="Validate only, do not upload")
    parser.add_argument("--rollout-pct", default=None, type=int, help="Optional rollout percent (0-100)")
    parser.add_argument("--rollout-seed", default=None, type=int, help="Optional rollout seed (int)")
    parser.add_argument("--min-version-allowed", default=None, help="Optional rollout min version gate")
    args = parser.parse_args()

    try:
        args.bin = str(validate_publishable_artifact(args.bin))
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    repo_root = Path(__file__).resolve().parent.parent.parent
    lcd_partitions = repo_root / "firmware" / "halo_lcd_prod" / "partitions.csv"
    try:
        ota_slot_bytes = parse_ota_slot_bytes(lcd_partitions)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    bin_path = Path(args.bin)
    if not bin_path.is_absolute():
        bin_path = bin_path.resolve()
    if not bin_path.exists():
        print(f"ERROR: BIN does not exist: {bin_path}", file=sys.stderr)
        sys.exit(1)
    if not bin_path.is_file():
        print(f"ERROR: BIN is not a file: {bin_path}", file=sys.stderr)
        sys.exit(1)
    exclude_tokens = ("merged", "bootloader", "partitions", "full", "factory", "flash")
    if any(tok in bin_path.name.lower() for tok in exclude_tokens):
        print("ERROR: BIN appears to be merged/full/bootloader/partitions artifact", file=sys.stderr)
        print(f"  Provided: {bin_path.name}", file=sys.stderr)
        print(f"  Exclude any file containing: {', '.join(exclude_tokens)}", file=sys.stderr)
        sys.exit(1)

    # Size gate
    size = get_file_size(bin_path)
    print(f"\n--- Partition-size gate ---")
    print(f"OTA_SLOT_BYTES={ota_slot_bytes} (from {lcd_partitions})")
    print(f"Binary size: {size} bytes")
    if size > ota_slot_bytes:
        print(f"ERROR: Binary exceeds OTA slot ({size} > {ota_slot_bytes})", file=sys.stderr)
        sys.exit(1)
    print(f"  ✓ Size fits OTA slot")

    # Marker validation (must match --version)
    print("\nValidating marker...")
    valid, version, build_id = validate_lcd_marker(bin_path, args.version)
    if not valid:
        sys.exit(1)
    if not build_id or build_id == "unknown":
        build_id = f"{version}-{datetime.now(timezone.utc).strftime('%b %d %Y-%H:%M:%S')}-lcd"

    # SHA256
    print("\nComputing SHA256...")
    sha256 = compute_sha256(bin_path)
    print(f"  SHA256: {sha256}")
    print(f"  Size: {size} bytes")
    print(f"PUBLISH_PROOF version={version} build_id={build_id} sha256={sha256} size={size} slot_bytes={ota_slot_bytes}")

    # Config
    config = get_aws_config(
        channel=args.channel,
        bucket=args.bucket or f"halo-ota-{args.channel}",
        prefix=args.prefix,
        region=args.region,
        profile=args.profile,
    )

    # Paths: halo/ota/<channel>/lcd/artifacts/lcd_<version>_<timestamp>.bin
    timestamp = datetime.now(timezone.utc).strftime("%b_%d_%Y_%H-%M-%S").replace(" ", "_")
    artifact_name = f"lcd_{version}_{timestamp}.bin"
    channel_lcd = f"{config['prefix']}/{config['channel']}/lcd"
    artifact_s3_key = f"{channel_lcd}/artifacts/{artifact_name}"
    manifest_versioned_key = f"{channel_lcd}/manifest_{version}.json"
    manifest_latest_key = f"{channel_lcd}/manifest_latest.json"

    # bin_url: base_url already includes prefix (e.g. https://bucket.s3.region/halo/ota)
    base_url = config["base_url"].rstrip("/")
    artifact_rel_path = f"{config['channel']}/lcd/artifacts/{artifact_name}"
    bin_url = f"{base_url}/{artifact_rel_path}"

    # Manifest content
    published_at_utc = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    try:
        publisher_host = __import__("socket").gethostname() or "local"
    except Exception:
        publisher_host = "local"
    manifest = generate_manifest(
        version=version,
        bin_url=bin_url,
        sha256=sha256,
        size=size,
        build_id=build_id,
        artifact_fw_version=version,
        min_version="0.0.0",
        max_slot_bytes=ota_slot_bytes,
        published_at_utc=published_at_utc,
        publisher_host=publisher_host,
        rollout_pct=args.rollout_pct,
        rollout_seed=args.rollout_seed,
        min_version_allowed=args.min_version_allowed,
        board="lcd",
    )
    manifest_json = json.dumps(manifest, indent=2)

    # Unit-style confirmation: firmware manifest URL and publisher keys must match
    # Firmware fetches: https://<bucket>.s3.<region>.amazonaws.com/halo/ota/<channel>/lcd/manifest_latest.json
    # Publisher writes: halo/ota/<channel>/lcd/manifest_latest.json and halo/ota/<channel>/lcd/artifacts/...
    manifest_url = f"https://{config['bucket']}.s3.{config['region']}.amazonaws.com/{manifest_latest_key}"
    print("\n--- URL alignment (firmware vs publisher) ---")
    print(f"  manifest_url = {manifest_url}")
    print(f"  bin_url      = {bin_url}")

    if args.dry_run:
        print("\n[DRY-RUN] Would upload:")
        print(f"  Binary: s3://{config['bucket']}/{artifact_s3_key}")
        print(f"  Manifest (versioned): s3://{config['bucket']}/{manifest_versioned_key}")
        print(f"  Manifest (latest): s3://{config['bucket']}/{manifest_latest_key}")
        print(f"\nMANIFEST_URL=https://{config['bucket']}.s3.{config['region']}.amazonaws.com/{manifest_latest_key}")
        PUBLISH_OK = True
        return 0

    # AWS identity check
    print("\nChecking AWS identity...")
    cmd = build_aws_cmd(config, "sts", "get-caller-identity")
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print("ERROR: AWS credentials failed", file=sys.stderr)
        sys.exit(1)

    if not ensure_s3_bucket(config):
        sys.exit(1)

    # Upload: artifact -> versioned manifest -> manifest_latest last (Cache-Control no-store)
    print("\nUploading to S3...")
    dist_dir = repo_root / "dist" / "lcd" / config["channel"]
    dist_dir.mkdir(parents=True, exist_ok=True)
    artifacts_dir = dist_dir / "artifacts"
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    dist_bin = artifacts_dir / artifact_name
    shutil.copy2(bin_path, dist_bin)

    if not upload_to_s3(config, dist_bin, artifact_s3_key, "application/octet-stream", "public, max-age=31536000, immutable"):
        sys.exit(1)
    print("\nVerifying artifact upload (HEAD)...")
    if not s3_head_object(config, artifact_s3_key, "artifact"):
        sys.exit(1)

    manifest_versioned_file = dist_dir / f"manifest_{version}.json"
    manifest_latest_file = dist_dir / "manifest_latest.json"
    manifest_versioned_file.write_text(manifest_json)
    manifest_latest_file.write_text(manifest_json)

    if not upload_to_s3(config, manifest_versioned_file, manifest_versioned_key, "application/json", "public, max-age=31536000, immutable"):
        sys.exit(1)
    print("\nVerifying versioned manifest upload (HEAD)...")
    if not s3_head_object(config, manifest_versioned_key, "manifest_versioned.json"):
        sys.exit(1)

    print("\nUploading manifest_latest.json (last, no-cache)...")
    if not upload_to_s3(config, manifest_latest_file, manifest_latest_key, "application/json", "no-store, no-cache, max-age=0, must-revalidate"):
        sys.exit(1)

    # Post-publish readback: manifest_latest must match expected values and size <= slot
    post_publish_readback(manifest_url, version, build_id, bin_url, size, ota_slot_bytes)

    # Optional artifact verification (download and re-hash)
    verify_uploaded_artifact(bin_url, sha256, size, manifest_url=manifest_url, config=config)

    # S3 HEAD proof for manifest_latest + artifact
    if not s3_head_proof(config, manifest_latest_key, artifact_s3_key):
        print("ERROR: S3 head-object proof failed", file=sys.stderr)
        sys.exit(1)

    print("\n" + "=" * 60)
    print("LCD OTA published successfully")
    print("=" * 60)
    print(f"Version: {version}")
    print(f"Channel: {config['channel']}")
    print(f"MANIFEST_URL={manifest_url}")
    print(f"BIN_URL={bin_url}")
    PUBLISH_OK = True
    return 0


if __name__ == "__main__":
    register_publish_exit_guard()
    sys.exit(main())
