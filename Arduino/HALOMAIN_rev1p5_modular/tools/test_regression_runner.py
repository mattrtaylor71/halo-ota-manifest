#!/usr/bin/env python3
"""Test the actual host gate with temporary fake suites; never run its real catalog.

Only local fixture children are spawned. No serial, network, firmware build,
hardware catalog entry, or real release source is executed or modified.
"""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "regression_runner_under_test", Path(__file__).with_name("run_regression_suite.py"))
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="halo-runner-meta-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name).resolve()
        self.root = self.base / "source"
        (self.root / "tools").mkdir(parents=True)
        (self.root / "runtime.h").write_text("original runtime\n")
        self.suites = []
        self.deps = {"negative_source_root": str(self.base / "unused-negative"),
                     "history_git_dir": str(self.base / "unused-history"),
                     "history_root": str(self.base / "unused-history-root")}

    def suite(self, name="test_fake", body="print('fixture pass')\n"):
        path = "tools/" + name + ".py"
        (self.root / path).write_text(body)
        case = {"id": name, "path": path, "args": [], "timeout_seconds": 5}
        self.suites.append(case)
        self.catalog()
        return case

    def catalog(self):
        (self.root / runner.CATALOG).write_text(json.dumps(
            {"schema_version": 1, "suites": self.suites}))

    def materialization(self):
        data = {"source_root": str(self.root),
                "source_snapshot": runner.fingerprint(self.root),
                "original_source_root": str(self.root),
                "git_commit": "fixture-not-a-release", "build_id": "fixture"}
        path = self.base / "materialization.json"
        path.write_text(json.dumps(data))
        return path

    def gate(self, preflight_error=None):
        materialization = self.materialization()
        out = self.base / "results"
        argv = ["run_regression_suite.py", "--source-root", str(self.root),
                "--materialization", str(materialization), "--out", str(out),
                "--jobs", "2"]
        with patch.object(sys, "argv", argv), \
                patch.object(runner, "preflight", return_value=self.deps,
                             side_effect=preflight_error), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            code = runner.main()
        return code, json.loads((out / "RESULT.json").read_text())

    def test_catalog_requires_complete_and_unique_inventory(self):
        self.suite()
        self.assertEqual(runner.load_catalog(self.root), self.suites)
        (self.root / "tools/test_new_unlisted.py").write_text("raise AssertionError\n")
        with self.assertRaisesRegex(ValueError, "Catalog drift"):
            runner.load_catalog(self.root)
        (self.root / "tools/test_new_unlisted.py").unlink()
        self.suites.append(dict(self.suites[0])); self.catalog()
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            runner.load_catalog(self.root)

    def test_missing_catalog_and_missing_entry_refuse(self):
        with self.assertRaises(FileNotFoundError):
            runner.load_catalog(self.root)
        case = self.suite()
        (self.root / case["path"]).unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            runner.load_catalog(self.root)

    def test_hardware_path_is_rejected_without_running_it(self):
        sentinel = self.base / "must-not-exist"
        path = "tools/wifi_coldstart_test.py"
        (self.root / path).write_text("from pathlib import Path\nPath(" +
                                     repr(str(sentinel)) + ").touch()\n")
        self.suites = [{"id": "wifi_coldstart_test", "path": path,
                        "args": [], "timeout_seconds": 5}]
        self.catalog()
        with self.assertRaisesRegex(ValueError, "missing_or_unsafe"):
            runner.load_catalog(self.root)
        self.assertFalse(sentinel.exists())

    def test_full_fingerprint_detects_untracked_nonruntime_add_edit_delete(self):
        self.suite()
        before = runner.fingerprint(self.root)
        extra = self.root / "notes.txt"; extra.write_text("new\n")
        added = runner.fingerprint(self.root)
        self.assertNotEqual(before, added); self.assertIn("notes.txt", added)
        extra.write_text("edited\n")
        self.assertNotEqual(added, runner.fingerprint(self.root))
        extra.unlink(); self.assertEqual(before, runner.fingerprint(self.root))
        (self.root / "runtime.h").unlink()
        self.assertNotEqual(before, runner.fingerprint(self.root))

    def test_symlink_source_refused(self):
        (self.root / "linked.h").symlink_to(self.root / "runtime.h")
        with self.assertRaisesRegex(ValueError, "symlink"):
            runner.fingerprint(self.root)

    def test_materialization_requires_complete_map_and_exact_root(self):
        self.suite(); path = self.materialization()
        files = runner.fingerprint(self.root)
        data, receipt = runner.materialized_source(self.root, path, files)
        self.assertEqual(data["source_snapshot"], files)
        self.assertEqual(receipt["sha256"], runner.sha(path))
        for change in ["map", "root"]:
            bad = dict(data)
            if change == "map": bad["source_snapshot"] = {}
            else: bad["source_root"] = str(self.base / "wrong-source")
            path.write_text(json.dumps(bad))
            with self.assertRaisesRegex(ValueError, "Materialization"):
                runner.materialized_source(self.root, path, files)

    def test_dependency_failure_is_not_pass_and_spawns_nothing(self):
        self.suite()
        with patch.object(runner.shutil, "which", return_value=None):
            with self.assertRaisesRegex(ValueError, "prerequisites"):
                runner.preflight(self.root, self.suites, self.root, self.root)
        with patch.object(runner, "run_case", side_effect=AssertionError("must not spawn")) as run:
            code, result = self.gate(ValueError("Missing host prerequisites: fixture"))
        self.assertEqual(code, 1); self.assertEqual(result["status"], "FAIL")
        self.assertEqual(result["cases"], []); run.assert_not_called()

    def test_failure_aggregation_keeps_pass_and_fail(self):
        self.suite("test_pass")
        self.suite("test_fail", "raise SystemExit(7)\n")
        code, result = self.gate()
        self.assertEqual(code, 1); self.assertEqual(result["status"], "FAIL")
        self.assertEqual({x["id"]: x["status"] for x in result["cases"]},
                         {"test_pass": "PASS", "test_fail": "FAIL"})
        self.assertTrue(result["source_unchanged_after_tests"])

    def test_passing_fixture_only_not_real_catalog(self):
        self.suite(); code, result = self.gate()
        self.assertEqual(code, 0); self.assertEqual(result["status"], "PASS")
        self.assertEqual(result["suite_count"], 1)
        self.assertFalse(result["physical_validation"])

    def test_mutating_suite_cannot_pass(self):
        self.suite(body="from pathlib import Path\nPath('runtime.h').write_text('mutated')\n")
        code, result = self.gate()
        self.assertEqual(code, 1); self.assertEqual(result["status"], "FAIL")
        self.assertFalse(result["source_unchanged_after_tests"])
        self.assertEqual(result["changed_paths"], ["runtime.h"])

    def test_silent_unittest_skip_is_failure(self):
        case = self.suite(body="print('OK (skipped=1)')\n")
        out = self.base / "skip"; out.mkdir()
        with contextlib.redirect_stdout(io.StringIO()):
            result = runner.run_case(self.root, out, case, self.deps)
        self.assertEqual(result["exit_code"], 0)
        self.assertTrue(result["skipped"]); self.assertEqual(result["status"], "FAIL")

    def test_environment_clears_inherited_baseline_and_python_overrides(self):
        injected = {"GIT_DIR": "wrong", "HALO_READINESS_BASELINE_REF": "old",
                    "PYTHONPATH": "wrong", "__PYVENV_LAUNCHER__": "wrong"}
        with patch.dict(os.environ, injected):
            env = runner.clean_environment()
        for key in injected: self.assertNotIn(key, env)
        self.assertEqual(env["PYTHONDONTWRITEBYTECODE"], "1")

    def test_stopping_refuses_new_child_before_spawn(self):
        runner.STOPPING.set()
        try:
            with patch.object(runner.subprocess, "Popen") as spawn:
                with self.assertRaisesRegex(RuntimeError, "refusing new child"):
                    runner.run_owned([sys.executable, "-c", "raise AssertionError"],
                                     cwd=self.root, env=runner.clean_environment(),
                                     log=self.base / "stopping.log", timeout=1)
                spawn.assert_not_called()
            self.assertFalse(runner.RUNNING)
        finally:
            runner.STOPPING.clear()

    def test_cleanup_failure_retains_failed_final_receipt(self):
        self.suite()
        fake_process = object()
        def finished_case(*args):
            with runner.RUNNING_LOCK:
                runner.RUNNING.add(fake_process)
            return {"id": "test_fake", "status": "PASS"}
        try:
            with patch.object(runner, "run_case", side_effect=finished_case), \
                    patch.object(runner, "terminate_group",
                                 side_effect=OSError("fixture cleanup failed")):
                code, result = self.gate()
            self.assertEqual(code, 1); self.assertEqual(result["status"], "FAIL")
            self.assertEqual(result["cleanup_errors"], ["fixture cleanup failed"])
            self.assertEqual(result["cases"][0]["status"], "PASS")
            self.assertFalse(runner.STOPPING.is_set())
        finally:
            with runner.RUNNING_LOCK:
                runner.RUNNING.discard(fake_process)

    def owned_tree(self, normal_exit):
        """Spawn only a known fixture tree; always kill our group after assertion."""
        pidfile = self.base / "tree.json"
        grandchild = "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(60)"
        child = ("import os,json,subprocess,sys,time,signal\n"
                 "signal.signal(signal.SIGTERM,signal.SIG_IGN)\n"
                 "p=subprocess.Popen([sys.executable,'-c'," + repr(grandchild) + "])\n"
                 "open(" + repr(str(pidfile)) + ",'w').write(json.dumps([os.getpid(),p.pid]))\n" +
                 ("time.sleep(.15)\n" if normal_exit else "time.sleep(60)\n"))
        pids = []
        try:
            code = runner.run_owned([sys.executable, "-c", child], cwd=self.root,
                                    env=runner.clean_environment(),
                                    log=self.base / "tree.log", timeout=1)
            pids = json.loads(pidfile.read_text())
            self.assertEqual(code, 0 if normal_exit else 124)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                status = subprocess.run(["ps", "-o", "stat=", "-p", str(pids[1])],
                                        text=True, capture_output=True, timeout=2).stdout.strip()
                if not status or status.startswith("Z"): break
                time.sleep(.02)
            self.assertTrue(not status or status.startswith("Z"),
                            "owned grandchild is still live after runner returned: " + status)
            self.assertFalse(runner.RUNNING)
        finally:
            if not pids and pidfile.exists(): pids = json.loads(pidfile.read_text())
            if pids:
                try: os.killpg(pids[0], signal.SIGKILL)
                except ProcessLookupError: pass

    def test_timeout_kills_owned_child_and_grandchild(self):
        self.owned_tree(False)

    def test_successful_child_cannot_leave_owned_grandchild(self):
        self.owned_tree(True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
