from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_source_patch_elf_runtime as checker


class ELFRuntimeEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.fixtures = Path(__file__).resolve().parents[2] / "unittests/Inputs/source-patch/neverc-linux"
        self.manifest = {"schema": checker.SCHEMA, "commit": "verified-revision",
                         "producer_commit": checker.PRODUCER, "images": []}
        for case in checker.CASES:
            path = self.root / (case + ".elf")
            shutil.copyfile(self.fixtures / "arm64/original-eh.elf", path)
            self.manifest["images"].append({"architecture": "arm64", "case": case,
                "path": path.name, "sha256": checker.pe.digest(path)})
        self.args = argparse.Namespace(root=self.root, arch="arm64")
        self.write_manifest()

    def write_manifest(self):
        (self.root / "manifest.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def execute(self):
        with patch.dict(checker.os.environ, {"GITHUB_SHA": "verified-revision"}), \
             patch.object(checker.platform, "machine", return_value="aarch64"), \
             patch.object(checker.platform, "system", return_value="Linux"), \
             patch.object(checker.pe, "run", side_effect=lambda command, log, expected:
                          subprocess.CompletedProcess(command, expected)) as run:
            checker.execute(self.args)
            return run

    def test_real_producer_inputs_and_native_code_extents_are_authenticated(self):
        rows = checker.fixtures(self.fixtures.resolve())
        self.assertEqual(set(rows), {"x64", "arm64"})
        for arch, row in rows.items():
            image = self.fixtures / arch / "original-eh.elf"
            self.assertEqual(checker.machine(image), row["machine"])
            self.assertEqual(len(checker.image_bytes(image, row["target_va"], 4)), 4)
            with self.assertRaisesRegex(ValueError, "extent"):
                checker.image_bytes(image, 0xffffffffffffffff, 4)

    def test_independent_complete_inventory_has_sixty_six_observations(self):
        run = self.execute()
        self.assertEqual(run.call_count, 66)
        report = json.loads((self.root / "arm64-runtime.json").read_text())
        self.assertEqual(report["native_host"], "arm64")
        self.assertEqual(len(report["observations"]), 66)
        self.assertEqual({row["mode"] for row in report["observations"] if row["case"] == "exceptions"},
                         {0, 1, 2, 3, 4, 5, 6, 7, 9})

    def test_wrong_commit_hash_inventory_or_path_retires_old_success(self):
        changes = [lambda: self.manifest.update(commit="other"),
                   lambda: self.manifest.update(producer_commit="other"),
                   lambda: self.manifest["images"][0].update(sha256="0" * 64),
                   lambda: self.manifest["images"].pop(),
                   lambda: self.manifest["images"][0].update(path="../other.elf"),
                   lambda: self.manifest["images"][0].update(case="c-ir")]
        baseline = json.dumps(self.manifest)
        for change in changes:
            with self.subTest(change=change):
                self.manifest = json.loads(baseline)
                change()
                self.write_manifest()
                (self.root / "arm64-runtime.json").write_text("stale-success")
                with self.assertRaises((ValueError, OSError)):
                    self.execute()
                self.assertFalse((self.root / "arm64-runtime.json").exists())

    def test_emulated_architecture_cannot_claim_native_qualification(self):
        with patch.object(checker.platform, "machine", return_value="x86_64"), \
             patch.object(checker.platform, "system", return_value="Linux"):
            with self.assertRaisesRegex(RuntimeError, "native host"):
                checker.execute(self.args)

    def test_real_runtime_failure_retires_old_success(self):
        (self.root / "arm64-runtime.json").write_text("stale-success")
        with patch.dict(checker.os.environ, {"GITHUB_SHA": "verified-revision"}), \
             patch.object(checker.platform, "machine", return_value="aarch64"), \
             patch.object(checker.platform, "system", return_value="Linux"), \
             patch.object(checker.pe, "run", side_effect=RuntimeError("unwinder failed")):
            with self.assertRaisesRegex(RuntimeError, "unwinder failed"):
                checker.execute(self.args)
        self.assertFalse((self.root / "arm64-runtime.json").exists())


if __name__ == "__main__":
    unittest.main()
