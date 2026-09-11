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

    def check(self, report=None, *, metadata=None, projection=None, generic_helpers=None):
        report = self.report if report is None else report
        # Keep complete source artifacts and matching JSON in the negative
        # cases, so a later missing-file check cannot conceal a weak gate.
        self.assertTrue(self.source.is_file())
        (self.output / "metadata/android-methods.json").write_text(json.dumps(
            report["android_method_recovery"] if metadata is None else metadata))
        return runner.validate_coverage(report, self.output, self.classes, self.expected, self.inputs,
                                        expected_projection=projection, expected_generic_helpers=generic_helpers)

    def test_generic_helpers_require_an_explicit_exact_independent_inventory(self):
        helpers = [{"class": "Lfixture/Sample;", "name": "__neverdThrow", "static": True,
                    "prototype": runner.class_identity.THROW_HELPER_PROTOTYPE,
                    "source_unit": "fixture/Sample.java", "kind": "throw-helper"}]
        self.coverage["generated_source_helpers"] = copy.deepcopy(helpers)
        self.assertEqual(self.check(generic_helpers=helpers)["method_count"], 4)
        with self.assertRaisesRegex(RuntimeError, "Ordinary recovery"):
            self.check()
        for mutation in ("omitted", "duplicate", "wrong-static", "numeric-static", "wrong-owner", "wrong-source", "extra"):
            report = copy.deepcopy(self.report)
            actual = report["android_method_recovery"]["generated_source_helpers"]
            if mutation == "omitted": actual.clear()
            elif mutation == "duplicate": actual.append(copy.deepcopy(actual[0]))
            elif mutation == "wrong-static": actual[0]["static"] = False
            elif mutation == "numeric-static": actual[0]["static"] = 1
            elif mutation == "wrong-owner": actual[0]["class"] = "Lfixture/Other;"
            elif mutation == "wrong-source": actual[0]["source_unit"] = "sources/fixture/Sample.java"
            else: actual[0]["unverified_extra"] = True
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, "Generic generated helper"):
                self.check(report, generic_helpers=helpers)

    def test_generic_helpers_do_not_allow_projection_or_inflate_original_counts(self):
        helpers = [{"class": "Lfixture/Sample;", "name": "__neverdThrow", "static": True,
                    "prototype": runner.class_identity.THROW_HELPER_PROTOTYPE,
                    "source_unit": "fixture/Sample.java", "kind": "throw-helper"}]
        self.coverage["generated_source_helpers"] = helpers
        for mutation in ("projection", "count", "missing-original"):
            report = copy.deepcopy(self.report)
            coverage = report["android_method_recovery"]
            if mutation == "projection": coverage["methods"][0]["projection_kind"] = "named-method-local"
            elif mutation == "count": coverage["method_count"] += 1
            else: coverage["methods"].pop(0)
            with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                self.check(report, generic_helpers=helpers)

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


def generic_facts():
    """Mock routing data, not a substitute for the actual javac/reflection fixture."""
    classes = {}
    for owner, declarations in runner.GENERIC_METHODS.items():
        methods = {}
        for member, signature in declarations.items():
            name, tail = member.split("(", 1)
            methods[owner + "->" + member] = {"name": name, "prototype": "(" + tail, "code": True,
                "access": 9 if owner == runner.GENERIC_OPS and name != "<init>" else 1,
                "signature": signature, **runner.deprecated_facts()}
        fields = {owner + "->" + name + ":" + descriptor:
                  {"name": name, "descriptor": descriptor, "access": 1, "constant_value": None,
                   "signature": signature, **runner.deprecated_facts()}
                  for name, (descriptor, signature) in (runner.GENERIC_FIELDS.items() if owner == runner.GENERIC_BOX else [])}
        classes[owner] = {"name": owner, "access": 0x31, "superclass": "Ljava/lang/Object;", "interfaces": [],
            "fields": fields, "methods": methods, "enclosing_method": None, "inner_class": None,
            "source_file": owner.rsplit("/", 1)[1][:-1] + ".java", "major": 52, "minor": 0,
            "signature": "<T:Ljava/lang/Object;>Ljava/lang/Object;" if owner == runner.GENERIC_BOX else None,
            "path": owner[1:-1] + ".class", "size": 4, "sha256": "a" * 64, **runner.deprecated_facts()}
    return classes


def generic_values():
    values = dict.fromkeys(runner.expected_keys("generic"), 1)
    values["array-exceptions"] = 7
    # Independent Java int overflow expectations for the seven fixture values.
    for i, value in enumerate((-2147483641, -293, 4, 7, 10, 307, -2147483644)):
        values["scalar:" + str(i)] = value
    return values


class GenericOracleTests(unittest.TestCase):
    def test_handwritten_original_contract_has_complete_generic_scopes(self):
        classes = generic_facts()
        runner.validate_generic_original(classes)
        self.assertEqual(sum(len(row["methods"]) for row in classes.values()), 13)
        self.assertEqual(len(runner.GENERIC_REFLECTION_KEYS), 22)
        self.assertEqual(len(runner.expected_keys("generic")), 103)
        runner.validate_generic_behavior(generic_values())
        for scope in ("class", "field", "method"):
            altered = copy.deepcopy(classes)
            box = altered[runner.GENERIC_BOX]
            target = box if scope == "class" else next(iter(box["fields" if scope == "field" else "methods"].values()))
            target["signature"] = None
            with self.subTest(scope=scope), self.assertRaisesRegex(RuntimeError, "Signature"):
                runner.validate_generic_original(altered)

    def test_same_erased_type_does_not_hide_wrong_bounds_variance_or_shadowing(self):
        for scope in ("variance", "shadow", "interface-first", "intersection"):
            classes = generic_facts()
            if scope == "variance":
                classes[runner.GENERIC_BOX]["fields"][runner.GENERIC_BOX + "->lower:Ljava/util/List;"]["signature"] = "Ljava/util/List<+TT;>;"
            else:
                owner = runner.GENERIC_BOX if scope == "shadow" else runner.GENERIC_OPS
                name = {"shadow": "shadow", "interface-first": "interfaceOnly", "intersection": "intersection"}[scope]
                method = next(row for row in classes[owner]["methods"].values() if row["name"] == name)
                method["signature"] = "(Ljava/lang/Object;)Ljava/lang/Object;"
            with self.subTest(scope=scope), self.assertRaisesRegex(RuntimeError, "Signature"):
                runner.validate_generic_original(classes)

    def test_equal_but_wrong_original_and_rebuilt_behavior_cannot_pass(self):
        for key in ("reflection:platform.List", "reflection:platform.Comparable",
                    "reflection:GenericBox.shadow", "exchange-old:0:1", "array-exceptions", "scalar:6"):
            values = generic_values()
            values[key] = 0
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "oracle failed"):
                runner.compare_generic_behavior(values, dict(values))
        incomplete = generic_values()
        del incomplete["reflection:GenericOps.interfaceOnly"]
        with self.assertRaisesRegex(RuntimeError, "complete independent key"):
            runner.compare_generic_behavior(incomplete, dict(incomplete))

    def test_reflection_artifact_requires_all_keys_once(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reflection.tsv"
            lines = [key + "\tobserved" for key in sorted(runner.GENERIC_REFLECTION_KEYS)]
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self.assertEqual(set(runner.generic_reflection(path)), runner.GENERIC_REFLECTION_KEYS)
            for invalid in (lines[:-1], lines + [lines[0]], lines + ["extra\tobserved"], ["bad"]):
                path.write_text("\n".join(invalid) + "\n", encoding="utf-8")
                with self.subTest(lines=len(invalid)), self.assertRaisesRegex(RuntimeError, "reflection evidence"):
                    runner.generic_reflection(path)


class GenericWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.verify = runner.Verify(self.root, self.root / "JDK", self.root / "d8", 30)
        self.original = generic_facts()
        self.rebuilt = copy.deepcopy(self.original)
        for owner, facts in self.rebuilt.items():
            prototype = runner.class_identity.THROW_HELPER_PROTOTYPE
            facts["methods"][owner + "->__neverdThrow" + prototype] = {
                "name": "__neverdThrow", "prototype": prototype, "access": 10, "code": True,
                "signature": runner.class_identity.THROW_HELPER_SIGNATURE}
        self.compiles, self.commands, self.dexes = [], [], []

    def compile(self, sources, output, *, classpath=None):
        output = Path(output)
        sources = list(map(Path, sources))
        self.compiles.append((sources, output, classpath))
        output.mkdir(parents=True)
        # The routing mock creates no executable code. Class facts are supplied
        # separately; cloud acceptance runs the real compiler and JVM instead.
        for owner in self.original:
            target = output / self.original[owner]["path"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"mock")

    def dex(self, paths, output, classpath):
        self.dexes.append((list(paths), output, classpath))
        output.mkdir(parents=True)
        target = output / "classes.dex"
        target.write_bytes(b"mock dex input")
        return target

    def run_command(self, argv, label):
        self.commands.append((list(map(str, argv)), label))
        if label in runner.GENERIC_CASES:
            output = Path(argv[argv.index("-o") + 1])
            sources = ["sources/fixture/GenericBox.java", "sources/fixture/GenericOps.java"]
            for source in sources:
                path = output / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("package fixture; class " + path.stem + " {}\n", encoding="utf-8")
            (output / "metadata").mkdir()
            rows = []
            for owner, facts in self.original.items():
                part = "classes2.dex" if label == "generic-multidex" and owner == runner.GENERIC_BOX else "classes.dex"
                for identity, method in facts["methods"].items():
                    rows.append({"identity": identity, "class": owner, "name": method["name"],
                        "prototype": method["prototype"], "input": part, "status": "recovered", "instruction_count": 2})
            coverage = {"schema_version": 1, "status": "recovered", "class_count": 2, "method_count": 13,
                "recovered_method_count": 13, "declaration_only_method_count": 0, "unrecovered_method_count": 0,
                "methods": rows, "generated_source_helpers": runner.generic_helpers()}
            report = {"status": "success", "platform": "android", "backend": {"name": "neverd", "version": "1", "execution": "builtin"},
                "java_sources": sources, "java_source_count": 2, "dex_count": 2 if label == "generic-multidex" else 1,
                "smali_count": 0, "android_method_recovery": coverage}
            (output / "report.json").write_text(json.dumps(report))
            (output / "metadata/android-methods.json").write_text(json.dumps(coverage))
        if label == "original-generic" or label.endswith("-execution"):
            reflection = Path(argv[-1])
            reflection.write_text("".join(key + "\tobserved " + key + "\n" for key in sorted(runner.GENERIC_REFLECTION_KEYS)), encoding="utf-8")
            return "\n".join(key + "=" + str(value) for key, value in sorted(generic_values().items()))
        return ""

    def inventory(self, directory):
        return copy.deepcopy(self.original if directory == self.root / "original-generic/classes" else self.rebuilt)

    def execute(self):
        with patch.object(self.verify, "run", side_effect=self.run_command), patch.object(self.verify, "compile_local", side_effect=self.compile), \
                patch.object(self.verify, "dex", side_effect=self.dex), \
                patch.object(runner.class_identity, "compiler_classes", side_effect=self.inventory):
            return self.verify.generic_cases(self.root / "neverd")

    def test_two_required_cases_recompile_every_source_and_keep_classpaths_separate(self):
        passed, failures = self.execute()
        self.assertEqual(failures, [])
        self.assertEqual({row["case"] for row in passed}, set(runner.GENERIC_CASES))
        self.assertTrue(all(row["method_count"] == 13 and row["matched_results"] == 103 for row in passed))
        self.assertEqual(len(self.dexes), 3)
        for case in runner.GENERIC_CASES:
            compiled = self.root / case / "compiled"
            invocation = next(row for row in self.compiles if row[1] == compiled)
            self.assertIsNone(invocation[2])
            self.assertEqual(invocation[0], [self.root / case / "recovered/sources/fixture/GenericBox.java",
                                            self.root / case / "recovered/sources/fixture/GenericOps.java"])
            harness = next(row for row in self.compiles if row[1] == self.root / case / "harness")
            self.assertEqual(harness[2], compiled)
            argv = next(argv for argv, label in self.commands if label == case + "-execution")
            self.assertEqual(argv[argv.index("-cp") + 1], os.pathsep.join(map(str, (self.root / case / "harness", compiled))))
            self.assertNotIn(str(self.root / "original-generic/classes"), argv[argv.index("-cp") + 1])
            self.assertTrue((self.root / case / "rebuilt-class-inventory.json").is_file())
            self.assertTrue((self.root / case / "generated-source-hashes.json").is_file())
        partitions = json.loads((self.root / "generic-multidex/input-inventory.json").read_text())["partitions"]
        self.assertEqual(partitions, [[runner.GENERIC_OPS], [runner.GENERIC_BOX]])

    def test_preparation_failure_records_both_required_cases(self):
        with patch.object(self.verify, "run", side_effect=RuntimeError("compiler unavailable")):
            passed, failures = self.verify.generic_cases(self.root / "neverd")
        self.assertEqual(passed, [])
        self.assertEqual({row["case"] for row in failures}, set(runner.GENERIC_CASES))
        self.assertTrue((self.root / "generic-preparation-failure.json").is_file())

    def test_recompiled_erasure_fails_both_cases_instead_of_running_only_behavior(self):
        self.rebuilt[runner.GENERIC_BOX]["signature"] = None
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual({row["case"] for row in failures}, set(runner.GENERIC_CASES))
        self.assertFalse(any(label.endswith("-execution") for _, label in self.commands))
        self.assertTrue(all("class declaration changed" in row["error"] for row in failures))


def constructor_facts():
    """Routing facts only; CI obtains the actual inventory from javac output."""
    classes = {}
    for owner, declarations in runner.CONSTRUCTOR_DECLARATIONS.items():
        fields = {owner + "->" + name + ":" + descriptor:
                  {"name": name, "descriptor": descriptor, "access": access, "constant_value": None,
                   "signature": None, **runner.deprecated_facts()}
                  for name, descriptor, access in declarations["fields"]}
        methods = {owner + "->" + member: {"name": "<init>", "prototype": member[len("<init>"):],
                   "access": 1, "code": True, "signature": signature, **runner.deprecated_facts()}
                   for member, signature in declarations["methods"].items()}
        classes[owner] = {"name": owner, "access": declarations["access"], "superclass": declarations["superclass"],
            "interfaces": [], "fields": fields, "methods": methods, "enclosing_method": None, "inner_class": None,
            "signature": None, "major": 52, "minor": 0, "path": owner[1:-1] + ".class", "size": 4,
            "sha256": "b" * 64, **runner.deprecated_facts()}
    return classes


class ConstructorOracleTests(unittest.TestCase):
    def test_complete_handwritten_overloads_and_two_generic_signatures(self):
        original = constructor_facts()
        runner.validate_constructor_original(original)
        self.assertEqual(len(original), 3)
        methods = [method for facts in original.values() for method in facts["methods"].values()]
        self.assertEqual(len(methods), 6)
        self.assertEqual(sum(method["signature"] is not None for method in methods), 2)
        self.assertEqual(len(runner.CONSTRUCTOR_REFLECTION), 14)
        self.assertEqual(len(runner.expected_keys("constructor")), 140)
        values = runner.constructor_oracle()
        self.assertEqual(values["super:0:0:tag"], 101)
        self.assertEqual(values["this:2:1:tag"], 303)
        self.assertEqual(values["super:0:0:marker"], -7)
        self.assertEqual(values["this:2:1:marker"], 19)
        self.assertEqual(values["base-direct:1:0:tag"], 202)
        self.assertEqual(values["this-direct:1:2:tag"], 404)
        self.assertEqual(values["final:super-body"], 6)
        self.assertEqual(values["final:this-body"], 6)

    def test_missing_overload_erased_formal_and_wrong_parent_are_rejected(self):
        for mutation in ("missing", "erased", "bound", "parent", "extra"):
            original = constructor_facts()
            if mutation == "missing":
                del original["Lfixture/PlainBase;"]["methods"]["Lfixture/PlainBase;-><init>(Ljava/lang/Object;)V"]
            elif mutation in ("erased", "bound"):
                method = original["Lfixture/SuperChild;"]["methods"]["Lfixture/SuperChild;-><init>(Ljava/lang/Object;I)V"]
                method["signature"] = None if mutation == "erased" else "<T::Ljava/lang/CharSequence;>(TT;I)V"
            elif mutation == "parent":
                original["Lfixture/SuperChild;"]["superclass"] = "Ljava/lang/Object;"
            else:
                original["Lfixture/ThisChoice;"]["methods"]["Lfixture/ThisChoice;-><init>()V"] = {
                    "name": "<init>", "prototype": "()V", "code": True, "access": 1, "signature": None}
            with self.subTest(mutation=mutation), self.assertRaisesRegex(RuntimeError, "Constructor original"):
                runner.validate_constructor_original(original)

    def test_wrong_overload_or_duplicate_effect_cannot_pass_even_if_both_results_agree(self):
        mutations = {"super:0:0:tag": 202, "this:2:1:tag": 404, "super:1:0:object-count": 2,
                     "this:1:0:body-count": 0, "this:1:1:sequence-count": 1,
                     "super:2:0:received": 0, "final:this-body": 12}
        for key, changed in mutations.items():
            baseline = runner.constructor_oracle()
            actual = dict(baseline, **{key: changed})
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "rebuilt behavior oracle failed"):
                runner.compare_constructor_behavior(baseline, actual)
            with self.subTest(equal_wrong=key), self.assertRaisesRegex(RuntimeError, "original behavior oracle failed"):
                runner.compare_constructor_behavior(actual, actual)

    def test_each_constructor_behavior_key_and_reflection_fact_is_mandatory(self):
        baseline = runner.constructor_oracle()
        for key in ("super:0:0:tag", "this:2:1:body-count", "reflection:SuperChild.generic",
                    "reflection:KIND.java.lang.Object", "reflection:KIND.java.lang.CharSequence"):
            actual = dict(baseline)
            del actual[key]
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "omitted an independent key"):
                runner.compare_constructor_behavior(baseline, actual)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reflection.tsv"
            lines = [key + "\t" + value for key, value in sorted(runner.CONSTRUCTOR_REFLECTION.items())]
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self.assertEqual(runner.constructor_reflection(path), runner.CONSTRUCTOR_REFLECTION)
            for invalid in (lines[:-1], lines + [lines[0]], lines + ["extra\tclass"],
                            [line.replace("#T;", "#U;") for line in lines],
                            [line.replace("java.lang.Object;interface=false", "java.lang.Object;interface=true") for line in lines]):
                path.write_text("\n".join(invalid) + "\n", encoding="utf-8")
                with self.subTest(invalid=invalid), self.assertRaisesRegex(RuntimeError, "constructor reflection|Constructor reflection"):
                    runner.constructor_reflection(path)


class ConstructorWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.verify = runner.Verify(self.root, self.root / "JDK", self.root / "d8", 30)
        self.original = constructor_facts()
        self.rebuilt = copy.deepcopy(self.original)
        for owner, facts in self.rebuilt.items():
            prototype = runner.class_identity.THROW_HELPER_PROTOTYPE
            facts["methods"][owner + "->__neverdThrow" + prototype] = {
                "name": "__neverdThrow", "prototype": prototype, "access": 10, "code": True,
                "signature": runner.class_identity.THROW_HELPER_SIGNATURE}
        self.actual = runner.constructor_oracle()
        self.helpers = runner.constructor_helpers()
        self.compiles, self.commands, self.dexes = [], [], []

    def compile(self, sources, output, *, classpath=None):
        output.mkdir(parents=True)
        self.compiles.append((list(map(Path, sources)), output, classpath))
        for facts in self.original.values():
            path = output / facts["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"mock")

    def dex(self, paths, output, classpath):
        self.dexes.append((list(paths), output, classpath))
        output.mkdir(parents=True)
        source = output / "classes.dex"
        source.write_bytes(b"mock constructor input")
        return source

    def run_command(self, argv, label):
        self.commands.append((list(map(str, argv)), label))
        if label in runner.CONSTRUCTOR_CASES:
            output = Path(argv[argv.index("-o") + 1])
            sources = ["sources/" + owner[1:-1] + ".java" for owner in sorted(self.original)]
            for source in sources:
                path = output / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("package fixture; class " + path.stem + " {}\n", encoding="utf-8")
            rows = [{"identity": identity, "class": owner, "name": method["name"],
                     "prototype": method["prototype"], "input": "classes.dex", "status": "recovered", "instruction_count": 2}
                    for owner, facts in self.original.items() for identity, method in facts["methods"].items()]
            coverage = {"schema_version": 1, "status": "recovered", "class_count": 3, "method_count": 6,
                "recovered_method_count": 6, "declaration_only_method_count": 0, "unrecovered_method_count": 0,
                "methods": rows, "generated_source_helpers": self.helpers}
            report = {"status": "success", "platform": "android", "backend": {"name": "neverd", "version": "1", "execution": "builtin"},
                "java_sources": sources, "java_source_count": 3, "dex_count": 1, "smali_count": 0,
                "android_method_recovery": coverage}
            (output / "metadata").mkdir()
            (output / "report.json").write_text(json.dumps(report))
            (output / "metadata/android-methods.json").write_text(json.dumps(coverage))
        if label == "original-constructor" or label == "constructor-dex-execution":
            Path(argv[-1]).write_text("".join(key + "\t" + value + "\n"
                for key, value in sorted(runner.CONSTRUCTOR_REFLECTION.items())), encoding="utf-8")
            values = runner.constructor_oracle() if label == "original-constructor" else self.actual
            return "\n".join(key + "=" + str(value) for key, value in sorted(values.items()))
        return ""

    def execute(self):
        def inventory(directory):
            return copy.deepcopy(self.original if directory == self.root / "original-constructor/classes" else self.rebuilt)
        with patch.object(self.verify, "run", side_effect=self.run_command), patch.object(self.verify, "compile_local", side_effect=self.compile), \
                patch.object(self.verify, "dex", side_effect=self.dex), \
                patch.object(runner.class_identity, "compiler_classes", side_effect=inventory):
            return self.verify.constructor_cases(self.root / "neverd")

    def test_native_case_preserves_full_inventory_and_rebuilds_all_three_sources_in_isolation(self):
        passed, failures = self.execute()
        self.assertEqual(failures, [])
        self.assertEqual(len(passed), 1)
        self.assertEqual(passed[0]["case"], "constructor-dex")
        self.assertEqual(passed[0]["method_count"], 6)
        self.assertEqual(passed[0]["matched_results"], 140)
        self.assertEqual(passed[0]["compiler_identity"]["signature_count"], {"classes": 0, "fields": 0, "methods": 2})
        self.assertEqual(len(self.dexes), 1)
        self.assertEqual(len(self.dexes[0][0]), 3)
        compiled = self.root / "constructor-dex/compiled"
        invocation = next(row for row in self.compiles if row[1] == compiled)
        self.assertIsNone(invocation[2])
        self.assertEqual(invocation[0], [self.root / "constructor-dex/recovered/sources" / (owner[1:-1] + ".java")
                                        for owner in sorted(self.original)])
        for label, directory in (("original-constructor", self.root / "original-constructor"),
                                 ("constructor-dex-execution", self.root / "constructor-dex")):
            classes = directory / ("classes" if label == "original-constructor" else "compiled")
            harness = next(row for row in self.compiles if row[1] == directory / "harness")
            self.assertEqual(harness[2], classes)
            argv = next(argv for argv, name in self.commands if name == label)
            self.assertEqual(argv[argv.index("-cp") + 1], os.pathsep.join(map(str, (directory / "harness", classes))))
        for name in ("input-inventory.json", "generated-source-hashes.json", "rebuilt-class-inventory.json",
                     "constructor-identity.json", "reflection.tsv", "reflection.json", "execution.json"):
            self.assertTrue((self.root / "constructor-dex" / name).is_file())

    def test_wrong_overload_with_unchanged_signatures_and_complete_coverage_is_still_failed(self):
        self.actual["super:0:0:tag"] = 202
        self.actual["this:2:1:tag"] = 404
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("behavior oracle failed", failures[0]["error"])
        self.assertTrue((self.root / "constructor-dex/constructor-identity.json").is_file())
        self.assertTrue((self.root / "constructor-dex/execution.json").is_file())
        self.assertTrue((self.root / "constructor-dex/failure.json").is_file())

    def test_missing_rebuilt_constructor_fails_before_harness_execution(self):
        del self.rebuilt["Lfixture/PlainBase;"]["methods"]["Lfixture/PlainBase;-><init>(Ljava/lang/Object;)V"]
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("original method or Signature changed", failures[0]["error"])
        self.assertFalse(any(label == "constructor-dex-execution" for _, label in self.commands))

    def test_missing_helpers_cannot_claim_full_constructor_recovery(self):
        self.helpers = []
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("generated helper", failures[0]["error"])
        self.assertFalse(any(label == "constructor-dex-execution" for _, label in self.commands))

    def test_duplicate_helpers_cannot_compensate_for_declared_inventory(self):
        self.helpers.append(copy.deepcopy(self.helpers[0]))
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("generated helper", failures[0]["error"])
        self.assertFalse(any(label == "constructor-dex-execution" for _, label in self.commands))

    def test_preparation_failure_records_the_required_case_without_skipping(self):
        with patch.object(self.verify, "run", side_effect=RuntimeError("compiler unavailable")):
            passed, failures = self.verify.constructor_cases(self.root / "neverd")
        self.assertEqual(passed, [])
        self.assertEqual([row["case"] for row in failures], ["constructor-dex"])
        self.assertTrue((self.root / "constructor-preparation-failure.json").is_file())


def deprecated_classes():
    """Mock orchestration only; the real case obtains these facts from javac."""
    classes = {}
    for owner, declarations in runner.DEPRECATED_DECLARATIONS.items():
        fields = {owner + "->" + name + ":I": {"name": name, "descriptor": "I", "access": access,
                  "constant_value": None, "signature": None, **runner.deprecated_facts(marked)}
                  for name, access, marked in declarations["fields"]}
        methods = {}
        for member, marked in declarations["methods"].items():
            name, tail = member.split("(", 1)
            methods[owner + "->" + member] = {"name": name, "prototype": "(" + tail, "access": 1,
                                              "code": True, "signature": None, **runner.deprecated_facts(marked)}
        classes[owner] = {"name": owner, "access": declarations["access"], "superclass": declarations["superclass"],
            "interfaces": [], "fields": fields, "methods": methods, "enclosing_method": None, "inner_class": None,
            "signature": None, "major": 52, "minor": 0, "path": owner[1:-1] + ".class", "size": 4,
            "sha256": "c" * 64, **runner.deprecated_facts(declarations["deprecated"])}
    return classes


class DeprecatedOracleTests(unittest.TestCase):
    def test_exact_original_six_bodies_and_four_independent_marker_roles(self):
        original = deprecated_classes()
        runner.validate_deprecated_original(original)
        self.assertEqual(len(original), 2)
        self.assertEqual(sum(len(row["methods"]) for row in original.values()), 6)
        self.assertEqual(sum(row["deprecated_attribute"] for row in original.values()), 1)
        self.assertEqual(sum(field["deprecated_attribute"] for row in original.values() for field in row["fields"].values()), 1)
        methods = [method for row in original.values() for method in row["methods"].values()]
        self.assertEqual(sum(method["deprecated_attribute"] for method in methods if method["name"] == "<init>"), 1)
        self.assertEqual(sum(method["deprecated_attribute"] for method in methods if method["name"] != "<init>"), 1)
        self.assertEqual(len(runner.DEPRECATED_REFLECTION), 13)
        self.assertEqual(len(runner.expected_keys("deprecated")), 92)
        values = runner.deprecated_oracle()
        self.assertEqual(values["deprecated:0:new"], -93)
        self.assertEqual(values["plain:5:combine"], 209)
        self.assertEqual(values["child:0:own"], -502)
        self.assertEqual(values["final:constructor-count"], 18)
        runner.compare_deprecated_behavior(values, values)

    def test_each_marker_fact_is_independent_and_plain_controls_cannot_gain_it(self):
        for scope in ("class", "field", "constructor", "method", "control"):
            for changed in ("attribute", "runtime", "both"):
                original = deprecated_classes()
                base = original[runner.DEPRECATED_BASE]
                if scope == "class": target = base
                elif scope == "field": target = base["fields"][runner.DEPRECATED_BASE + "->legacy:I"]
                elif scope == "constructor": target = base["methods"][runner.DEPRECATED_BASE + "-><init>()V"]
                elif scope == "method": target = base["methods"][runner.DEPRECATED_BASE + "->oldAdd(I)I"]
                else: target = original[runner.DEPRECATED_CHILD]
                replacement = runner.deprecated_facts(scope == "control")
                if changed in ("attribute", "both"): target["deprecated_attribute"] = replacement["deprecated_attribute"]
                if changed in ("runtime", "both"): target["runtime_visible_annotations"] = replacement["runtime_visible_annotations"]
                with self.subTest(scope=scope, changed=changed), self.assertRaisesRegex(RuntimeError, "Deprecated original"):
                    runner.validate_deprecated_original(original)

    def test_false_reflection_or_behavior_cannot_pass_even_when_both_sides_agree(self):
        for key, wrong in {"reflection:PlainChild.class": 0, "deprecated:0:old": -93,
                           "child:5:constructor-delta": 2, "final:constructor-count": 36}.items():
            values = runner.deprecated_oracle()
            values[key] = wrong
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "original behavior oracle failed"):
                runner.compare_deprecated_behavior(values, dict(values))
        values = runner.deprecated_oracle()
        del values["reflection:PlainChild.inherited"]
        with self.assertRaisesRegex(RuntimeError, "omitted an independent key"):
            runner.compare_deprecated_behavior(values, dict(values))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reflection.tsv"
            lines = [key + "\t" + value for key, value in sorted(runner.DEPRECATED_REFLECTION.items())]
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self.assertEqual(runner.deprecated_reflection(path), runner.DEPRECATED_REFLECTION)
            for invalid in (lines[:-1], lines + [lines[0]],
                            [line.replace("class-present=false", "class-present=true") for line in lines]):
                path.write_text("\n".join(invalid) + "\n", encoding="utf-8")
                with self.subTest(invalid=invalid), self.assertRaisesRegex(RuntimeError, "Deprecated reflection"):
                    runner.deprecated_reflection(path)

    def test_unannotated_generic_and_constructor_oracles_keep_annotation_absence(self):
        for factory, validate in ((generic_facts, runner.validate_generic_original),
                                  (constructor_facts, runner.validate_constructor_original)):
            original = factory()
            validate(original)
            for scope in ("class", "field", "method"):
                changed = copy.deepcopy(original)
                facts = next(iter(changed.values()))
                target = facts if scope == "class" else next(iter(facts["fields" if scope == "field" else "methods"].values()))
                target.update(runner.deprecated_facts(True))
                with self.subTest(factory=factory.__name__, scope=scope), self.assertRaises(RuntimeError):
                    validate(changed)


class DeprecatedWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.verify = runner.Verify(self.root, self.root / "JDK", self.root / "d8", 30)
        self.original = deprecated_classes()
        self.rebuilt = copy.deepcopy(self.original)
        for owner, facts in self.rebuilt.items():
            prototype = runner.class_identity.THROW_HELPER_PROTOTYPE
            facts["methods"][owner + "->__neverdThrow" + prototype] = {
                "name": "__neverdThrow", "prototype": prototype, "access": 10, "code": True,
                "signature": runner.class_identity.THROW_HELPER_SIGNATURE, **runner.deprecated_facts()}
        self.actual = runner.deprecated_oracle()
        self.reflection = dict(runner.DEPRECATED_REFLECTION)
        self.helpers = runner.deprecated_helpers()
        self.marker_override = None
        self.compiles, self.commands, self.dexes = [], [], []

    def compile(self, sources, output, *, classpath=None):
        output.mkdir(parents=True)
        self.compiles.append((list(map(Path, sources)), output, classpath))
        for facts in self.original.values():
            path = output / facts["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"mock")

    def dex(self, paths, output, classpath):
        self.dexes.append((list(paths), output, classpath))
        output.mkdir(parents=True)
        source = output / "classes.dex"
        source.write_bytes(b"mock deprecated input")
        return source

    def run_command(self, argv, label):
        self.commands.append((list(map(str, argv)), label))
        if label in runner.DEPRECATED_CASES:
            output = Path(argv[argv.index("-o") + 1])
            sources = ["sources/" + owner[1:-1] + ".java" for owner in sorted(self.original)]
            for source in sources:
                path = output / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("package fixture; class " + path.stem + " {}\n", encoding="utf-8")
            rows = [{"identity": identity, "class": owner, "name": method["name"], "prototype": method["prototype"],
                     "input": "classes.dex", "status": "recovered", "instruction_count": 2,
                     "deprecated": method["deprecated_attribute"]}
                    for owner, facts in self.original.items() for identity, method in facts["methods"].items()]
            if self.marker_override is not None: rows[0]["deprecated"] = self.marker_override
            coverage = {"schema_version": 1, "status": "recovered", "class_count": 2, "method_count": 6,
                "recovered_method_count": 6, "declaration_only_method_count": 0, "unrecovered_method_count": 0,
                "methods": rows, "generated_source_helpers": self.helpers}
            report = {"status": "success", "platform": "android", "backend": {"name": "neverd", "version": "1", "execution": "builtin"},
                "java_sources": sources, "java_source_count": 2, "dex_count": 1, "smali_count": 0,
                "android_method_recovery": coverage}
            (output / "metadata").mkdir()
            (output / "report.json").write_text(json.dumps(report))
            (output / "metadata/android-methods.json").write_text(json.dumps(coverage))
        if label in ("original-deprecated", "deprecated-dex-execution"):
            reflection = runner.DEPRECATED_REFLECTION if label == "original-deprecated" else self.reflection
            Path(argv[-1]).write_text("".join(key + "\t" + value + "\n" for key, value in sorted(reflection.items())), encoding="utf-8")
            values = runner.deprecated_oracle() if label == "original-deprecated" else self.actual
            return "\n".join(key + "=" + str(value) for key, value in sorted(values.items()))
        return ""

    def execute(self):
        def inventory(directory):
            return copy.deepcopy(self.original if directory == self.root / "original-deprecated/classes" else self.rebuilt)
        with patch.object(self.verify, "run", side_effect=self.run_command), patch.object(self.verify, "compile_local", side_effect=self.compile), \
                patch.object(self.verify, "dex", side_effect=self.dex), \
                patch.object(runner.class_identity, "compiler_classes", side_effect=inventory):
            return self.verify.deprecated_cases(self.root / "neverd")

    def test_real_case_requires_all_sources_isolated_classpaths_and_exact_marker_inventory(self):
        passed, failures = self.execute()
        self.assertEqual(failures, [])
        self.assertEqual(len(passed), 1)
        self.assertEqual(passed[0]["case"], "deprecated-dex")
        self.assertEqual(passed[0]["method_count"], 6)
        self.assertEqual(passed[0]["matched_results"], 92)
        self.assertEqual(passed[0]["reflection_declaration_count"], 13)
        expected = {"classes": 1, "fields": 1, "constructors": 1, "methods": 1}
        self.assertEqual(passed[0]["compiler_identity"]["deprecated_attribute_count"], expected)
        self.assertEqual(passed[0]["compiler_identity"]["runtime_visible_deprecated_count"], expected)
        self.assertEqual(len(self.dexes), 1)
        self.assertEqual(len(self.dexes[0][0]), 2)
        compiled = self.root / "deprecated-dex/compiled"
        invocation = next(row for row in self.compiles if row[1] == compiled)
        self.assertIsNone(invocation[2])
        self.assertEqual(invocation[0], [self.root / "deprecated-dex/recovered/sources" / (owner[1:-1] + ".java")
                                        for owner in sorted(self.original)])
        for label, directory in (("original-deprecated", self.root / "original-deprecated"),
                                 ("deprecated-dex-execution", self.root / "deprecated-dex")):
            classes = directory / ("classes" if label == "original-deprecated" else "compiled")
            harness = next(row for row in self.compiles if row[1] == directory / "harness")
            self.assertEqual(harness[2], classes)
            argv = next(argv for argv, name in self.commands if name == label)
            self.assertEqual(argv[argv.index("-cp") + 1], os.pathsep.join(map(str, (directory / "harness", classes))))
        for name in ("input-inventory.json", "generated-source-hashes.json", "rebuilt-class-inventory.json",
                     "deprecated-identity.json", "reflection.tsv", "reflection.json", "execution.json"):
            self.assertTrue((self.root / "deprecated-dex" / name).is_file())

    def test_recompiled_marker_loss_fails_before_reflection_execution(self):
        self.rebuilt[runner.DEPRECATED_BASE]["runtime_visible_annotations"] = []
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertFalse(any(label == "deprecated-dex-execution" for _, label in self.commands))
        self.assertTrue((self.root / "deprecated-dex/rebuilt-class-inventory.json").is_file())

    def test_successful_identity_cannot_hide_wrong_body_or_duplicate_constructor_effect(self):
        self.actual["deprecated:0:old"] = -93
        self.actual["child:5:constructor-delta"] = 2
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("behavior oracle failed", failures[0]["error"])
        self.assertTrue((self.root / "deprecated-dex/deprecated-identity.json").is_file())
        self.assertTrue((self.root / "deprecated-dex/execution.json").is_file())

    def test_runtime_reflection_must_not_inherit_class_marker(self):
        self.reflection["PlainChild.class"] = "declared=java.lang.Deprecated;present=true"
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("reflection evidence changed", failures[0]["error"])

    def test_numeric_native_marker_is_not_a_boolean_fact(self):
        self.marker_override = 1
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("method marker disagrees", failures[0]["error"])
        self.assertFalse(any(label == "deprecated-dex-execution" for _, label in self.commands))

    def test_zero_helpers_cannot_omit_actual_generated_declarations(self):
        self.helpers.clear()
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("generated helper inventory", failures[0]["error"])

    def test_helpers_cannot_receive_the_class_marker(self):
        helper = next(method for method in self.rebuilt[runner.DEPRECATED_BASE]["methods"].values()
                      if method["name"] == "__neverdThrow")
        helper.update(runner.deprecated_facts(True))
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertFalse(any(label == "deprecated-dex-execution" for _, label in self.commands))

    def test_original_preparation_failure_records_required_case_without_skipping(self):
        with patch.object(self.verify, "run", side_effect=RuntimeError("compiler unavailable")):
            passed, failures = self.verify.deprecated_cases(self.root / "neverd")
        self.assertEqual(passed, [])
        self.assertEqual([row["case"] for row in failures], ["deprecated-dex"])
        self.assertTrue((self.root / "deprecated-preparation-failure.json").is_file())


def marker_classes(*, rebuilt=False):
    """Mock pipeline facts only; the real oracle reads javac output from disk."""
    classes = {}
    for name in runner.MARKER_VISIBLE:
        owner = "Lfixture/" + name + ";"
        annotation = name in runner.MARKER_DEFINITIONS
        access, superclass, interfaces, fields, members = runner.MARKER_CONCRETE.get(
            name, (0x2601 if annotation else 0x601, "Ljava/lang/Object;",
                   ["Ljava/lang/annotation/Annotation;"] if annotation else [], [], []))
        field_rows = {owner + "->" + field + ":" + descriptor: {
            "name": field, "descriptor": descriptor, "access": flags, "constant_value": None,
            "signature": None, **runner.deprecated_facts()} for field, descriptor, flags in fields}
        method_rows = {}
        for member in members:
            method, tail = member.split("(", 1)
            method_rows[owner + "->" + member] = {"name": method, "prototype": "(" + tail,
                "access": 1, "code": True, "signature": None, **runner.deprecated_facts()}
        if rebuilt and name in runner.MARKER_CONCRETE:
            prototype = runner.class_identity.THROW_HELPER_PROTOTYPE
            method_rows[owner + "->__neverdThrow" + prototype] = {
                "name": "__neverdThrow", "prototype": prototype, "access": 10, "code": True,
                "signature": runner.class_identity.THROW_HELPER_SIGNATURE, **runner.deprecated_facts()}
        classes[owner] = {"name": owner, "access": access, "superclass": superclass,
            "interfaces": list(interfaces), "fields": field_rows, "methods": method_rows,
            "enclosing_method": None, "inner_class": None, "signature": None,
            "source_file": name + ".java", "major": 52, "minor": 0,
            "path": owner[1:-1] + ".class", "size": 4, "sha256": "d" * 64,
            "deprecated_attribute": False,
            "runtime_visible_annotations": copy.deepcopy(runner.MARKER_VISIBLE[name]),
            "runtime_invisible_annotations": copy.deepcopy(runner.MARKER_INVISIBLE.get(name, []))}
    return classes


class MarkerOracleTests(unittest.TestCase):
    def test_fixed_counts_roles_default_distinctions_and_independent_numeric_values(self):
        for rebuilt in (False, True):
            classes = marker_classes(rebuilt=rebuilt)
            result = runner.validate_marker_compiler(classes, rebuilt=rebuilt)
            self.assertEqual(result["class_count"], 15)
            self.assertEqual(result["annotation_declaration_count"], 9)
            self.assertEqual(result["original_method_count"], 10)
            self.assertEqual(result["generated_helper_count"], 5 if rebuilt else 0)
            self.assertEqual(sum(len(row["methods"]) for row in classes.values()), 15 if rebuilt else 10)
        self.assertEqual(len(runner.marker_helpers()), 5)
        self.assertEqual(len(runner.MARKER_REFLECTION), 39)
        self.assertEqual(len(runner.expected_keys("marker")), 81)
        self.assertEqual(len(runner.expected_keys("deprecated")), 92)
        self.assertIn("retention=absent;target=absent", runner.MARKER_REFLECTION["MarkerDefault.definition"])
        self.assertIn("retention=CLASS;target=[TYPE_USE]", runner.MARKER_REFLECTION["MarkerClass.definition"])
        self.assertIn("retention=absent;target=[]", runner.MARKER_REFLECTION["MarkerEmptyTarget.definition"])
        self.assertIn("target=[TYPE,ANNOTATION_TYPE]", runner.MARKER_REFLECTION["MarkerRuntime.definition"])
        self.assertEqual(runner.MARKER_REFLECTION["MarkerChild.declared"], "types=")
        self.assertEqual(runner.MARKER_REFLECTION["MarkerChild.present"], "types=fixture.MarkerInherited")
        self.assertEqual(runner.MARKER_REFLECTION["MarkerImplementer.present"], "types=")
        self.assertEqual(runner.MARKER_REFLECTION["MarkerSourceUse.declared"], "types=")
        values = runner.marker_oracle()
        self.assertEqual(values["base-seed:0"], -97)
        self.assertEqual(values["base-adjust:5"], 201)
        self.assertEqual(values["child-shifted:0"], -187)
        self.assertEqual(values["child-shifted:5"], 213)
        self.assertEqual(values["plain:4"], 17)
        self.assertEqual(values["implements:1"], 10)
        self.assertEqual(values["source-use:2"], -7)
        runner.compare_marker_behavior(values, dict(values))

    def test_matching_wrong_original_and_rebuilt_metadata_both_fail_fixed_contract(self):
        def change(classes, kind):
            owner = "Lfixture/MarkerDefault;"
            if kind == "explicit-class-default":
                classes[owner]["runtime_visible_annotations"] = runner.marker_meta(retention="CLASS")
            elif kind == "explicit-empty-default":
                classes[owner]["runtime_visible_annotations"] = runner.marker_meta(targets=[])
            elif kind == "missing-empty-target":
                classes["Lfixture/MarkerEmptyTarget;"]["runtime_visible_annotations"] = []
            elif kind == "sorted-target":
                rows = classes["Lfixture/MarkerRuntime;"]["runtime_visible_annotations"]
                target = next(row for row in rows if row["type"] == "Ljava/lang/annotation/Target;")
                target["elements"][0]["values"].reverse()
            elif kind == "inherited-on-plain":
                classes["Lfixture/MarkerPlain;"]["runtime_visible_annotations"] = runner.marker_uses("MarkerInherited")
            elif kind == "meta-inherited-on-plain":
                classes["Lfixture/MarkerPlain;"]["runtime_visible_annotations"] = runner.marker_meta(inherited=True)
            elif kind == "source-application":
                classes["Lfixture/MarkerSourceUse;"]["runtime_invisible_annotations"] = runner.marker_uses("MarkerSource")
            elif kind == "runtime-as-class":
                row = classes["Lfixture/MarkerBase;"]
                marker = next(value for value in row["runtime_visible_annotations"] if value["type"] == "Lfixture/MarkerRuntime;")
                row["runtime_visible_annotations"].remove(marker)
                row["runtime_invisible_annotations"].append(marker)
            elif kind == "type-use-becomes-type":
                classes["Lfixture/MarkerClass;"]["runtime_visible_annotations"] = runner.marker_meta(retention="CLASS", targets=["TYPE"])
            elif kind == "missing-type-use-annotation-role":
                rows = classes["Lfixture/MarkerTagged;"]["runtime_invisible_annotations"]
                rows[:] = [row for row in rows if row["type"] != "Lfixture/MarkerClass;"]
            elif kind == "nonempty-marker":
                classes["Lfixture/MarkerTagged;"]["runtime_visible_annotations"][0]["elements"] = [{"name": "value", "tag": "s", "value": "x"}]
            else:
                classes["Lfixture/MarkerPlain;"]["runtime_visible_annotations"] = [{"type": "Lfixture/External;", "elements": []}]
        kinds = ("explicit-class-default", "explicit-empty-default", "missing-empty-target", "sorted-target",
                 "inherited-on-plain", "meta-inherited-on-plain", "source-application", "runtime-as-class",
                 "type-use-becomes-type", "missing-type-use-annotation-role", "nonempty-marker", "external")
        for kind in kinds:
            original, rebuilt = marker_classes(), marker_classes(rebuilt=True)
            change(original, kind)
            change(rebuilt, kind)
            for is_rebuilt, classes in ((False, original), (True, rebuilt)):
                with self.subTest(kind=kind, rebuilt=is_rebuilt), self.assertRaisesRegex(RuntimeError, "annotation contract"):
                    runner.validate_marker_compiler(classes, rebuilt=is_rebuilt)

    def test_exact_member_and_helper_inventory_rejects_shape_flags_and_boolean_aliases(self):
        for kind in ("missing-body", "invented-member", "annotation-helper", "interface-helper", "missing-helper",
                     "helper-marker", "helper-signature", "helper-access", "code-integer", "body-removed"):
            classes = marker_classes(rebuilt=True)
            plain = classes["Lfixture/MarkerPlain;"]
            body_key = "Lfixture/MarkerPlain;->value(I)I"
            helper_key = "Lfixture/MarkerPlain;->__neverdThrow" + runner.class_identity.THROW_HELPER_PROTOTYPE
            if kind == "missing-body": del plain["methods"][body_key]
            elif kind == "invented-member":
                classes["Lfixture/MarkerDefault;"]["methods"]["Lfixture/MarkerDefault;->value()I"] = {
                    "name": "value", "prototype": "()I", "access": 0x401, "code": False,
                    "signature": None, **runner.deprecated_facts()}
            elif kind in ("annotation-helper", "interface-helper"):
                owner = "Lfixture/MarkerDefault;" if kind == "annotation-helper" else "Lfixture/MarkerInterface;"
                classes[owner]["methods"][owner + "->__neverdThrow" + runner.class_identity.THROW_HELPER_PROTOTYPE] = copy.deepcopy(plain["methods"][helper_key])
            elif kind == "missing-helper": del plain["methods"][helper_key]
            elif kind == "helper-marker": plain["methods"][helper_key]["runtime_visible_annotations"] = runner.marker_uses("MarkerRuntime")
            elif kind == "helper-signature": plain["methods"][helper_key]["signature"] = None
            elif kind == "helper-access": plain["methods"][helper_key]["access"] = 9
            elif kind == "code-integer": plain["methods"][body_key]["code"] = 1
            else: plain["methods"][body_key]["code"] = False
            with self.subTest(kind=kind), self.assertRaisesRegex(RuntimeError, "member|Code"):
                runner.validate_marker_compiler(classes, rebuilt=True)

    def test_reflection_and_behavior_equal_wrong_pairs_are_independently_rejected(self):
        for key, wrong in (("base-adjust:5", 200), ("child-shifted:0", -188),
                           ("reflection:MarkerChild.present", 0), ("implements:1", True)):
            values = runner.marker_oracle()
            values[key] = wrong
            with self.subTest(key=key), self.assertRaisesRegex(RuntimeError, "original independent"):
                runner.compare_marker_behavior(values, dict(values))
        values = runner.marker_oracle()
        del values["source-use:5"]
        with self.assertRaisesRegex(RuntimeError, "original independent"):
            runner.compare_marker_behavior(values, dict(values))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "reflection.tsv"
            lines = [key + "\t" + value for key, value in sorted(runner.MARKER_REFLECTION.items())]
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self.assertEqual(runner.marker_reflection(path), runner.MARKER_REFLECTION)
            changes = {
                "MarkerDefault.definition": runner.MARKER_REFLECTION["MarkerDefault.definition"].replace("retention=absent", "retention=CLASS"),
                "MarkerDefault.definition-empty": runner.MARKER_REFLECTION["MarkerDefault.definition"].replace("target=absent", "target=[]"),
                "MarkerRuntime.definition": runner.MARKER_REFLECTION["MarkerRuntime.definition"].replace("TYPE,ANNOTATION_TYPE", "ANNOTATION_TYPE,TYPE"),
                "MarkerPlain.present": "types=fixture.MarkerInherited",
                "MarkerImplementer.present": "types=fixture.MarkerInherited",
            }
            for key, value in changes.items():
                actual_key = key.removesuffix("-empty")
                wrong = dict(runner.MARKER_REFLECTION)
                wrong[actual_key] = value
                # Identical original and generated files still fail the fixed contract.
                for side in ("original", "rebuilt"):
                    path.write_text("".join(name + "\t" + text + "\n" for name, text in sorted(wrong.items())), encoding="utf-8")
                    with self.subTest(key=key, side=side), self.assertRaisesRegex(RuntimeError, "independent declaration"):
                        runner.marker_reflection(path)
            for invalid in (lines[:-1], lines + [lines[0]], lines + ["unknown\ttypes="]):
                path.write_text("\n".join(invalid) + "\n", encoding="utf-8")
                with self.subTest(invalid=invalid), self.assertRaises(RuntimeError):
                    runner.marker_reflection(path)


class MarkerWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.verify = runner.Verify(self.root, self.root / "JDK", self.root / "d8", 30)
        self.original, self.rebuilt = marker_classes(), marker_classes(rebuilt=True)
        self.baseline, self.actual = runner.marker_oracle(), runner.marker_oracle()
        self.original_reflection, self.reflection = dict(runner.MARKER_REFLECTION), dict(runner.MARKER_REFLECTION)
        self.helpers = runner.marker_helpers()
        self.coverage_mutation = None
        self.compiles, self.commands, self.dexes, self.inventory_modes = [], [], [], []

    def compile(self, sources, output, *, classpath=None):
        output.mkdir(parents=True)
        self.compiles.append((list(map(Path, sources)), output, classpath))
        for facts in self.original.values():
            path = output / facts["path"]
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"mock")

    def dex(self, paths, output, classpath):
        self.dexes.append((list(paths), output, classpath))
        output.mkdir(parents=True)
        path = output / "classes.dex"
        path.write_bytes(b"mock owned marker input")
        return path

    def run_command(self, argv, label):
        self.commands.append((list(map(str, argv)), label))
        if label == "marker-dex":
            output = Path(argv[argv.index("-o") + 1])
            sources = ["sources/" + owner[1:-1] + ".java" for owner in sorted(self.original)]
            for source in sources:
                path = output / source
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("package fixture; class " + path.stem + " {}\n", encoding="utf-8")
            rows = [{"identity": identity, "class": owner, "name": method["name"], "prototype": method["prototype"],
                     "input": "classes.dex", "status": "recovered", "instruction_count": 2, "deprecated": False}
                    for owner, facts in self.original.items() for identity, method in facts["methods"].items()]
            coverage = {"schema_version": 1, "status": "recovered", "class_count": 15, "method_count": 10,
                "recovered_method_count": 10, "declaration_only_method_count": 0, "unrecovered_method_count": 0,
                "methods": rows, "generated_source_helpers": self.helpers}
            if self.coverage_mutation is not None:
                self.coverage_mutation(coverage)
            report = {"status": "success", "platform": "android", "backend": {"name": "neverd", "version": "1", "execution": "builtin"},
                "java_sources": sources, "java_source_count": 15, "dex_count": 1, "smali_count": 0,
                "android_method_recovery": coverage}
            (output / "metadata").mkdir()
            (output / "report.json").write_text(json.dumps(report))
            (output / "metadata/android-methods.json").write_text(json.dumps(coverage))
        if label in ("original-marker", "marker-dex-execution"):
            reflection = self.original_reflection if label == "original-marker" else self.reflection
            Path(argv[-1]).write_text("".join(key + "\t" + value + "\n" for key, value in sorted(reflection.items())), encoding="utf-8")
            values = self.baseline if label == "original-marker" else self.actual
            return "\n".join(key + "=" + str(value) for key, value in sorted(values.items()))
        return ""

    def execute(self):
        def inventory(directory, *, owned_markers):
            self.inventory_modes.append((directory, owned_markers))
            return copy.deepcopy(self.original if directory == self.root / "original-marker/classes" else self.rebuilt)
        with patch.object(self.verify, "run", side_effect=self.run_command), patch.object(self.verify, "compile_local", side_effect=self.compile), \
                patch.object(self.verify, "dex", side_effect=self.dex), \
                patch.object(runner.class_identity, "compiler_classes", side_effect=inventory):
            return self.verify.marker_cases(self.root / "neverd")

    def test_full_case_binds_exact_sources_denominator_and_isolated_compiler_modes(self):
        passed, failures = self.execute()
        self.assertEqual(failures, [])
        self.assertEqual(len(passed), 1)
        result = passed[0]
        self.assertEqual(result["case"], "marker-dex")
        self.assertEqual(result["class_count"], 15)
        self.assertEqual(result["method_count"], 10)
        self.assertEqual(result["matched_results"], 81)
        self.assertEqual(result["reflection_declaration_count"], 39)
        self.assertEqual(result["compiler_identity"]["annotation_declaration_count"], 9)
        self.assertEqual(result["compiler_identity"]["generated_helper_count"], 5)
        self.assertEqual(self.inventory_modes, [(self.root / "original-marker/classes", True), (self.root / "marker-dex/compiled", True)])
        self.assertEqual(len(self.dexes), 1)
        self.assertEqual(self.dexes[0][0], [self.root / "original-marker/classes" / (owner[1:-1] + ".class") for owner in sorted(self.original)])
        self.assertEqual(self.dexes[0][2], self.root / "original-marker/classes")
        self.assertEqual(len(self.compiles), 4)
        self.assertEqual(len(self.compiles[0][0]), 15)
        self.assertIsNone(self.compiles[0][2])
        compiled = self.root / "marker-dex/compiled"
        invocation = next(row for row in self.compiles if row[1] == compiled)
        self.assertIsNone(invocation[2])
        self.assertEqual(invocation[0], [self.root / "marker-dex/recovered/sources" / (owner[1:-1] + ".java") for owner in sorted(self.original)])
        for label, directory in (("original-marker", self.root / "original-marker"), ("marker-dex-execution", self.root / "marker-dex")):
            classes = directory / ("classes" if label == "original-marker" else "compiled")
            harness = next(row for row in self.compiles if row[1] == directory / "harness")
            self.assertEqual(harness[0], [self.root / "original-marker/harness-source/MarkerHarness.java"])
            self.assertEqual(harness[2], classes)
            argv = next(argv for argv, name in self.commands if name == label)
            self.assertEqual(argv[argv.index("-cp") + 1], os.pathsep.join(map(str, (directory / "harness", classes))))
        for name in ("input-inventory.json", "generated-source-hashes.json", "rebuilt-class-inventory.json",
                     "marker-identity.json", "reflection.tsv", "reflection.json", "execution.json"):
            self.assertTrue((self.root / "marker-dex" / name).is_file())
        self.assertTrue((self.root / "original-marker/marker-identity.json").is_file())

    def test_matching_wrong_default_metadata_fails_before_d8(self):
        for classes in (self.original, self.rebuilt):
            classes["Lfixture/MarkerDefault;"]["runtime_visible_annotations"] = runner.marker_meta(retention="CLASS")
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertEqual(failures[0]["stage"], "original-preparation")
        self.assertEqual(self.dexes, [])
        self.assertFalse(any(label == "original-marker" for _, label in self.commands))

    def test_rebuilt_target_loss_fails_before_reflection_execution(self):
        self.rebuilt["Lfixture/MarkerEmptyTarget;"]["runtime_visible_annotations"] = []
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("annotation contract", failures[0]["error"])
        self.assertFalse(any(label == "marker-dex-execution" for _, label in self.commands))
        self.assertTrue((self.root / "marker-dex/rebuilt-class-inventory.json").is_file())

    def test_matching_wrong_behavior_is_rejected_at_original_preparation(self):
        self.baseline["child-shifted:5"] = self.actual["child-shifted:5"] = 212
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("original independent behavior", failures[0]["error"])
        self.assertEqual(self.dexes, [])

    def test_generated_numeric_failure_remains_failure_after_declaration_acceptance(self):
        self.actual["plain:4"] = 18
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("rebuilt independent behavior", failures[0]["error"])
        self.assertTrue((self.root / "marker-dex/marker-identity.json").is_file())
        self.assertTrue((self.root / "marker-dex/execution.json").is_file())

    def test_matching_wrong_inheritance_reflection_fails_original_contract(self):
        self.original_reflection["MarkerPlain.present"] = self.reflection["MarkerPlain.present"] = "types=fixture.MarkerInherited"
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("independent declaration contract", failures[0]["error"])
        self.assertEqual(self.dexes, [])

    def test_missing_native_body_cannot_reduce_the_original_method_denominator(self):
        def missing(coverage):
            coverage["methods"].pop()
            coverage["method_count"] -= 1
            coverage["recovered_method_count"] -= 1
        self.coverage_mutation = missing
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertFalse(any(output == self.root / "marker-dex/compiled" for _, output, _ in self.compiles))

    def test_zero_helpers_cannot_hide_generated_declarations(self):
        self.helpers.clear()
        passed, failures = self.execute()
        self.assertEqual(passed, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("generated helper inventory", failures[0]["error"])

    def test_original_preparation_exception_records_the_required_case(self):
        with patch.object(self.verify, "run", side_effect=RuntimeError("compiler unavailable")):
            passed, failures = self.verify.marker_cases(self.root / "neverd")
        self.assertEqual(passed, [])
        self.assertEqual([row["case"] for row in failures], ["marker-dex"])
        self.assertTrue((self.root / "marker-preparation-failure.json").is_file())


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
