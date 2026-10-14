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


class CppProducerEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        base = Path(self.directory.name)
        inputs = Path(__file__).resolve().parents[2] / "unittests/Inputs/source-patch"
        self.root = base / "cpp-linux-objects"
        shutil.copytree(inputs / "cpp-linux-objects", self.root)
        (base / "neverc-linux").mkdir()
        shutil.copyfile(inputs / "neverc-linux/exceptions.cpp", base / "neverc-linux/exceptions.cpp")

    def test_real_clang_and_gcc_objects_cover_both_native_architectures(self):
        rows = checker.cpp_objects(self.root)
        self.assertEqual(set(rows), {"x64", "arm64"})
        for row in rows.values():
            self.assertEqual(set(row["objects"]), {"clang", "gcc"})

    def test_changed_object_source_and_machine_cannot_qualify(self):
        path = self.root / "arm64/clang.o"
        original = path.read_bytes()
        path.write_bytes(original + b"changed")
        with self.assertRaisesRegex(ValueError, "digest"):
            checker.cpp_objects(self.root)
        path.write_bytes(original)
        source = self.root.parent / "neverc-linux/exceptions.cpp"
        source.write_bytes(source.read_bytes() + b"changed")
        with self.assertRaisesRegex(ValueError, "digest"):
            checker.cpp_objects(self.root)
        source.write_bytes(source.read_bytes()[:-7])
        changed = bytearray(original)
        changed[18:20] = (62).to_bytes(2, "little")
        path.write_bytes(changed)
        manifest_path = self.root / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        arm64 = next(row for row in manifest["entries"] if row["architecture"] == "arm64")
        arm64["objects"]["clang"]["sha256"] = checker.pe.digest(path)
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "machine"):
            checker.cpp_objects(self.root)

    def test_neverc_and_incomplete_or_escaping_cpp_inventory_are_rejected(self):
        path = self.root / "manifest.json"
        baseline = json.loads(path.read_text())
        for mutation in (
                lambda m: m.update(language="c"),
                lambda m: m["entries"].pop(),
                lambda m: m["entries"][0]["objects"].pop("gcc"),
                lambda m: m["entries"][0]["objects"]["gcc"].update(version="NeverC"),
                lambda m: m["entries"][0]["objects"]["gcc"].update(path="../outside.o")):
            manifest = json.loads(json.dumps(baseline))
            mutation(manifest)
            path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                checker.cpp_objects(self.root)


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

    def test_bitcode_cannot_be_relabelled_as_a_native_elf_producer(self):
        root = self.root / "producer-fixtures"
        shutil.copytree(self.fixtures, root)
        path = root / "arm64/c.o"
        path.write_bytes(b"BC\xc0\xde" + b"bitcode" * 10)
        manifest_path = root / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        row = next(row for row in manifest["entries"] if row["architecture"] == "arm64")
        row["files"]["c.o"]["sha256"] = checker.pe.digest(path)
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "ELF64"):
            checker.fixtures(root)

    def test_independent_complete_inventory_has_120_observations(self):
        run = self.execute()
        self.assertEqual(run.call_count, 120)
        report = json.loads((self.root / "arm64-runtime.json").read_text())
        self.assertEqual(report["native_host"], "arm64")
        self.assertEqual(len(report["observations"]), 120)
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
