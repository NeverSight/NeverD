"""Qualification uses every frozen case and rechecks native evidence."""
import argparse
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

from scripts import benchmark_binary_diff as bench
from scripts import benchmark_binary_diff_qualification as qualification


class QualificationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.output = self.root / "runs"
        self.plan = self.root / "plan.json"
        inputs = []
        for side in ("primary", "secondary"):
            path = self.root / (side + ".bin")
            path.write_bytes(side.encode())
            inputs.append(path)
        evidence = self.root / "truth.json"
        evidence.write_text("{}")
        self.case = {
            "id": "first", "category": "owned control", "truth_complete": False,
            "truth_kind": "unique-source-bodies-v1",
            "truth_artifacts": [bench.artifact(evidence, self.root)],
            "truth": [{"id": "function", "primary": "0x1000", "secondary": "0x2000"}],
            **{side: dict(bench.artifact(path, self.root), image_base="0x0")
               for side, path in zip(("primary", "secondary"), inputs)},
        }
        self.corpus = self.write("corpus.json", {
            "schema": bench.PARTIAL_CORPUS_SCHEMA,
            "cases": [self.case, dict(self.case, id="second")],
        })
        self.protocol = self.write("protocol.json", {
            "schema": qualification.PROTOCOL_SCHEMA, "track": "end-to-end", "scope": "owned controls",
            "case_ids": ["first", "second"],
            "gates": {"minimum_labelled_pairs_per_case": 1, "minimum_conditional_precision_per_case": 0.99,
                      "minimum_truth_recall_per_case": 0.9, "minimum_macro_f1": 0.95,
                      "maximum_receipt_failures": 0, "maximum_timeouts": 0},
            "runtime": {"threads": 1, "stage_timeout_seconds": 10},
        })
        report = {
            "schema": "neverd.bindiff", "schema_version": 3, "state": "complete",
            **{side: {"sha256": bench.digest(path), "image_base": "0x0"}
               for side, path in zip(("primary", "secondary"), inputs)},
            "functions": {"primary": [["0x1000"], ["0x1010"]], "secondary": [["0x2000"], ["0x2010"]]},
            "matches": [{"primary": "0x1000", "secondary": "0x2000"},
                        {"primary": "0x1010", "secondary": "0x2010"}],
        }
        self.executable = self.root / "neverd"
        self.executable.write_text(f"#!{sys.executable}\nimport sys\nfrom pathlib import Path\n"
                                   "if '--version' in sys.argv:\n print('owned fixture')\n sys.exit(0)\n"
                                   "path = Path(sys.argv[sys.argv.index('--save') + 1])\n"
                                   f"path.write_text({json.dumps(report)!r})\nsys.exit(3)\n")
        self.executable.chmod(0o755)
        self.library = self.root / "libneverd.so"
        self.library.write_bytes(b"fixture, never loaded")

    def write(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value))
        return path

    def freeze(self):
        return qualification.freeze(argparse.Namespace(
            plan=self.plan, protocol=self.protocol, corpus=self.corpus,
            neverd=self.executable, library=self.library, output_dir=self.output))

    def execute(self):
        self.freeze()
        self.assertEqual(qualification.main(["run", "--plan", str(self.plan)]), 0)
        return bench.load(self.output / "qualification.json")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_success_preserves_unscored_pairs_and_partial_truth(self):
        result = self.execute()
        self.assertTrue(result["passed"])
        self.assertEqual(result["macro_f1"], 1)
        self.assertEqual(len(result["cases"]), 2)
        self.assertTrue(all(case["metrics"]["unscored"] == 1 for case in result["cases"]))
        self.assertTrue(all(case["metrics"]["exact_mapping"] is None for case in result["cases"]))

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_missing_case_cannot_disappear_into_a_successful_macro(self):
        self.execute()
        (self.output / "second/run.json").unlink()
        result = qualification.qualify(self.plan)
        self.assertFalse(result["passed"])
        self.assertEqual(result["receipt_failures"], 1)
        self.assertEqual(result["macro_f1"], 0.5)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_timeout_remains_a_failure(self):
        self.execute()
        path = self.output / "second/compare.json"
        stage = bench.load(path)
        stage["error"] = "timeout"
        path.write_text(json.dumps(stage))
        result = qualification.qualify(self.plan)
        self.assertFalse(result["passed"])
        self.assertEqual((result["receipt_failures"], result["timeouts"]), (1, 1))

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_runtime_probe_failure_is_retained_with_every_case(self):
        self.executable.write_text(f"#!{sys.executable}\nraise SystemExit(7)\n")
        self.freeze()
        self.assertEqual(qualification.main(["run", "--plan", str(self.plan)]), 1)
        result = bench.load(self.output / "qualification.json")
        self.assertFalse(result["passed"])
        self.assertEqual((len(result["cases"]), result["macro_f1"]), (2, 0))
        self.assertIn("error", result["runtime_probe"])
        self.assertEqual(bench.load(self.output / "version.json")["exit"], 7)

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_runtime_probe_uses_the_declared_timeout(self):
        self.executable.write_text(f"#!{sys.executable}\nimport time\ntime.sleep(30)\n")
        declaration = bench.load(self.protocol)
        declaration["runtime"]["stage_timeout_seconds"] = 0.05
        self.protocol.write_text(json.dumps(declaration))
        self.freeze()
        self.assertEqual(qualification.main(["run", "--plan", str(self.plan)]), 1)
        result = bench.load(self.output / "qualification.json")
        self.assertEqual(result["timeouts"], 1)
        self.assertEqual(result["macro_f1"], 0)
        self.assertEqual(bench.load(self.output / "version.json")["error"], "timeout")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_wrong_runtime_or_thread_setting_is_refused(self):
        self.execute()
        path = self.output / "first/run.json"
        original = bench.load(path)
        for field in ("runtime", "threads"):
            run = copy.deepcopy(original)
            if field == "runtime":
                run["artifacts"][0]["sha256"] = "0" * 64
            else:
                run["configuration"]["threads"] = 2
            path.write_text(json.dumps(run))
            with self.subTest(field=field):
                self.assertFalse(qualification.qualify(self.plan)["passed"])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_running_the_same_candidate_with_a_wider_deadline_is_refused(self):
        self.freeze()
        bench.run_neverd(argparse.Namespace(corpus=self.corpus, neverd=self.executable,
                         library=self.library, output_dir=self.output, threads=1, timeout=20))
        result = qualification.qualify(self.plan)
        self.assertFalse(result["passed"])
        self.assertIn("deadline", result["runtime_probe"]["error"])
        self.assertTrue(all("deadline" in case["error"] for case in result["cases"]))

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_edited_aggregate_scores_do_not_replace_native_evidence(self):
        self.execute()
        (self.output / "results.json").write_text('{"passed":true,"cases":[]}')
        path = self.output / "first/result.nddiff"
        path.write_text("{}")
        result = qualification.qualify(self.plan)
        self.assertFalse(result["passed"])
        self.assertEqual(result["receipt_failures"], 1)

    def test_changed_protocol_or_candidate_invalidates_plan(self):
        self.freeze()
        for path in (self.protocol, self.library, self.corpus):
            original = path.read_bytes()
            path.write_bytes(original + b" ")
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                qualification.checked_plan(self.plan)
            path.write_bytes(original)

    def test_existing_output_and_plan_are_never_overwritten(self):
        self.output.mkdir()
        with self.assertRaisesRegex(ValueError, "must not exist"):
            self.freeze()
        self.output.rmdir()
        self.freeze()
        with self.assertRaises(FileExistsError):
            self.freeze()

    def test_protocol_cases_cannot_be_replaced_by_a_smaller_corpus(self):
        corpus = bench.load(self.corpus)
        corpus["cases"].pop()
        self.corpus.write_text(json.dumps(corpus))
        with self.assertRaisesRegex(ValueError, "exactly the protocol cases"):
            self.freeze()

    def test_undefined_precision_and_each_gate_fail_independently(self):
        gates = bench.load(self.protocol)["gates"]
        good = {"expected_pairs": 1, "precision": 1, "recall": 1}
        for key, value in (("expected_pairs", 0), ("precision", None),
                           ("precision", 0.98), ("recall", 0.89)):
            with self.subTest(key=key, value=value):
                self.assertFalse(qualification.gates_passed(dict(good, **{key: value}), gates))

    def test_unknown_or_invalid_gates_fail_before_running(self):
        original = bench.load(self.protocol)
        for key, value in (("minimum_macro_f1", float("nan")), ("minimum_truth_recall_per_case", True),
                           ("minimum_labelled_pairs_per_case", 0), ("maximum_timeouts", 1),
                           ("unsupported_gate", 1)):
            declaration = copy.deepcopy(original)
            declaration["gates"][key] = value
            self.protocol.write_text(json.dumps(declaration))
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.freeze()
            self.assertFalse(self.plan.exists())


if __name__ == "__main__":
    unittest.main()
