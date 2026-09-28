"""Host-only dependency paths for local and isolated firmware test environments.

Arduino's own directory overrides are shared with the test suite. No defaults
change on existing benches, and no firmware or device configuration is touched.
"""
import os
from pathlib import Path


def _directory(variable, default):
    value = os.environ.get(variable)
    if value is None:
        return default
    path = Path(value)
    if not value or not path.is_absolute():
        raise ValueError(variable + " must name an absolute directory")
    return path


def arduino_user():
    return _directory("ARDUINO_DIRECTORIES_USER", Path.home() / "Documents/Arduino")


def arduino_data():
    return _directory("ARDUINO_DIRECTORIES_DATA", Path.home() / "Library/Arduino15")


def mbedtls_prefix():
    return _directory("TREPO_MBEDTLS_PREFIX", Path("/opt/homebrew/opt/mbedtls"))


def artifact_checker():
    return _directory("TREPO_ARTIFACT_CHECKER", Path(
        "/Users/MattTaylor/halo-provision-memory202-20260921/service203-prep/check_release_artifacts_203.py"))


def negative_source_root():
    return _directory("TREPO_REGRESSION_NEGATIVE_ROOT", Path.home() /
        "halo-device-analytics-2026-09-10/offline-backup-20260915/candidate163-002/snapshot/source")
