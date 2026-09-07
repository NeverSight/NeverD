"""Android adapter contract tests; real recovery is tested by the backend smoke."""

from __future__ import annotations

import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "neverd"))

from mobile import android
from mobile.common import Limits, MobileError


class AndroidTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "sample.dex"
        self.source.write_bytes(b"dex\n035\0" + b"\0" * 112)
        self.output = self.root / "output"
        self.calls: list[list[str]] = []
        self.code_inputs: list[tuple[str, bytes]] = []
        self.diagnostics = ""
        self.generated = "package example; public class Sample { public int value() { return 42; } }\n"
        self.backend_failure = False
        self.backend_version = "1.5.6\n"

    def backend(self, argv: list[str], log: Path, timeout: int, *, env: dict[str, str] | None = None) -> None:
        self.calls.append(argv)
        self.assertIsNotNone(env)
        self.assertTrue(Path(env["JADX_CONFIG_DIR"]).is_dir())
        self.assertNotIn("JADX_DISABLE_ALL_SECURITY_FLAGS", env)
        if "--version" in argv:
            log.write_text(self.backend_version)
            return
        self.assertEqual(timeout, 300)
        if self.backend_failure:
            raise MobileError("backend exited with status 3")
        code = Path(argv[-1])
        self.code_inputs = [(p.name, p.read_bytes()) for p in sorted(code.rglob("*")) if p.is_file()]
        log.write_text(self.diagnostics)
        if self.generated is not None:
            output = Path(argv[argv.index("--output-dir") + 1]) / "sources" / "example" / "Sample.java"
            output.parent.mkdir(parents=True)
            output.write_text(self.generated)

    def run_android(self, source: Path | None = None, *, limits: Limits | None = None) -> dict:
        with patch.object(android, "run_tool", side_effect=self.backend):
            return android.decompile_android(source or self.source, self.output, jadx="test-jadx", limits=limits or Limits())

    def test_dex_has_real_backend_contract_and_source_report(self) -> None:
        report = self.run_android()
        self.assertEqual(report["java_sources"], ["sources/example/Sample.java"])
        self.assertEqual(report["java_source_count"], 1)
        self.assertEqual(report["dex_count"], 1)
        self.assertEqual(report["status"], "success")
        self.assertEqual(report["backend"], {"name": "jadx", "version": "1.5.6"})
        self.assertEqual(self.calls[1][1:3], ["--config", "none"])
        self.assertIn("--no-res", self.calls[1])
        self.assertEqual(self.code_inputs[0][1], self.source.read_bytes())

    def test_multidex_apk_passes_every_root_dex_together(self) -> None:
        self.source = self.root / "multiple.apk"
        with zipfile.ZipFile(self.source, "w") as archive:
            archive.writestr("classes.dex", b"dex\n035\0first")
            archive.writestr("classes2.dex", b"dex\n039\0second")
            archive.writestr("classes10.dex", b"dex\n039\0tenth")
            archive.writestr("assets/dynamic.dex", b"not loaded")
            archive.writestr("assets/build.jadx.kts", b"not executed")
            archive.writestr("AndroidManifest.xml", b"resource")
        report = self.run_android()
        self.assertEqual(report["dex_count"], 3)
        self.assertEqual([n for n, _ in self.code_inputs], ["classes.dex", "classes10.dex", "classes2.dex"])
        self.assertEqual(len(self.calls), 2)
        self.assertFalse((self.output / "resources").exists())

    def test_smali_directory_preserves_all_classes_and_filters_other_plugins(self) -> None:
        self.source = self.root / "smali"
        nested = self.source / "example"
        nested.mkdir(parents=True)
        for name in ["Outer.smali", "Outer$Nested.smali", "Peer.SMALI"]:
            (nested / name).write_text(f".class public Lexample/{name.split('.')[0]};\n.super Ljava/lang/Object;\n")
        (nested / "execute.jadx.kts").write_text("not run")
        (nested / "unrelated.dex").write_bytes(b"not loaded")
        report = self.run_android()
        self.assertEqual(report["smali_count"], 3)
        self.assertEqual(len(self.code_inputs), 3)
        self.assertTrue(all(name.endswith(".smali") for name, _ in self.code_inputs))
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(report["input_kind"], "smali-directory")

    def test_single_smali_and_option_like_filename(self) -> None:
        self.source = self.root / "--output-dir $never; input.smali"
        self.source.write_text(".class public Lexample/Sample;\n.super Ljava/lang/Object;\n")
        report = self.run_android()
        self.assertEqual(report["smali_count"], 1)
        self.assertEqual(self.code_inputs[0][0], "input.smali")
        self.assertNotIn(str(self.source), self.calls[1])
        self.assertTrue(any("Single smali" in item for item in report["limitations"]))

    def test_backend_failure_is_not_success(self) -> None:
        self.backend_failure = True
        with self.assertRaisesRegex(MobileError, "status 3"):
            self.run_android()

    def test_logged_assembly_failure_even_with_zero_status_is_not_success(self) -> None:
        self.diagnostics = "INFO - progress\rERROR - Failed to assemble smali file: Bad.smali\n"
        with self.assertRaisesRegex(MobileError, "reported input"):
            self.run_android()

    def test_plugin_load_warning_even_with_zero_status_is_not_success(self) -> None:
        self.diagnostics = "WARN - Failed to load code for plugin: smali-input\n"
        with self.assertRaisesRegex(MobileError, "reported input"):
            self.run_android()

    def test_generated_failure_comment_is_not_success(self) -> None:
        self.generated = "class Sample {\n    /* JADX ERROR: Method load error */\n}\n"
        with self.assertRaisesRegex(MobileError, "incomplete Java"):
            self.run_android()

    def test_failure_comment_text_inside_java_string_is_not_a_diagnostic(self) -> None:
        self.generated = 'class Sample { String text = "/* JADX ERROR: example */"; }\n'
        self.assertEqual(self.run_android()["java_source_count"], 1)

    def test_duplicate_class_omission_is_not_success(self) -> None:
        self.diagnostics = "WARN - Found duplicated class: fixture.Peer, count: 2\n"
        with self.assertRaisesRegex(MobileError, "reported input"):
            self.run_android()

    def test_unreadable_output_directory_is_not_silently_skipped(self) -> None:
        sources = self.root / "sources"
        sources.mkdir()

        def unreadable_walk(path, *, followlinks, onerror):
            onerror(PermissionError("fixture denied"))
            return iter(())

        with patch.object(android.os, "walk", side_effect=unreadable_walk):
            with self.assertRaisesRegex(MobileError, "fixture denied"):
                android._check_output(sources, Limits())

    def test_missing_and_empty_java_output_is_not_success(self) -> None:
        self.generated = None
        with self.assertRaisesRegex(MobileError, "no Java sources"):
            self.run_android()
        self.output = self.root / "second-output"
        self.generated = ""
        with self.assertRaisesRegex(MobileError, "empty Java"):
            self.run_android()

    def test_invalid_dex_does_not_reach_decompiler(self) -> None:
        self.source.write_bytes(b"PK\x03\x04fake ZIP disguised as dex")
        with self.assertRaisesRegex(MobileError, "Invalid DEX"):
            self.run_android()
        self.assertEqual(len(self.calls), 1)

    def test_apk_without_code_fails(self) -> None:
        self.source = self.root / "native-only.apk"
        with zipfile.ZipFile(self.source, "w") as archive:
            archive.writestr("lib/arm64-v8a/native.so", b"native")
        with self.assertRaisesRegex(MobileError, "no root classes"):
            self.run_android()

    def test_unsafe_apk_is_rejected_before_decompiler(self) -> None:
        self.source = self.root / "unsafe.apk"
        with zipfile.ZipFile(self.source, "w") as archive:
            archive.writestr("classes.dex", b"dex\n035\0")
            archive.writestr("../outside", b"unsafe")
        with self.assertRaises(MobileError):
            self.run_android()
        self.assertEqual(len(self.calls), 1)
        self.assertFalse((self.root / "outside").exists())

    def test_smali_directory_without_smali_fails(self) -> None:
        self.source = self.root / "empty-smali"
        self.source.mkdir()
        with self.assertRaisesRegex(MobileError, "no .smali files"):
            self.run_android()

    def test_unsupported_backend_version_is_actionable(self) -> None:
        self.backend_version = "1.4.7\n"
        with self.assertRaisesRegex(MobileError, "1.5.6 or newer"):
            self.run_android()

    def test_input_and_output_limits(self) -> None:
        with self.assertRaisesRegex(MobileError, "byte limit"):
            self.run_android(limits=Limits(max_bytes=8))
        self.output = self.root / "another-output"
        self.generated = "class Sample {}\n" * 100
        with self.assertRaisesRegex(MobileError, "output exceeds"):
            self.run_android(limits=Limits(max_bytes=256))

    @unittest.skipIf(os.name == "nt", "symlink permissions vary on Windows")
    def test_smali_symlink_is_rejected(self) -> None:
        directory = self.root / "smali"
        directory.mkdir()
        (directory / "linked.smali").symlink_to(self.source)
        with self.assertRaises(MobileError):
            self.run_android(directory)

    def test_windows_batch_launcher_uses_java_without_a_shell(self) -> None:
        distribution = self.root / "jadx distribution"
        (distribution / "lib").mkdir(parents=True)
        jar = distribution / "lib" / "jadx-1.5.6-all.jar"
        jar.write_bytes(b"placeholder")
        with patch.object(android.shutil, "which", return_value="java-executable"), patch.dict(os.environ, {"JAVA_HOME": ""}):
            command = android._launcher(str(distribution / "bin" / "jadx.bat"))
        self.assertEqual(command, ["java-executable", "-cp", str(jar.resolve()), "jadx.cli.JadxCLI"])

    def test_batch_launcher_without_distribution_is_actionable(self) -> None:
        with self.assertRaisesRegex(MobileError, "lib/jadx"):
            android._launcher(str(self.root / "jadx.bat"))

    def test_bare_launcher_found_as_batch_in_path_uses_java(self) -> None:
        distribution = self.root / "path-distribution"
        (distribution / "lib").mkdir(parents=True)
        jar = distribution / "lib" / "jadx-1.5.6-all.jar"
        jar.write_bytes(b"placeholder")
        launcher = distribution / "bin" / "jadx.bat"

        def locate(name: str) -> str:
            return str(launcher) if name == "jadx" else "java-executable"

        with patch.object(android.shutil, "which", side_effect=locate), patch.dict(os.environ, {"JAVA_HOME": ""}):
            command = android._launcher("jadx")
        self.assertEqual(command, ["java-executable", "-cp", str(jar.resolve()), "jadx.cli.JadxCLI"])


if __name__ == "__main__":
    unittest.main()
