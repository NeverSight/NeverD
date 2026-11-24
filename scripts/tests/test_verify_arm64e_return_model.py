"""Distinguish native ordered execution from skips and inconsistent routes."""

import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify_arm64e_return_model as model


class NativeOrderedReportTests(unittest.TestCase):
    def setUp(self):
        config = model.CONFIG
        self.policy = config["ordered"]["native"]
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        root = Path(self.scratch.name)
        self.guests = root / config["staging_directory"]
        self.guests.mkdir()
        self.output = root / config["test_object_directory"]
        header = struct.pack(config["macho_header"], config["macho_magic"], config["arm64_cpu"],
                             self.policy["subtype"], config["executable_kind"])
        images = {}
        for name, _, _ in model.ordered_routes(self.policy["object_file"]):
            image = self.guests / name
            image.write_bytes(header)
            images[name] = {"sha256": model.digest(image)}
        self.names = list(images)
        model.write_json(self.guests / config["manifest"],
                         {"ordered_profile": self.policy["profile"], "guests": images})

    def outcome(self, scope, status=0):
        records = [self.policy[scope]]
        if scope != "unavailable":
            records.append(self.policy["rules"][0])
        return subprocess.CompletedProcess([], status, "\n".join(records) + "\n", "")

    def run_routes(self, outcomes):
        with mock.patch.object(model.subprocess, "run", side_effect=outcomes):
            return model.run_native_ordered(self.guests, self.output)

    def test_absent_hardware_preserves_skip_exit(self):
        skip = self.policy["skip_exit"]
        self.assertEqual(self.run_routes([self.outcome("unavailable", skip)] * len(self.names)), skip)
        report = json.loads((self.output / model.CONFIG["report"]).read_text())
        self.assertFalse(report["gpr_executed"])
        self.assertFalse(report["simd_executed"])

    def test_gpr_only_does_not_qualify_simd(self):
        self.assertEqual(self.run_routes([self.outcome("gpr")] * len(self.names)), 0)
        report = json.loads((self.output / model.CONFIG["report"]).read_text())
        self.assertTrue(report["gpr_executed"])
        self.assertFalse(report["simd_executed"])

    def test_routes_must_agree_on_simd_execution(self):
        outcomes = [self.outcome("simd")] * len(self.names)
        outcomes[-1] = self.outcome("gpr")
        with self.assertRaisesRegex(ValueError, self.policy["inconsistent"]):
            self.run_routes(outcomes)

    def test_skip_text_cannot_accompany_success(self):
        with self.assertRaises(RuntimeError):
            self.run_routes([self.outcome("unavailable")])
