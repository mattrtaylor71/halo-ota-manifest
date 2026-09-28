#!/usr/bin/env python3
"""Verify isolated host dependencies keep legacy defaults and fail closed."""
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

import host_paths
from run_regression_suite import clean_environment


class HostPathsTest(unittest.TestCase):
    def test_existing_bench_defaults(self):
        env = {k: v for k, v in os.environ.items() if k not in
               ("ARDUINO_DIRECTORIES_USER", "ARDUINO_DIRECTORIES_DATA", "TREPO_MBEDTLS_PREFIX",
                "TREPO_ARTIFACT_CHECKER", "TREPO_REGRESSION_NEGATIVE_ROOT")}
        with patch.dict(os.environ, env, clear=True):
            self.assertEqual(host_paths.arduino_user(), Path.home() / "Documents/Arduino")
            self.assertEqual(host_paths.arduino_data(), Path.home() / "Library/Arduino15")
            self.assertEqual(host_paths.mbedtls_prefix(), Path("/opt/homebrew/opt/mbedtls"))
            self.assertEqual(host_paths.artifact_checker(), Path(
                "/Users/MattTaylor/halo-provision-memory202-20260921/service203-prep/check_release_artifacts_203.py"))
            self.assertEqual(host_paths.negative_source_root(), Path.home() /
                "halo-device-analytics-2026-09-10/offline-backup-20260915/candidate163-002/snapshot/source")

    def test_explicit_overrides_reach_sanitized_test_children(self):
        overrides = {"ARDUINO_DIRECTORIES_USER": "/isolated/arduino-user",
                     "ARDUINO_DIRECTORIES_DATA": "/isolated/arduino-data",
                     "TREPO_MBEDTLS_PREFIX": "/isolated/mbedtls",
                     "TREPO_ARTIFACT_CHECKER": "/isolated/checker.py",
                     "TREPO_REGRESSION_NEGATIVE_ROOT": "/isolated/negative163/source"}
        with patch.dict(os.environ, overrides):
            env = clean_environment()
            for key, value in overrides.items():
                self.assertEqual(env[key], value)
            result = subprocess.run([sys.executable, "-B", "-c",
                "from host_paths import *; print(arduino_user()); print(arduino_data()); print(mbedtls_prefix()); print(artifact_checker()); print(negative_source_root())"],
                cwd=Path(__file__).parent, env=env, capture_output=True, text=True,
                check=True, timeout=10)
        self.assertEqual(result.stdout.splitlines(), list(overrides.values()))

    def test_invalid_override_does_not_silently_use_global_toolchain(self):
        for variable, getter in (("ARDUINO_DIRECTORIES_USER", host_paths.arduino_user),
                                 ("ARDUINO_DIRECTORIES_DATA", host_paths.arduino_data),
                                 ("TREPO_MBEDTLS_PREFIX", host_paths.mbedtls_prefix),
                                 ("TREPO_ARTIFACT_CHECKER", host_paths.artifact_checker),
                                 ("TREPO_REGRESSION_NEGATIVE_ROOT", host_paths.negative_source_root)):
            for value in ("", "relative/path", "~/toolchain"):
                with self.subTest(variable=variable, value=value), patch.dict(os.environ, {variable: value}):
                    with self.assertRaisesRegex(ValueError, variable):
                        getter()


if __name__ == "__main__":
    unittest.main()
