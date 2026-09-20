#!/usr/bin/env python3
"""Synthetic local fixtures for the offline backend contract gate."""

import copy
from datetime import datetime, timedelta, timezone
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOL = Path(__file__).with_name("verify_backend_scope.py")
spec = importlib.util.spec_from_file_location("verify_backend_scope", TOOL)
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
NOW = datetime(2026, 9, 20, 18, tzinfo=timezone.utc)


def fixture():
    # Independent snapshot fixture: green collector checks do not replace
    # validating the deployed artifact and configuration themselves.
    return {
        "at": "2026-09-20T17:00:00Z",
        "status": "PASS",
        "functions": [
            {
                "name": "trepo-grocery-backend-dev-PresignFunction-EOb8DRx0IhAC",
                "code_sha256": "UHOCo+aIbKfSiAsKtejcYGSsTZWX5Z0/GCZ6ATFnmQk=",
                "state": "Active", "last_update": "Successful",
                "settings": {"UPLOAD_ADMISSION_V1": "true"},
            },
            {
                "name": "trepo-quick-ack-async-ingest-dev",
                "code_sha256": "lCzBjg10AxQ8SsIb1mz2qgO56FvGou5oEnOYvUt1/W0=",
                "state": "Active", "last_update": "Successful", "settings": {},
            },
            {
                "name": "trepo-quick-ack-async-worker-dev",
                "code_sha256": "XO7na5WlcDkJpbETnBgFv8Nylt3Gx5qVya5MJ3j34CA=",
                "state": "Active", "last_update": "Successful",
                "settings": {"ACTION_MODE": "real", "WRITE_SHARED_ONLY": "true"},
            },
        ],
        "checks": [{"name": name, "pass": True} for name in (
            "image_all_account_admission",
            "trepo-quick-ack-async-ingest-dev_no_owner_gate",
            "trepo-quick-ack-async-worker-dev_no_owner_gate",
            "trepo-grocery-uploads-dev_upload_trigger",
            "trepo-grocery-discards-dev_upload_trigger",
            "trepo-grocery-uploads-prod_upload_trigger",
            "trepo-grocery-discards-prod_upload_trigger",
            "presign_route_exists",
        )],
    }


class ContractTests(unittest.TestCase):
    def errors(self, inventory, **kwargs):
        return guard.verify_inventory(inventory, now=NOW, **kwargs)

    def test_valid_snapshot_passes_without_mutating_it(self):
        inventory = fixture()
        before = copy.deepcopy(inventory)
        self.assertEqual(self.errors(inventory), [])
        self.assertEqual(inventory, before)

    def test_stale_deployed_source_fails_despite_green_collector_checks(self):
        for index in range(3):
            with self.subTest(function=index):
                inventory = fixture()
                inventory["functions"][index]["code_sha256"] = "old-deployed-source"
                self.assertTrue(any("SHA256" in error for error in self.errors(inventory)))

    def test_disabled_or_missing_admission_fails_despite_correct_source(self):
        for value in ("false", "", "TRUE", True, None):
            with self.subTest(value=value):
                inventory = fixture()
                inventory["functions"][0]["settings"]["UPLOAD_ADMISSION_V1"] = value
                self.assertTrue(any("UPLOAD_ADMISSION_V1" in error for error in self.errors(inventory)))
        inventory["functions"][0]["settings"].clear()
        self.assertTrue(self.errors(inventory))

    def test_owner_setting_must_be_absent_even_if_empty_or_wildcard(self):
        for value in ("", "*", None, "private-owner"):
            with self.subTest(value=value):
                inventory = fixture()
                inventory["functions"][0]["settings"]["UPLOAD_ADMISSION_OWNERS"] = value
                self.assertTrue(any("must be absent" in error for error in self.errors(inventory)))

    def test_voice_owner_cohorts_are_rejected_for_both_functions(self):
        for index in (1, 2):
            for key in ("VOICE_ALLOWLIST", "VOICE_COHORT", "ALLOWED_VOICE_OWNERS", "OWNER_IDS_ALLOWED", "UPLOAD_ADMISSION_OWNERS"):
                with self.subTest(function=index, key=key):
                    inventory = fixture()
                    inventory["functions"][index]["settings"][key] = ""
                    self.assertTrue(any("owner cohort" in error for error in self.errors(inventory)))

    def test_worker_must_apply_real_actions_to_shared_state(self):
        for key, value in (("ACTION_MODE", "dry_run"), ("WRITE_SHARED_ONLY", "false"), ("WRITE_SHARED_ONLY", True)):
            with self.subTest(key=key, value=value):
                inventory = fixture()
                inventory["functions"][2]["settings"][key] = value
                self.assertTrue(any(key in error for error in self.errors(inventory)))

    def test_inactive_or_incomplete_update_fails(self):
        for index in range(3):
            for key, value in (("state", "Pending"), ("last_update", "InProgress")):
                with self.subTest(function=index, key=key):
                    inventory = fixture()
                    inventory["functions"][index][key] = value
                    self.assertTrue(any("Active" in error for error in self.errors(inventory)))

    def test_missing_and_duplicate_functions_fail(self):
        for index in range(3):
            with self.subTest(function=index):
                inventory = fixture()
                inventory["functions"].pop(index)
                self.assertTrue(any("required function missing" in error for error in self.errors(inventory)))
        inventory = fixture()
        inventory["functions"].append(copy.deepcopy(inventory["functions"][0]))
        self.assertTrue(any("duplicate" in error for error in self.errors(inventory)))

    def test_every_required_check_needs_unique_explicit_passing_evidence(self):
        for index in range(8):
            for mutation in ("missing", False, "PASS", 1, None):
                with self.subTest(check=index, mutation=mutation):
                    inventory = fixture()
                    if mutation == "missing":
                        inventory["checks"].pop(index)
                    else:
                        inventory["checks"][index]["pass"] = mutation
                    self.assertTrue(self.errors(inventory))
        inventory = fixture()
        inventory["checks"].append(copy.deepcopy(inventory["checks"][0]))
        self.assertTrue(any("duplicate" in error for error in self.errors(inventory)))

    def test_freshness_limit_and_explicit_historical_limit(self):
        inventory = fixture()
        inventory["at"] = (NOW - timedelta(hours=24)).isoformat()
        self.assertEqual(self.errors(inventory), [])
        inventory["at"] = (NOW - timedelta(hours=24, seconds=1)).isoformat()
        self.assertTrue(any("max age" in error for error in self.errors(inventory)))
        self.assertEqual(self.errors(inventory, max_age_hours=48), [])

    def test_missing_malformed_naive_and_future_timestamps_fail(self):
        for stamp in (None, "invalid", "2026-09-20T17:00:00", (NOW + timedelta(seconds=1)).isoformat()):
            with self.subTest(stamp=stamp):
                inventory = fixture()
                inventory["at"] = stamp
                self.assertTrue(any("freshness" in error for error in self.errors(inventory)))

    def test_malformed_evidence_fails_without_crashing(self):
        for value in (None, [], "unexpected"):
            self.assertTrue(self.errors(value))
        for key in ("functions", "checks"):
            for value in (None, {}, [None], [{"name": []}]):
                with self.subTest(key=key, value=value):
                    inventory = fixture()
                    inventory[key] = value
                    self.assertTrue(self.errors(inventory))
        inventory = fixture()
        inventory["functions"][0]["settings"] = None
        self.assertTrue(self.errors(inventory))


class CliTests(unittest.TestCase):
    def run_cli(self, content, *args):
        with tempfile.TemporaryDirectory(prefix="backend-scope-fixture-") as directory:
            path = Path(directory) / "snapshot.json"
            path.write_text(content)
            result = subprocess.run(
                [sys.executable, "-B", str(TOOL), str(path), *args],
                capture_output=True, text=True, timeout=5,
            )
            self.assertEqual(path.read_text(), content)
            self.assertEqual(list(Path(directory).iterdir()), [path])
            return result

    def test_cli_pass_identifies_offline_only_evidence(self):
        inventory = fixture()
        inventory["at"] = datetime.now(timezone.utc).isoformat()
        result = self.run_cli(json.dumps(inventory))
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(report["scope"], "offline_snapshot_contract_only")
        self.assertFalse(report["live_e2e_verified"])
        self.assertEqual(report["max_age_hours"], 24)

    def test_failure_output_does_not_expose_input_values(self):
        inventory = fixture()
        inventory["functions"][0]["settings"]["UPLOAD_ADMISSION_V1"] = "private-secret-value"
        inventory["functions"].append({"name": "private-secret-name"})
        inventory["functions"].append({"name": "private-secret-name"})
        result = self.run_cli(json.dumps(inventory))
        self.assertEqual(result.returncode, 1)
        self.assertNotIn("private-secret", result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "FAIL")

    def test_ambiguous_invalid_or_oversized_json_fails_safely(self):
        for content in ('{"secret":"private-secret",', '{"checks":[],"checks":[]}', " " * (1024 * 1024 + 1)):
            with self.subTest(length=len(content)):
                result = self.run_cli(content)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(json.loads(result.stdout)["status"], "FAIL")
                self.assertNotIn("private-secret", result.stdout + result.stderr)
                self.assertNotIn("Traceback", result.stdout + result.stderr)

    def test_max_age_must_be_positive_and_finite(self):
        for age in ("0", "-1", "nan", "inf", "invalid"):
            with self.subTest(age=age):
                result = self.run_cli("{}", "--max-age-hours", age)
                self.assertEqual(result.returncode, 2)
                self.assertIn("positive finite", result.stderr)


if __name__ == "__main__":
    unittest.main()
