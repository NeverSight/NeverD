"""CI-only guards: commands are simulated, never executed by these tests.

The class/DEX header fixtures exercise evidence validation, not compiler or
decoder correctness. Actual javac and D8 runs belong to the real-app CI jobs.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from scripts.mobile_real_apps_android_recompile import (
    ATTEMPT_FILE, INJECTION_ENV, attempt_java_recompile,
)


def dex_header_fixture():
    data = bytearray(112)
    data[:8] = b"dex\n035\0"
    struct.pack_into("<III", data, 32, 112, 112, 0x12345678)
    return data


def record(path, root):
    data = path.read_bytes()
    return {"path": path.relative_to(root).as_posix(), "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}


class CompilationContext:
    def __init__(self, root):
        root = root.resolve()
        self.work = root / "evidence space"
        self.work.mkdir()
        self.output = self.work / "recovered"
        self.output.mkdir()
        self.apk = self.work / "original.apk"
        self.apk.write_bytes(b"self-authored original APK identity fixture, never a compiler input")
        self.timeout = 90
        self.variant = {"id": "owned-android", "profile": "official-release"}
        self.app = {"source_commit": "a" * 40, "android": {"java": "21", "compile_sdk": 35,
                    "build_tools": "35.0.0", "package_name": "owned.application"}}
        self.result = {"case_id": "owned-android", "consumer_commit": "b" * 40,
                       "manifest_sha256": "c" * 64, "commands": []}
        self.commands = []
        self.sdk, self.jdk = root / "sdk", root / "jdk"
        suffix = ".exe" if os.name == "nt" else ""
        files = {
            self.sdk / "platforms/android-35/android.jar": b"simulated SDK declaration archive",
            self.sdk / "platforms/android-35/source.properties": b"AndroidVersion.ApiLevel=35\nPkg.Revision=2\n",
            self.sdk / "build-tools/35.0.0/source.properties": b"Pkg.Revision=35.0.0\n",
            self.sdk / ("build-tools/35.0.0/aapt2" + suffix): b"simulated aapt2 executable",
            self.sdk / "build-tools/35.0.0/lib/d8.jar": b"simulated SDK D8 implementation",
            self.jdk / ("bin/java" + suffix): b"simulated java executable",
            self.jdk / ("bin/javac" + suffix): b"simulated javac executable",
            self.jdk / "release": b'JAVA_VERSION="21.0.6"\nIMPLEMENTOR="Owned fixture"\n',
            self.jdk / "lib/modules": b"simulated JDK runtime modules",
        }
        for path, data in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.report = {"schema_version": 1, "platform": "android", "input_kind": "apk", "status": "success",
                       "backend": {"name": "neverd", "execution": "builtin"},
                       "java_sources": ["sources/owned/Example.java", "sources/owned/Peer.java"],
                       "java_source_count": 2}
        self.sources = []
        for name in self.report["java_sources"]:
            path = self.output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("package owned; public class " + path.stem + " {}\n", encoding="utf-8")
            self.sources.append(record(path, self.output))
        (self.output / "report.json").write_text(json.dumps(self.report), encoding="utf-8")
        self.javac_exit = self.d8_exit = 0
        self.d8_stderr = ""
        self.badging = "package: name='owned.application' versionCode='1'\nsdkVersion:'24'\n"
        self.class_data = b"\xca\xfe\xba\xbe\x00\x00\x00\x34"
        self.dex_data = dex_header_fixture()
        self.exception_command = None
        self.after_command = None

    def write_json(self, name, value):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")

    def command(self, name, argv, **kwargs):
        self.commands.append((name, list(argv), kwargs))
        command_id = f"{len(self.commands):04d}-{name}"
        stdout, stderr, code = "simulated tool version\n", "", 0
        if name == "android-recompile-apk-manifest":
            stdout = self.badging
        elif name == "android-recompile-javac":
            code = self.javac_exit
            stdout, stderr = "", "javac fixture diagnostic\n" if code else ""
            directory = Path(argv[argv.index("-d") + 1])
            (directory / "owned").mkdir()
            for name_part in ("Example", "Peer"):
                (directory / "owned" / (name_part + ".class")).write_bytes(self.class_data)
        elif name == "android-recompile-d8":
            code, stdout, stderr = self.d8_exit, "", self.d8_stderr
            destination = Path(argv[argv.index("--output") + 1])
            with zipfile.ZipFile(destination, "w") as archive:
                archive.writestr("classes.dex", self.dex_data)
        elif not name.endswith("-version"):
            raise AssertionError("Unexpected tool command: " + name)
        self.write_json("commands/" + command_id + ".json", {"argv": argv, "returncode": code})
        (self.work / "commands" / (command_id + ".stdout")).write_text(stdout, encoding="utf-8")
        (self.work / "commands" / (command_id + ".stderr")).write_text(stderr, encoding="utf-8")
        self.result["commands"].append({"id": command_id, "name": name, "exit_code": code})
        if self.after_command:
            self.after_command(name)
        if name == self.exception_command:
            raise TimeoutError("simulated bounded command timeout")
        return subprocess.CompletedProcess(argv, code, stdout, stderr)

    def attempt(self, **overrides):
        arguments = {"apk": self.apk, "output": self.output, "report": self.report,
                     "sources": self.sources, "recovery_qualified": True}
        arguments.update(overrides)
        (self.output / "report.json").write_text(json.dumps(self.report), encoding="utf-8")
        with patch.dict(os.environ, {"ANDROID_SDK_ROOT": str(self.sdk), "JAVA_HOME": str(self.jdk)}):
            return attempt_java_recompile(self, **arguments)


class GeneratedJavaAttemptTests(unittest.TestCase):
    def test_all_sources_compile_with_only_android_declarations_and_new_class_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            # These original implementation-like artifacts are deliberately
            # present but must never be copied or used as classpath inputs.
            for name in ("original.class", "original.jar", "original.dex"):
                (ctx.output / name).write_bytes(b"must not be used")
            result = ctx.attempt()
            self.assertEqual(result["status"], "success", result)
            self.assertTrue(result["compile_complete"])
            self.assertEqual(result["kind"], "generated-java-compilation")
            self.assertEqual(result["schema_version"], 1)
            self.assertFalse(result["independent"])
            self.assertFalse(result["maturity_qualified"])
            self.assertFalse(result["apk_reconstructed"])
            self.assertFalse(result["art_behavior_verified"])
            self.assertEqual(result["source_count"], 2)
            self.assertEqual(result["original_code_compiler_inputs"], [])
            self.assertEqual(result["handwritten_stub_inputs"], [])
            self.assertEqual(result["additional_application_dependencies"], [])
            self.assertEqual({row["generated_path"] for row in result["sources"]}, set(ctx.report["java_sources"]))
            java = next(argv for name, argv, _ in ctx.commands if name == "android-recompile-javac")
            self.assertIn("-proc:none", java)
            self.assertIn("-implicit:none", java)
            self.assertNotIn("--release", java)
            for option in ("-classpath", "-sourcepath", "-processorpath"):
                directory = Path(java[java.index(option) + 1])
                self.assertEqual(directory, ctx.work / "java-compilation/empty")
                self.assertEqual(list(directory.iterdir()), [])
            self.assertEqual(Path(java[java.index("-bootclasspath") + 1]), ctx.work / "java-compilation/platform/android.jar")
            args = (ctx.work / result["javac_argument_file"]["path"]).read_text(encoding="utf-8")
            self.assertEqual(len(args.splitlines()), 2)
            self.assertTrue(all(line.startswith('"') and line.endswith('"') for line in args.splitlines()))
            self.assertTrue(all(Path(row["path"]).name in args for row in result["sources"]))
            d8 = next(argv for name, argv, _ in ctx.commands if name == "android-recompile-d8")
            self.assertEqual(d8[d8.index("--min-api") + 1], "24")
            self.assertEqual(d8[-1], str(ctx.work / "java-compilation/generated-classes.jar"))
            self.assertNotIn(str(ctx.apk), java + d8)
            self.assertIn("--release", d8)
            with zipfile.ZipFile(d8[-1]) as archive:
                self.assertEqual(archive.namelist(), ["owned/Example.class", "owned/Peer.class"])
            self.assertEqual(len(result["dex_outputs"]), 1)
            self.assertEqual(result["dex_outputs"][0]["validation"], "container-and-fixed-header-only")
            self.assertEqual(len(result["commands"]), len(ctx.commands))
            self.assertEqual(json.loads((ctx.work / ATTEMPT_FILE).read_text())["status"], "success")

    def test_environment_overwrites_inherited_compiler_injection_variables(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            with patch.dict(os.environ, {key: "untrusted-injected-option" for key in INJECTION_ENV}):
                result = ctx.attempt()
            self.assertEqual(result["status"], "success", result)
            for _, _, options in ctx.commands:
                for key in INJECTION_ENV:
                    self.assertEqual(options["env"][key], "")
                self.assertEqual(options["env"]["JAVA_HOME"], str(ctx.jdk.resolve()))
                self.assertEqual(options["cwd"], ctx.work / "java-compilation")
                self.assertEqual(options["timeout"], ctx.timeout)

    def test_partial_recovery_can_compile_without_upgrading_its_qualification(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            ctx.report["status"] = "partial"
            result = ctx.attempt(recovery_qualified=False)
            self.assertEqual(result["status"], "success", result)
            self.assertFalse(result["recovery_qualified"])
            self.assertFalse(result["maturity_qualified"])
            self.assertIn("complete original method coverage", result["remaining_obligations"])

    def test_source_profiles_use_actual_variant_package_and_debug_mode(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            ctx.variant["profile"] = "gradle-debug"
            ctx.app["android"]["source_build"] = {"profiles": {"gradle-debug": {"application_id": "owned.debug"}}}
            ctx.badging = "package: name='owned.debug' versionCode='1'\nsdkVersion:'26'\n"
            result = ctx.attempt()
            self.assertEqual(result["status"], "success", result)
            self.assertEqual(result["d8"]["mode"], "--debug")
            self.assertEqual(result["manifest"], {"package_name": "owned.debug", "min_sdk": 26})

    def test_inventory_must_include_every_java_even_outside_the_sources_subtree(self):
        for unlisted in ("sources/owned/Hidden.java", "Hidden.java"):
            with self.subTest(unlisted=unlisted), tempfile.TemporaryDirectory() as temporary:
                ctx = CompilationContext(Path(temporary))
                (ctx.output / unlisted).write_text("class Hidden {}", encoding="utf-8")
                result = ctx.attempt()
                self.assertEqual(result["status"], "failed")
                self.assertIn("Unlisted", result["reason"])
                self.assertFalse(ctx.commands)

    def test_missing_duplicate_changed_and_unsafe_source_rows_never_reach_javac(self):
        for mutation in ("missing", "duplicate", "changed", "parent-path", "newline", "bad-digest"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                ctx = CompilationContext(Path(temporary))
                sources = copy.deepcopy(ctx.sources)
                if mutation == "missing": sources.pop()
                elif mutation == "duplicate": sources.append(sources[0])
                elif mutation == "changed": (ctx.output / sources[0]["path"]).write_text("changed", encoding="utf-8")
                elif mutation == "parent-path": sources[0]["path"] = "sources/../Original.java"
                elif mutation == "newline": sources[0]["path"] = "sources/Bad\n-options.java"
                elif mutation == "bad-digest": sources[0]["sha256"] = "z" * 64
                result = ctx.attempt(sources=sources)
                self.assertEqual(result["status"], "failed")
                self.assertFalse(ctx.commands)

    def test_symlinked_source_tree_is_rejected_without_reading_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            original = Path.is_symlink
            with patch.object(Path, "is_symlink", lambda path: path == ctx.output / "sources/owned" or original(path)):
                result = ctx.attempt()
            self.assertEqual(result["status"], "failed")
            self.assertIn("symlink", result["reason"].lower())
            self.assertFalse(ctx.commands)

    def test_external_backend_or_bad_report_cannot_supply_compiler_sources(self):
        for mutation in ("external", "wrong-kind", "bool-schema"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                ctx = CompilationContext(Path(temporary))
                if mutation == "external": ctx.report["backend"]["execution"] = "external"
                elif mutation == "wrong-kind": ctx.report["input_kind"] = "dex"
                else: ctx.report["schema_version"] = True
                result = ctx.attempt()
                self.assertEqual(result["status"], "failed")
                self.assertFalse(ctx.commands)

    def test_report_object_must_match_the_published_report_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            changed = copy.deepcopy(ctx.report)
            changed["status"] = "partial"
            result = ctx.attempt(report=changed)
            self.assertEqual(result["status"], "failed")
            self.assertIn("report changed", result["reason"])
            self.assertFalse(ctx.commands)

    def test_missing_or_mismatched_sdk_and_jdk_fail_without_dependency_substitutes(self):
        for mutation in ("missing-platform", "sdk-version", "tools-version", "jdk-version"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                ctx = CompilationContext(Path(temporary))
                if mutation == "missing-platform": (ctx.sdk / "platforms/android-35/android.jar").unlink()
                elif mutation == "sdk-version": (ctx.sdk / "platforms/android-35/source.properties").write_text("AndroidVersion.ApiLevel=36\n")
                elif mutation == "tools-version": (ctx.sdk / "build-tools/35.0.0/source.properties").write_text("Pkg.Revision=36.0.0\n")
                else: (ctx.jdk / "release").write_text('JAVA_VERSION="17.0.13"\n')
                result = ctx.attempt()
                self.assertEqual(result["status"], "failed")
                self.assertFalse(ctx.commands)
                self.assertEqual(result["additional_application_dependencies"], [])

    def test_apk_manifest_must_have_one_exact_package_and_minimum_sdk(self):
        for text in ("package: name='other.app'\nsdkVersion:'24'\n",
                     "package: name='owned.application'\n",
                     "package: name='owned.application'\nsdkVersion:'24'\nsdkVersion:'25'\n",
                     "package: name='owned.application'\nsdkVersion:'VanillaIceCream'\n"):
            with self.subTest(text=text), tempfile.TemporaryDirectory() as temporary:
                ctx = CompilationContext(Path(temporary))
                ctx.badging = text
                result = ctx.attempt()
                self.assertEqual(result["status"], "failed")
                self.assertEqual(result["phase"], "manifest")
                self.assertFalse(any(name == "android-recompile-javac" for name, _, _ in ctx.commands))

    def test_javac_failure_preserves_partial_outputs_and_does_not_run_d8_compile(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            ctx.javac_exit = 1
            result = ctx.attempt()
            self.assertEqual(result["status"], "failed")
            self.assertEqual(result["javac"], {"status": "failed", "returncode": 1})
            self.assertTrue((ctx.work / "java-compilation/classes/owned/Example.class").is_file())
            self.assertEqual(len(result["observed_compiler_outputs"]), 2)
            self.assertFalse(any(name == "android-recompile-d8" for name, _, _ in ctx.commands))
            self.assertTrue(any("javac" in item.name for item in (ctx.work / "commands").glob("*.stderr")))

    def test_d8_nonzero_warnings_and_invalid_output_never_count_as_compilation_success(self):
        for mutation in ("nonzero", "warning", "bad-header"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                ctx = CompilationContext(Path(temporary))
                if mutation == "nonzero": ctx.d8_exit = 1
                elif mutation == "warning": ctx.d8_stderr = "Warning: Missing class owned.Dependency\n"
                else: ctx.dex_data = b"invalid"
                result = ctx.attempt()
                self.assertEqual(result["status"], "failed")
                self.assertFalse(result["compile_complete"])
                self.assertTrue((ctx.work / "java-compilation/generated-classes.jar").is_file())
                self.assertTrue((ctx.work / "java-compilation/generated-dex.zip").is_file())

    def test_invalid_class_output_is_not_passed_to_d8(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            ctx.class_data = b"not a Java 8 class file"
            result = ctx.attempt()
            self.assertEqual(result["status"], "failed")
            self.assertIn("class output", result["reason"])
            self.assertFalse(any(name == "android-recompile-d8" for name, _, _ in ctx.commands))

    def test_timeout_retains_command_and_attempt_failure_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            ctx.exception_command = "android-recompile-javac"
            result = ctx.attempt()
            self.assertEqual(result["status"], "failed")
            self.assertIn("timeout", result["reason"])
            self.assertTrue(result["commands"][-1].endswith("android-recompile-javac"))
            self.assertTrue((ctx.work / ATTEMPT_FILE).is_file())

    def test_input_mutation_during_tools_invalidates_otherwise_successful_compilation(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            def mutation(name):
                if name == "android-recompile-javac":
                    (ctx.output / ctx.sources[0]["path"]).write_text("changed during execution", encoding="utf-8")
            ctx.after_command = mutation
            result = ctx.attempt()
            self.assertEqual(result["status"], "failed")
            self.assertFalse(result["compile_complete"])
            self.assertIn("input integrity check failed", result["reason"])

    def test_stale_compilation_tree_is_never_reused_or_deleted(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = CompilationContext(Path(temporary))
            directory = ctx.work / "java-compilation"
            directory.mkdir()
            old = directory / "old.class"
            old.write_bytes(b"stale implementation")
            result = ctx.attempt()
            self.assertEqual(result["status"], "failed")
            self.assertEqual(old.read_bytes(), b"stale implementation")
            self.assertFalse(ctx.commands)


if __name__ == "__main__":
    unittest.main()
