#!/usr/bin/env python3
"""Offline regression checks for the production LCD publisher's slot gate."""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import publish_lcd_ota as publisher


class LcdPublishSlotTests(unittest.TestCase):
    SLOT_BYTES = 0x280000

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def table(self, contents):
        path = self.root / "partitions.csv"
        path.write_text(contents)
        return path

    def test_production_partition_size(self):
        root = Path(publisher.__file__).resolve().parents[2]
        table = root / "firmware" / "halo_lcd_prod" / "partitions.csv"
        self.assertEqual(publisher.parse_ota_slot_bytes(table), self.SLOT_BYTES)

    def test_asymmetric_slots_use_smaller_inactive_capacity(self):
        path = self.table("app0,app,ota_0,0x10000,0x300000\n"
                          "app1,app,ota_1,0x310000,2621440\n")
        self.assertEqual(publisher.parse_ota_slot_bytes(path), self.SLOT_BYTES)

    def test_missing_table_fails_closed(self):
        with self.assertRaises(ValueError):
            publisher.parse_ota_slot_bytes(self.root / "missing.csv")

    def test_missing_slot_fails_closed(self):
        with self.assertRaises(ValueError):
            publisher.parse_ota_slot_bytes(self.table("app0,app,ota_0,0x10000,0x280000\n"))

    def test_invalid_or_duplicate_slot_fails_closed(self):
        for size in ("bad", "0", "-1"):
            with self.subTest(size=size), self.assertRaises(ValueError):
                publisher.parse_ota_slot_bytes(self.table(
                    f"app0,app,ota_0,0x10000,{size}\napp1,app,ota_1,0x290000,0x280000\n"))
        with self.assertRaises(ValueError):
            publisher.parse_ota_slot_bytes(self.table(
                "app0,app,ota_0,0x10000,0x280000\napp1,app,ota_0,0x290000,0x280000\n"))

    def dry_run(self, size):
        binary = self.root / "halo_lcd_prod.ino.bin"
        marker = b"HALO_FW_MARKER:6.4.9|BUILD_ID:fixture|BOARD:lcd\x00"
        binary.write_bytes(marker + b"\xff" * (size - len(marker)))
        argv = ["publish_lcd_ota.py", "--channel", "prod", "--version", "6.4.9",
                "--bin", str(binary), "--dry-run"]
        output = io.StringIO()
        # Any attempted AWS call is a test failure, even in error paths.
        with patch.object(sys, "argv", argv), \
             patch.object(publisher, "ensure_s3_bucket", side_effect=AssertionError("AWS called")), \
             patch.object(publisher, "generate_manifest", wraps=publisher.generate_manifest) as manifest, \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            try:
                status = publisher.main()
            except SystemExit as exc:
                status = exc.code
        return status, output.getvalue(), manifest

    def test_exact_slot_size_passes_and_manifest_uses_production_capacity(self):
        status, output, manifest = self.dry_run(self.SLOT_BYTES)
        self.assertEqual(status, 0, output)
        self.assertEqual(manifest.call_args.kwargs["max_slot_bytes"], self.SLOT_BYTES)
        self.assertEqual(manifest.call_args.kwargs["size"], self.SLOT_BYTES)
        self.assertEqual(manifest.call_args.kwargs["board"], "lcd")
        self.assertIn("firmware/halo_lcd_prod/partitions.csv", output)

    def test_one_byte_over_physical_slot_rejected_before_manifest(self):
        status, output, manifest = self.dry_run(self.SLOT_BYTES + 1)
        self.assertEqual(status, 1, output)
        self.assertIn("Binary exceeds OTA slot", output)
        manifest.assert_not_called()


if __name__ == "__main__":
    unittest.main()
