from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_source_patch_hosted_pe_runtime as checker


class HostedProducerEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / "fixtures"
        source = Path(__file__).resolve().parents[2] / "unittests/Inputs/source-patch/neverc-hosted"
        shutil.copytree(source, self.root)

    def test_real_pinned_producer_has_object_payload_and_audit_digests(self):
        for arch in ("x64", "arm64"):
            obj, payload = checker.producer_fixture(self.root, arch)
            self.assertTrue(obj.is_file())
            self.assertGreater(len(payload), 40)

    def test_changed_source_payload_object_audit_and_revision_are_rejected(self):
        for name in ("replacement.c", "scalar.c", "x64.obj", "x64.bin", "x64.audit.json",
                     "x64/c.ll", "x64/c.obj", "x64/c-hosted.obj", "x64/c-hosted.bin"):
            with self.subTest(name=name):
                path = self.root / name
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                with self.assertRaisesRegex(ValueError, "digest"):
                    checker.producer_fixture(self.root, "x64")
                path.write_bytes(original)
        path = self.root / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["producer_commit"] = "different"
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "revision"):
            checker.producer_fixture(self.root, "x64")

    def test_pe_code_extraction_bounds_actual_file_backed_extent(self):
        data = bytearray(256)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 70, 1)
        struct.pack_into("<H", data, 84, 64)
        struct.pack_into("<H", data, 88, 0x20b)
        struct.pack_into("<Q", data, 112, 0x140000000)
        struct.pack_into("<IIII", data, 160, 4, 0x1000, 4, 240)
        data[240:244] = b"code"
        path = self.root / "image.exe"
        path.write_bytes(data)
        self.assertEqual(checker.image_bytes(path, 0x140001000, 4), b"code")
        with self.assertRaisesRegex(ValueError, "extent"):
            checker.image_bytes(path, 0x140001000, 5)


class HostedRuntimeEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        data = bytearray(80)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 60, 64)
        data[64:68] = b"PE\0\0"
        struct.pack_into("<H", data, 68, 0xaa64)
        self.manifest = {"schema": checker.SCHEMA, "commit": "verified-revision",
                         "producer_commit": checker.PRODUCER, "images": []}
        for case in checker.CASES:
            path = self.root / (case + ".exe")
            path.write_bytes(data)
            self.manifest["images"].append({"architecture": "arm64", "case": case,
                "path": path.name, "sha256": checker.pe.digest(path)})
        self.args = argparse.Namespace(root=self.root, arch="arm64", runner=None,
                                       expected_native_host="arm64")
        self.write_manifest()

    def write_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def execute(self):
        with patch.dict(checker.os.environ, {"GITHUB_SHA": "verified-revision"}), \
             patch.object(checker.pe, "native_machine", return_value=0xaa64), \
             patch.object(checker.pe, "run", side_effect=lambda command, log, expected:
                          subprocess.CompletedProcess(command, expected)) as run:
            checker.execute(self.args)
            return run

    def test_cpp_ir_objects_and_hosted_frames_require_36_native_observations(self):
        run = self.execute()
        self.assertEqual(run.call_count, 36)
        report = json.loads((self.root / "arm64-runtime.json").read_text())
        self.assertEqual(report["native_host"], "arm64")
        self.assertEqual({row["case"] for row in report["observations"]}, set(checker.CASES))

    def test_wrong_commit_digest_or_inventory_retires_previous_success(self):
        changes = [lambda: self.manifest.update(commit="other"),
                   lambda: self.manifest.update(producer_commit="other"),
                   lambda: self.manifest["images"][0].update(sha256="0" * 64),
                   lambda: self.manifest["images"].pop(),
                   lambda: self.manifest["images"][0].update(case="c-ir")]
        baseline = json.dumps(self.manifest)
        for change in changes:
            self.manifest = json.loads(baseline)
            change()
            self.write_manifest()
            (self.root / "arm64-runtime.json").write_text("stale-success")
            with self.assertRaises(ValueError):
                self.execute()
            self.assertFalse((self.root / "arm64-runtime.json").exists())

    def test_external_or_wrong_architecture_runner_cannot_claim_native(self):
        with patch.object(checker.pe, "native_machine", return_value=0x8664):
            with self.assertRaisesRegex(RuntimeError, "native machine"):
                checker.execute(self.args)
        self.args.runner = Path("external")
        with self.assertRaisesRegex(ValueError, "cannot claim native"):
            checker.execute(self.args)


if __name__ == "__main__":
    unittest.main()
