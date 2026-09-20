#!/usr/bin/env python3
"""Read-only, offline contract gate for a sanitized backend inventory snapshot.

This reads one local JSON file. It does not contact the backend, inspect local
credentials, run the inventory collector, or establish live end-to-end behavior.
PASS means only that the supplied snapshot satisfies this pinned contract.
"""

import argparse
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import sys


MAX_INVENTORY_BYTES = 1024 * 1024
FUNCTIONS = {
    "presign": (
        "trepo-grocery-backend-dev-PresignFunction-EOb8DRx0IhAC",
        "UHOCo+aIbKfSiAsKtejcYGSsTZWX5Z0/GCZ6ATFnmQk=",
    ),
    "voice_ingest": (
        "trepo-quick-ack-async-ingest-dev",
        "lCzBjg10AxQ8SsIb1mz2qgO56FvGou5oEnOYvUt1/W0=",
    ),
    "voice_worker": (
        "trepo-quick-ack-async-worker-dev",
        "XO7na5WlcDkJpbETnBgFv8Nylt3Gx5qVya5MJ3j34CA=",
    ),
}
REQUIRED_CHECKS = (
    "image_all_account_admission",
    "trepo-quick-ack-async-ingest-dev_no_owner_gate",
    "trepo-quick-ack-async-worker-dev_no_owner_gate",
    "trepo-grocery-uploads-dev_upload_trigger",
    "trepo-grocery-discards-dev_upload_trigger",
    "trepo-grocery-uploads-prod_upload_trigger",
    "trepo-grocery-discards-prod_upload_trigger",
    "presign_route_exists",
)
# Match the inventory collector's cohort/allowlist selection, plus legacy
# whitelist spelling and the image-owner admission key if used on voice.
OWNER_GATE_KEY = re.compile(
    r"ALLOWLIST|WHITELIST|COHORT|ALLOWED.*OWNER|OWNER.*ALLOWED|UPLOAD_ADMISSION_OWNERS",
    re.IGNORECASE,
)


def _positive_hours(value):
    try:
        hours = float(value)
    except (ValueError, TypeError):
        raise argparse.ArgumentTypeError("max age must be a positive finite number") from None
    if not math.isfinite(hours) or hours <= 0:
        raise argparse.ArgumentTypeError("max age must be a positive finite number")
    return hours


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def load_inventory(path):
    """Bound the read and reject ambiguous JSON; never include input in errors."""
    with Path(path).open("rb") as stream:
        raw = stream.read(MAX_INVENTORY_BYTES + 1)
    if len(raw) > MAX_INVENTORY_BYTES:
        raise ValueError("inventory is too large")
    return json.loads(raw, object_pairs_hook=_unique_object)


def _named_records(value, label, errors):
    records = {}
    if not isinstance(value, list):
        errors.append(f"{label}: expected a list of named records")
        return records
    for record in value:
        if not isinstance(record, dict) or not isinstance(record.get("name"), str) or not record["name"]:
            errors.append(f"{label}: invalid named record")
            continue
        name = record["name"]
        if name in records:
            errors.append(f"{label}: duplicate record name")
        else:
            records[name] = record
    return records


def verify_inventory(inventory, max_age_hours=24, now=None):
    """Return static diagnostics only; supplied environment values stay private."""
    errors = []
    max_age_hours = _positive_hours(max_age_hours)
    if not isinstance(inventory, dict):
        return ["inventory: expected an object"]

    now = now or datetime.now(timezone.utc)
    stamp = inventory.get("at")
    try:
        if not isinstance(stamp, str):
            raise ValueError()
        captured = datetime.fromisoformat(stamp.replace("Z", "+00:00"))
        if captured.tzinfo is None or captured.utcoffset() is None:
            raise ValueError()
        age_hours = (now - captured).total_seconds() / 3600
        if age_hours < 0:
            errors.append("freshness: snapshot timestamp is in the future")
        elif age_hours > max_age_hours:
            errors.append("freshness: snapshot exceeds max age")
    except (TypeError, ValueError, OverflowError):
        errors.append("freshness: missing or invalid timezone-aware snapshot timestamp")

    functions = _named_records(inventory.get("functions"), "functions", errors)
    for role, (name, expected_sha) in FUNCTIONS.items():
        function = functions.get(name)
        if function is None:
            errors.append(f"{role}: required function missing")
            continue
        if function.get("code_sha256") != expected_sha:
            errors.append(f"{role}: deployed code SHA256 does not match pinned contract")
        if function.get("state") != "Active" or function.get("last_update") != "Successful":
            errors.append(f"{role}: function must be Active with Successful last update")
        settings = function.get("settings")
        if not isinstance(settings, dict):
            errors.append(f"{role}: settings must be present as an object")
            continue
        if role == "presign":
            if settings.get("UPLOAD_ADMISSION_V1") != "true":
                errors.append("presign: UPLOAD_ADMISSION_V1 must be the string true")
            if "UPLOAD_ADMISSION_OWNERS" in settings:
                errors.append("presign: UPLOAD_ADMISSION_OWNERS must be absent")
        else:
            if any(not isinstance(key, str) or OWNER_GATE_KEY.search(key) for key in settings):
                errors.append(f"{role}: owner cohort or allowlist setting must be absent")
            if role == "voice_worker":
                if settings.get("ACTION_MODE") != "real":
                    errors.append("voice_worker: ACTION_MODE must be real")
                if settings.get("WRITE_SHARED_ONLY") != "true":
                    errors.append("voice_worker: WRITE_SHARED_ONLY must be the string true")

    checks = _named_records(inventory.get("checks"), "checks", errors)
    for name in REQUIRED_CHECKS:
        if name not in checks:
            errors.append(f"check {name}: required evidence missing")
        elif checks[name].get("pass") is not True:
            errors.append(f"check {name}: must explicitly pass")
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inventory", help="local sanitized inventory JSON file; never refreshed automatically")
    parser.add_argument(
        "--max-age-hours", type=_positive_hours, default=24,
        help="positive freshness limit (default: 24); a larger limit permits historical evidence, not current live proof",
    )
    args = parser.parse_args(argv)
    try:
        inventory = load_inventory(args.inventory)
        errors = verify_inventory(inventory, args.max_age_hours)
    except (OSError, ValueError, RecursionError):
        # JSON values, environment values, and OS error paths may be sensitive.
        errors = ["inventory: cannot read valid, unique-key JSON within the 1 MiB limit"]
    print(json.dumps({
        "status": "FAIL" if errors else "PASS",
        "scope": "offline_snapshot_contract_only",
        "live_e2e_verified": False,
        "max_age_hours": args.max_age_hours,
        "errors": errors,
    }, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
