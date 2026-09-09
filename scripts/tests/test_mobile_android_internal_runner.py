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

    def check(self, report=None, *, metadata=None, projection=None):
        report = self.report if report is None else report
        # Keep complete source artifacts and matching JSON in the negative
        # cases, so a later missing-file check cannot conceal a weak gate.
        self.assertTrue(self.source.is_file())
        (self.output / "metadata/android-methods.json").write_text(json.dumps(
            report["android_method_recovery"] if metadata is None else metadata))
        return runner.validate_coverage(report, self.output, self.classes, self.expected, self.inputs,
                                        expected_projection=projection)

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

    def projection(self):
        row = self.coverage["methods"][1]
        row.update(status="source-projected", projection_kind="named-method-local",
                   reason="Lexical projection requires independent class identity validation")
        self.coverage.update(status="partial", projected_method_count=1, recovered_method_count=1)
        return {row["identity"]}

    def test_only_explicit_independent_projection_inventory_accepts_partial(self):
        projection = self.projection()
        counts = self.check(projection=projection)
        self.assertEqual(counts["method_count"], 4)
        self.assertEqual(counts["projected_method_count"], 1)
        self.assertEqual(counts["recovered_method_count"], 1)
        with self.assertRaisesRegex(RuntimeError, "Standalone method coverage"):
            self.check()

    def test_projected_original_cannot_be_reclassified_as_recovered(self):
        projection = self.projection()
        self.coverage["methods"][1]["status"] = "recovered"
        with self.assertRaisesRegex(RuntimeError, "classification changed"):
            self.check(projection=projection)

    def test_matching_totals_cannot_move_projection_to_a_different_method(self):
        projection = self.projection()
        self.coverage["methods"][0].update(status="source-projected", projection_kind="named-method-local", reason="scope")
        self.coverage["methods"][1].update(status="recovered")
        with self.assertRaisesRegex(RuntimeError, "classification changed"):
            self.check(projection=projection)

    def test_projection_requires_partial_summary_exact_count_and_explanation(self):
        projection = self.projection()
        for mutation in ("summary", "count", "reason", "kind", "omitted"):
            report = copy.deepcopy(self.report)
            coverage = report["android_method_recovery"]
            if mutation == "summary": coverage["status"] = "recovered"
            elif mutation == "count": coverage["projected_method_count"] = 0
            elif mutation == "omitted": coverage["methods"].pop(1)
            else: coverage["methods"][1].pop("reason" if mutation == "reason" else "projection_kind")
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, "Standalone|Aggregate|precise scope|Missing methods"):
                self.check(report, projection=projection)

    def test_relabelled_ordinary_summary_cannot_hide_projection_metadata(self):
        for key in ("class_source_bindings", "generated_source_helpers"):
            report = copy.deepcopy(self.report)
            report["android_method_recovery"][key] = [{"class": "Lfixture/Sample$1Worker;"}]
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "conceal local source projection"):
                self.check(report)

    def test_projection_cannot_consume_a_declaration_or_an_unknown_identity(self):
        for projection in (set(), {"missing"}, {"Lfixture/Sample;->nativeValue(J)J"}):
            with self.subTest(projection=projection), self.assertRaisesRegex(RuntimeError, "independent projection inventory"):
                self.check(projection=projection)

    def test_local_behavior_matrix_requires_every_scope_counter_and_boundary_key(self):
        keys = runner.expected_keys("local")
        self.assertEqual(len(keys), 217)
        for key in ("reflection:first-long", "first:0:0", "first-count:6:6", "wide:6", "final-second", "constant-reflection"):
            self.assertIn(key, keys)
            text = "\n".join(name + "=0" for name in sorted(keys - {key}))
            with self.subTest(missing=key), self.assertRaisesRegex(RuntimeError, "Behavior inventory changed"):
                runner.results(text, "local")

    def test_same_signature_worker_swap_and_lost_constructor_effect_cannot_pass_behavior(self):
        baseline = {key: number for number, key in enumerate(sorted(runner.expected_keys("local")))}
        runner.compare_local_behavior(baseline, dict(baseline))
        changed = dict(baseline)
        changed["first:0:0"], changed["second:0:0"] = changed["second:0:0"], changed["first:0:0"]
        with self.assertRaisesRegex(RuntimeError, "changed behavior.*first:0:0"):
            runner.compare_local_behavior(baseline, changed)
        changed = dict(baseline, **{"first-count:6:6": -1})
        with self.assertRaisesRegex(RuntimeError, "changed behavior.*first-count:6:6"):
            runner.compare_local_behavior(baseline, changed)
        changed = dict(baseline)
        del changed["constant-reflection"]
        with self.assertRaisesRegex(RuntimeError, "complete independent key"):
            runner.compare_local_behavior(baseline, changed)


class LocalCompilerIsolationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.verify = runner.Verify(self.root, self.root / "JDK", self.root / "d8", 30)

    def test_recompiled_java8_uses_only_sources_and_a_new_empty_classpath(self):
        source = self.root / "recovered/LocalClassBehavior.java"
        with patch.object(self.verify, "run") as command:
            self.verify.compile_local([source], self.root / "classes")
        argv = list(map(str, command.call_args.args[0]))
        self.assertEqual(argv[argv.index("--release") + 1], "8")
        self.assertIn("-proc:none", argv)
        self.assertIn("-implicit:none", argv)
        empty = str(self.root / "local-empty-classpath")
        for flag in ("-classpath", "-sourcepath", "-processorpath"):
            self.assertEqual(argv[argv.index(flag) + 1], empty)
        self.assertEqual(argv[-1], str(source))
        self.assertNotIn(str(self.root / "original-local/classes"), argv)

    def test_harness_classpath_does_not_enable_implicit_original_source_compilation(self):
        compiled = self.root / "compiled-generated-only"
        with patch.object(self.verify, "run") as command:
            self.verify.compile_local([self.root / "Harness.java"], self.root / "harness", classpath=compiled)
        argv = list(map(str, command.call_args.args[0]))
        self.assertEqual(argv[argv.index("-classpath") + 1], str(compiled))
        self.assertEqual(argv[argv.index("-sourcepath") + 1], str(self.root / "local-empty-classpath"))

    def test_nonempty_isolation_directory_is_a_failure_before_compilation(self):
        empty = self.root / "local-empty-classpath"
        empty.mkdir()
        (empty / "Leaked.class").write_bytes(b"fixture")
        with patch.object(self.verify, "run") as command, self.assertRaisesRegex(RuntimeError, "not empty"):
            self.verify.compile_local([], self.root / "classes")
        command.assert_not_called()

    def test_local_partitions_are_complete_and_cross_dex_ownership_is_distinct(self):
        from scripts.tests.test_mobile_android_class_identity import inventory
        original = inventory()
        partitions = runner.local_partitions(original)
        self.assertEqual(set(partitions), set(runner.LOCAL_CASES))
        self.assertEqual([len(partitions[name]) for name in runner.LOCAL_CASES], [1, 2, 2])
        for groups in partitions.values():
            flat = [owner for group in groups for owner in group]
            self.assertEqual(set(flat), set(original))
            self.assertEqual(len(flat), len(original))
            self.assertTrue(all(groups))
        self.assertEqual(partitions[runner.LOCAL_CASES[1]][0], [runner.LOCAL_OWNER])
        wide = runner.local_roles(original)["first-long"]
        self.assertEqual(partitions[runner.LOCAL_CASES[2]][0], [wide])

    def test_original_preparation_failure_marks_all_three_required_cases(self):
        with patch.object(self.verify, "run", side_effect=RuntimeError("compiler failed")):
            passed, failures = self.verify.local_cases(self.root / "neverd")
        self.assertEqual(passed, [])
        self.assertEqual({row["case"] for row in failures}, set(runner.LOCAL_CASES))
        self.assertEqual(len(failures), 3)


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
