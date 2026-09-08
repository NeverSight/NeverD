"""The independent Android acceptance gate must reject false completeness."""
from __future__ import annotations

import copy
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "test_mobile_android_internal.py"
SPEC = importlib.util.spec_from_file_location("neverd_android_internal_acceptance", SCRIPT)
runner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = runner
SPEC.loader.exec_module(runner)


class AndroidInternalCoverageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name)
        self.source = self.output / "sources/fixture/Sample.java"
        self.source.parent.mkdir(parents=True)
        self.source.write_text("package fixture; public abstract class Sample { public Sample() {} "
                               "public int value(int x) { return x; } public static native long nativeValue(long x); "
                               "public abstract int abstractValue(int x); }\n")
        (self.output / "metadata").mkdir()
        owner = "Lfixture/Sample;"
        declarations = [("<init>", "()V", "body"), ("value", "(I)I", "body"),
                        ("nativeValue", "(J)J", "declaration"), ("abstractValue", "(I)I", "declaration")]
        self.expected = {owner + "->" + name + prototype: kind for name, prototype, kind in declarations}
        self.classes, self.inputs = {owner}, {owner: "classes.dex"}
        rows = [{"identity": owner + "->" + name + prototype, "class": owner, "name": name, "prototype": prototype,
                 "input": "classes.dex", "instruction_count": 2 if kind == "body" else 0,
                 "status": "recovered" if kind == "body" else "declaration-only",
                 **({"reason": "original abstract/native declaration has no Dalvik body"} if kind != "body" else {})}
                for name, prototype, kind in declarations]
        self.coverage = {"schema_version": 1, "status": "recovered", "class_count": 1, "method_count": 4,
                         "recovered_method_count": 2, "declaration_only_method_count": 2,
                         "unrecovered_method_count": 0, "methods": rows}
        self.report = {"status": "success", "platform": "android",
                       "backend": {"name": "neverd", "version": "1", "execution": "builtin"},
                       "android_method_recovery": self.coverage,
                       "java_source_count": 1, "java_sources": ["sources/fixture/Sample.java"]}

    def check(self, report=None, *, metadata=None):
        report = self.report if report is None else report
        # Keep complete source artifacts and matching JSON in the negative
        # cases, so a later missing-file check cannot conceal a weak gate.
        self.assertTrue(self.source.is_file())
        (self.output / "metadata/android-methods.json").write_text(json.dumps(
            report["android_method_recovery"] if metadata is None else metadata))
        return runner.validate_coverage(report, self.output, self.classes, self.expected, self.inputs)

    def test_complete_body_and_declaration_inventory_passes(self):
        self.assertEqual(self.check(), {"class_count": 1, "method_count": 4, "recovered_method_count": 2,
                                        "declaration_only_method_count": 2, "unrecovered_method_count": 0})

    def test_omitted_body_cannot_hide_in_matching_report_and_metadata(self):
        self.coverage["methods"].pop(0)
        with self.assertRaisesRegex(RuntimeError, "Missing methods.*init"):
            self.check()

    def test_real_body_cannot_be_reclassified_as_a_declaration(self):
        self.coverage["methods"][1]["status"] = "declaration-only"
        self.coverage["methods"][1]["instruction_count"] = 0
        with self.assertRaisesRegex(RuntimeError, "classification changed.*value"):
            self.check()

    def test_totals_must_match_original_compiler_inventory(self):
        self.coverage["method_count"] = 3
        with self.assertRaisesRegex(RuntimeError, "Aggregate method counts"):
            self.check()

    def test_external_backend_cannot_claim_builtin_acceptance(self):
        self.report["backend"] = {"name": "other", "execution": "external"}
        with self.assertRaisesRegex(RuntimeError, "built-in engine"):
            self.check()

    def test_duplicate_identity_does_not_compensate_for_missing_methods(self):
        self.coverage["methods"].append(copy.deepcopy(self.coverage["methods"][0]))
        with self.assertRaisesRegex(RuntimeError, "Duplicate/unexpected method"):
            self.check()

    def test_recovered_body_must_have_instructions(self):
        self.coverage["methods"][0]["instruction_count"] = 0
        with self.assertRaisesRegex(RuntimeError, "classification changed.*init"):
            self.check()

    def test_multidex_input_ownership_must_match_original_partition(self):
        self.coverage["methods"][0]["input"] = "classes2.dex"
        with self.assertRaisesRegex(RuntimeError, "Incorrect input ownership"):
            self.check()

    def test_malformed_identity_is_an_explicit_gate_error(self):
        self.coverage["methods"][0] = None
        with self.assertRaisesRegex(RuntimeError, "Malformed method identity"):
            self.check()

    def test_native_and_abstract_declarations_cannot_claim_generated_bodies(self):
        for index in (2, 3):
            with self.subTest(kind=self.coverage["methods"][index]["name"]):
                report = copy.deepcopy(self.report)
                row = report["android_method_recovery"]["methods"][index]
                row.update(status="recovered", instruction_count=1)
                with self.assertRaisesRegex(RuntimeError, "classification changed"):
                    self.check(report)

    def test_standalone_metadata_and_source_inventory_cannot_drift(self):
        metadata = copy.deepcopy(self.coverage)
        metadata["method_count"] = 0
        with self.assertRaisesRegex(RuntimeError, "Standalone method coverage"):
            self.check(metadata=metadata)
        self.report["java_source_count"] = 0
        with self.assertRaisesRegex(RuntimeError, "artifact inventory"):
            self.check()

    def test_behavior_output_must_include_all_distinct_keys(self):
        expected = runner.expected_keys("single")
        lines = [key + "=0" for key in sorted(expected)]
        self.assertEqual(set(runner.results("\n".join(lines), "single")), expected)
        with self.assertRaisesRegex(RuntimeError, "Behavior inventory changed"):
            runner.results("\n".join(lines[:-1]), "single")
        with self.assertRaisesRegex(RuntimeError, "duplicate behavior"):
            runner.results("\n".join(lines + [lines[0]]), "single")


class AndroidInternalPortabilityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="android runner paths ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_windows_batch_launchers_use_java_argument_vectors(self):
        tools = self.root / "Android SDK/build-tools/35.0.0"
        (tools / "lib").mkdir(parents=True)
        jar = tools / "lib/d8.jar"
        jar.write_bytes(b"test distribution layout")
        java = str(self.root / "JDK 21/bin/java.exe")
        expected = [java, "-cp", str(jar), "com.android.tools.r8.D8"]
        for launcher in (tools / "d8.bat", tools / "d8.cmd", jar):
            self.assertEqual(runner.d8_command(launcher, java), expected)
        self.assertEqual(runner.d8_command(tools / "d8", java), [str(tools / "d8")])

    def test_missing_batch_distribution_is_not_sent_to_a_shell(self):
        with self.assertRaisesRegex(RuntimeError, "lib/d8.jar"):
            runner.d8_command(self.root / "d8.bat", "java.exe")

    def test_explicit_tool_configuration_is_not_silently_replaced(self):
        explicit, environment = self.root / "explicit d8", self.root / "environment d8"
        explicit.write_text("fixture"); environment.write_text("fixture")
        with patch.dict(os.environ, {"NEVERD_D8": str(environment)}):
            self.assertEqual(runner.choose_d8(explicit), explicit.resolve())
            self.assertEqual(runner.choose_d8(None), environment.resolve())
            with self.assertRaisesRegex(RuntimeError, "configured D8"):
                runner.choose_d8(self.root / "missing")

    def test_sdk_discovery_prefers_highest_stable_build_tools(self):
        sdk = self.root / "SDK with spaces"
        name = "d8.bat" if os.name == "nt" else "d8"
        for version in ("35.0.0", "35.0.1", "36.0.0", "37.0.0-rc2"):
            path = sdk / "build-tools" / version / name
            path.parent.mkdir(parents=True); path.write_text("fixture")
        with patch.dict(os.environ, {"ANDROID_HOME": str(sdk)}, clear=True), \
                patch.object(runner.shutil, "which", return_value=None), \
                patch.object(runner.Path, "home", return_value=self.root / "home"):
            self.assertEqual(runner.choose_d8(None), (sdk / "build-tools/36.0.0" / name).resolve())

    def test_explicit_jdk_requires_both_compiler_and_runtime(self):
        jdk = self.root / "JDK 21"
        (jdk / "bin").mkdir(parents=True)
        suffix = ".exe" if os.name == "nt" else ""
        (jdk / "bin" / ("java" + suffix)).write_text("fixture")
        with self.assertRaisesRegex(RuntimeError, "JDK is required"):
            runner.choose_jdk(jdk)
        (jdk / "bin" / ("javac" + suffix)).write_text("fixture")
        self.assertEqual(runner.choose_jdk(jdk), jdk.resolve())


if __name__ == "__main__": unittest.main()
