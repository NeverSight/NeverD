"""Exercise the built native CLI and relocated helper runtime."""

import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from scripts.tests.test_mobile_ios import native_fixture, source_report
from mobile.common import MobileError
from mobile.driver import parser, recover

ROOT = Path(__file__).resolve().parents[2]


class MobilePublicationTests(unittest.TestCase):
    def test_swift_failure_does_not_publish_partial_native_sources(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.macho"
            source.write_bytes(native_fixture())
            output = root / "output"

            def native(argv, log, timeout):
                Path(argv[argv.index("-o") + 1]).write_text(source_report(
                    "int example(void) { return 42; }\n"))
                log.write_text("complete")

            with patch("mobile.ios.run_tool", side_effect=native), \
                    patch("mobile.swift_source.shutil.which", return_value=None), \
                    self.assertRaisesRegex(MobileError, "configured Swift demangler"):
                recover(parser().parse_args([str(source), "-o", str(output),
                                             "--swift-demangle", "/missing/demangler"]))
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".neverd-mobile-*")), [])

    def test_android_rejects_explicit_swift_demangler_option(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.smali"
            source.write_text(".class public LInput;")
            with self.assertRaisesRegex(MobileError, "apply only to iOS"):
                recover(parser().parse_args([str(source), "-o", str(root / "output"),
                                             "--swift-demangle", "tool"]))

    def test_failure_does_not_publish_partial_java(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.smali"
            source.write_text(".class public LInput;")
            output = root / "output"

            def fail(source, staging, **kwargs):
                (staging / "partial.java").write_text("incomplete")
                raise MobileError("assembly failed")

            with patch("mobile.android.decompile_android", side_effect=fail):
                with self.assertRaisesRegex(MobileError, "assembly failed"):
                    recover(parser().parse_args([str(source), "-o", str(output)]))
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".neverd-mobile-*")), [])

    def test_oversized_report_is_rejected_before_any_report_write(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "input.smali"
            source.write_text(".class public LInput;")
            output = root / "output"
            args = parser().parse_args([str(source), "-o", str(output), "--max-bytes", "100"])
            with patch("mobile.android.decompile_android", return_value={"detail": "x" * 101}), \
                    patch.object(Path, "write_text", side_effect=AssertionError("report written before preflight")):
                with self.assertRaisesRegex(MobileError, "limits"):
                    recover(args)
            self.assertFalse(output.exists())
            self.assertEqual(list(root.glob(".neverd-mobile-*")), [])


@unittest.skipUnless(os.environ.get("NEVERD_BUILD_DIR"), "set NEVERD_BUILD_DIR for native CLI tests")
class MobileCLITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build = Path(os.environ["NEVERD_BUILD_DIR"]).resolve()
        name = "neverd.exe" if os.name == "nt" else "neverd"
        candidates = [build / "bin" / name, build / "bin" / "Release" / name,
                      build / "bin" / "Debug" / name]
        cls.binary = next((p for p in candidates if p.is_file()), None)
        if cls.binary is None:
            raise AssertionError("NEVERD_BUILD_DIR has no built neverd executable")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="neverd cli outside ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "App sample.ipa"
        with zipfile.ZipFile(self.source, "w") as archive:
            archive.writestr("Payload/Test.app/Info.plist", plistlib.dumps({"CFBundleExecutable": "Test"}))
            archive.writestr("Payload/Test.app/Test", native_fixture())

    def cli(self, *args, binary=None):
        return subprocess.run([str(binary or self.binary), "mobile", *map(str, args)],
                              cwd=self.root, text=True, capture_output=True, timeout=30)

    def test_ios_metadata_from_ipa_outside_checkout(self):
        output = self.root / "metadata output"
        result = self.cli(self.source, "-o", output, "--metadata-only", "--json", "--python", sys.executable)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "success")
        self.assertEqual(report["platform"], "ios")
        self.assertEqual(report["source"], self.source.name)
        self.assertNotIn(str(self.root), json.dumps(report))
        self.assertEqual(json.loads((output / "report.json").read_text()), report)

    def test_json_failure_and_existing_output_preserved(self):
        output = self.root / "existing"
        output.mkdir()
        (output / "keep").write_text("original")
        result = self.cli(self.source, "-o", output, "--json")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout)["status"], "error")
        self.assertEqual((output / "keep").read_text(), "original")

    def test_explicit_interpreter_error(self):
        result = self.cli(self.source, "-o", self.root / "output", "--python", self.root / "no-python")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("error:", result.stderr)

    def test_relocated_binary_and_runtime(self):
        destination = self.root / "relocated binary"
        destination.mkdir()
        relocated = destination / self.binary.name
        shutil.copy2(self.binary, relocated)
        shutil.copytree(self.binary.parent / "mobile", destination / "mobile", ignore=shutil.ignore_patterns("__pycache__"))
        # Runtime libraries use executable-relative lookup on supported builds.
        for path in self.binary.parent.iterdir():
            if path.is_file() and (path.suffix in (".dylib", ".dll", ".so") or ".so." in path.name):
                shutil.copy2(path, destination / path.name)
        result = self.cli(self.source, "-o", self.root / "relocated output", "--metadata-only", "--json", binary=relocated)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(result.stdout)["status"], "success")
        android = self.root / "owned.smali"
        android.write_text(".class public LOwned;\n.super Ljava/lang/Object;\n"
                           ".method public static value()I\n.registers 1\n"
                           "const/4 v0, 7\nreturn v0\n.end method\n")
        recovered = self.root / "relocated android"
        result = self.cli(android, "-o", recovered, "--json", "--python", sys.executable, binary=relocated)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["backend"]["execution"], "builtin")
        self.assertEqual(report["android_method_recovery"]["recovered_method_count"], 1)
        self.assertTrue((recovered / "sources" / "Owned.java").is_file())


if __name__ == "__main__":
    unittest.main()
