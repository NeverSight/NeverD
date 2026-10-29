"""Independent truth, result-admission and tool-format regression checks."""
import argparse
from contextlib import closing
import copy
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest
from unittest import mock

from scripts import benchmark_binary_diff as bench


class MappingTests(unittest.TestCase):
    def test_wrong_and_missed_are_not_disjoint(self):
        result = bench.score({(1, 3), (2, 4)}, [(1, 4), (2, 3)])
        self.assertEqual((result["correct"], result["wrong"], result["missed"]), (0, 2, 2))
        self.assertEqual(result["precision"], 0)
        self.assertEqual(result["recall"], 0)
        self.assertFalse(result["exact_mapping"])

    def test_repeated_correlator_evidence_does_not_inflate_score(self):
        result = bench.score({(1, 3), (2, 4)}, [(1, 3), (1, 3), (2, 4)])
        self.assertEqual(result["accepted_pairs"], 2)
        self.assertEqual(result["duplicate_evidence_rows"], 1)
        self.assertEqual(result["correct"], 2)
        self.assertTrue(result["exact_mapping"])

    def test_conflicting_associations_are_not_overwritten(self):
        result = bench.score({(1, 3), (2, 4)}, [(1, 3), (1, 4)])
        self.assertEqual((result["correct"], result["wrong"], result["missed"]), (1, 1, 1))
        self.assertEqual(result["wrong_pairs"], [["0x1", "0x4"]])

    def test_abstention_has_undefined_precision(self):
        result = bench.score({(1, 3)}, [])
        self.assertIsNone(result["precision"])
        self.assertEqual(result["recall"], 0)
        self.assertEqual(result["f1"], 0)
        self.assertFalse(result["exact_mapping"])

    def test_unrelated_functions_have_no_expected_pair(self):
        expected = bench.truth_pairs([
            {"id": "removed", "primary": "0x1000", "secondary": None},
            {"id": "added", "primary": None, "secondary": "0x2000"},
        ])
        result = bench.score(expected, [(0x1000, 0x2000)])
        self.assertEqual(result["wrong"], 1)
        self.assertIsNone(result["recall"])
        self.assertEqual(result["f1"], 0)
        self.assertFalse(result["exact_mapping"])

    def test_partial_truth_does_not_score_unknown_pairs_as_errors(self):
        result = bench.score_partial({(1, 3), (2, 4)}, [(1, 3), (9, 10), (9, 10)])
        self.assertEqual((result["correct"], result["wrong"], result["missed"], result["unscored"]), (1, 0, 1, 1))
        self.assertEqual(result["accepted_pairs"], 2)
        self.assertEqual(result["scored_accepted_pairs"], 1)
        self.assertEqual(result["duplicate_evidence_rows"], 1)
        self.assertEqual(result["precision"], 1)
        self.assertEqual(result["recall"], 0.5)
        self.assertAlmostEqual(result["f1"], 2 / 3)
        self.assertIsNone(result["exact_mapping"])

    def test_either_known_endpoint_makes_a_contradiction_scorable(self):
        result = bench.score_partial({(1, 3), (2, 4)}, [(1, 7), (8, 4), (9, 10)])
        self.assertEqual((result["wrong"], result["missed"], result["unscored"]), (2, 2, 1))
        self.assertEqual(result["f1"], 0)

    def test_partial_exactness_never_promises_whole_mapping_exactness(self):
        result = bench.score_partial({(1, 3)}, [(1, 3), (9, 10)])
        self.assertTrue(result["scored_mapping_exact"])
        self.assertIsNone(result["exact_mapping"])
        self.assertTrue(bench.mapping_passed({"metrics": result}))
        empty = bench.score_partial({(1, 3)}, [(9, 10)])
        self.assertIsNone(empty["precision"])
        self.assertEqual(empty["f1"], 0)
        self.assertFalse(bench.mapping_passed({"metrics": empty}))

    def test_native_relocation_uses_declared_bases_and_complete_inventories(self):
        case = {"primary": {"image_base": "0x0"}, "secondary": {"image_base": "0x400000"},
                "truth": [{"primary": "0x120", "secondary": "0x400230"},
                          {"primary": "0x240", "secondary": "0x400350"}]}
        spaces = [{"image_base": 0x100000, "entries": [0x100120]},
                  {"image_base": 0x200000, "entries": [0x200230, 0x200350]}]
        pairs, coverage = bench.normalize_pairs([(0x100120, 0x200230)], spaces, case)
        self.assertEqual(pairs, [(0x120, 0x400230)])
        self.assertEqual(coverage[0]["missing_labelled_functions"], ["0x240"])
        self.assertEqual(coverage[1]["discovered_labelled_functions"], 2)
        with self.assertRaisesRegex(ValueError, "below"):
            bench.normalize_pairs([(0x120, 0x200230)], spaces, case)
        with self.assertRaisesRegex(ValueError, "absent"):
            bench.normalize_pairs([(0x100130, 0x200230)], spaces, case)

    def test_invalid_truth_is_not_silently_reduced(self):
        row = {"id": "one", "primary": "0x1000", "secondary": "0x2000"}
        cases = [[], [row, row], [row, dict(row, id="other")],
                 [dict(row, primary=None, secondary=None)], [dict(row, id="")]]
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(ValueError):
                bench.truth_pairs(rows)

    def test_address_formats_and_bounds(self):
        self.assertEqual(bench.address("0x1000"), 4096)
        self.assertEqual(bench.address("001000", 16), 4096)
        self.assertEqual(bench.address("001000", 10), 1000)
        self.assertEqual(bench.address((1 << 64) - 1), (1 << 64) - 1)
        for value in (True, -1, 1 << 64, 1.5, None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                bench.address(value)


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.inputs = [self.root / "primary.bin", self.root / "secondary.bin"]
        self.inputs[0].write_bytes(b"original input")
        self.inputs[1].write_bytes(b"changed input")
        self.report = {
            "schema": "neverd.bindiff", "schema_version": 3, "state": "complete",
            "primary": {"sha256": bench.digest(self.inputs[0])},
            "secondary": {"sha256": bench.digest(self.inputs[1])},
            "matches": [{"primary": "0x1000", "secondary": "0x2000"}],
            "ambiguities": [{"primary": "0x1000", "secondary": ["0x3000"]}],
        }
        self.case = {
            "id": "owned", "category": "test", "truth_complete": True,
            "primary": bench.artifact(self.inputs[0], self.root),
            "secondary": bench.artifact(self.inputs[1], self.root),
            "truth": [{"id": "entry", "primary": "0x1000", "secondary": "0x2000"}],
        }

    def write(self, name, data):
        path = self.root / name
        path.write_text(json.dumps(data))
        return path

    def receipt(self):
        path = self.write("report.json", self.report)
        run = {
            "schema": bench.RUN_SCHEMA, "case": "owned", "format": "neverd",
            "track": "end-to-end", "completed": True, "tool": {"name": "NeverD", "version": "test"},
            "input_sha256": [bench.digest(p) for p in self.inputs],
            "result": bench.artifact(path, self.root), "artifacts": [],
        }
        corpus_path = self.write("corpus.json", {"schema": bench.CORPUS_SCHEMA, "cases": [self.case]})
        return corpus_path, self.write("run.json", run), run

    def test_absolute_artifact_retains_hash_checks_when_relative_paths_are_unavailable(self):
        source = self.inputs[0]
        with mock.patch.object(bench.os.path, "relpath", side_effect=ValueError("different drives")):
            record = bench.artifact(source, self.root)
        self.assertEqual(record["path"], str(source.resolve()))
        self.assertEqual(bench.checked_artifact(record, self.root), source.resolve())
        source.write_bytes(b"changed after recording")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            bench.checked_artifact(record, self.root)

    def test_complete_report_scores_only_accepted_matches(self):
        corpus, run, _ = self.receipt()
        result = bench.evaluate(corpus, run)
        self.assertTrue(result["metrics"]["exact_mapping"])
        self.assertEqual(result["metrics"]["accepted_pairs"], 1)
        self.assertEqual(result["input_binding"], "native-and-receipt")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux CLI/runtime execution")
    def test_runner_retains_bad_output_and_distinguishes_mapping_from_body_coverage(self):
        report = copy.deepcopy(self.report)
        report["coverage"] = {"primary": {"incomplete": 2}, "unmeasured": 0}
        report["primary"]["discovery_complete"] = False
        report["secondary"]["discovery_complete"] = True
        executable = self.root / "neverd"
        executable.write_text(f"#!{sys.executable}\n" +
            "import json, sys\nfrom pathlib import Path\n" +
            "if '--version' in sys.argv:\n print('owned fake CLI')\n sys.exit(0)\n" +
            "output = Path(sys.argv[sys.argv.index('--save') + 1])\n" +
            "if output.parent.name == 'bad':\n output.write_text('{broken')\n sys.exit(1)\n" +
            f"report = json.loads({json.dumps(report)!r})\n" +
            "if output.parent.name == 'stopped':\n report['state'] = 'cancelled'\n" +
            "output.write_text(json.dumps(report))\nsys.exit(3)\n")
        executable.chmod(0o755)
        library = self.root / "libneverd.so"
        library.write_bytes(b"owned runner fixture, never loaded")
        path = self.write("corpus.json", {"schema": bench.CORPUS_SCHEMA,
            "cases": [dict(self.case, id=name) for name in ("bad", "partial", "stopped")]})
        output = self.root / "runs"
        args = argparse.Namespace(corpus=path, neverd=executable, library=library,
                                  output_dir=output, threads=1, timeout=10)
        self.assertEqual(bench.run_neverd(args), 2)
        results = bench.load(output / "results.json")["cases"]
        self.assertEqual(len(results), 3)
        self.assertIn("error", results[0])
        self.assertTrue(results[1]["metrics"]["exact_mapping"])
        self.assertEqual(results[1]["native_coverage"], report["coverage"])
        self.assertEqual(results[1]["native_discovery_complete"], [False, True])
        self.assertIn("error", results[2])
        receipt = bench.load(output / "bad/run.json")
        self.assertFalse(receipt["completed"])
        self.assertIn("invalid native report", receipt["error"])
        bench.checked_artifact(receipt["result"], output / "bad")

    def test_invalid_corpus_is_rejected_before_runtime_or_output_creation(self):
        invalid_cases = [[], [self.case, self.case],
                         [dict(self.case, truth_complete=False)],
                         [dict(self.case, truth=[])]]
        for identity in ("", ".", "..", "../outside", "/absolute",
                         "nested/case", "nested\\case", "a:b", "line\nname", None):
            invalid_cases.append([dict(self.case, id=identity)])
        output = self.root / "run-output"
        with mock.patch.object(bench.subprocess, "check_output") as launch:
            for cases in invalid_cases:
                corpus = self.write("corpus.json", {
                    "schema": bench.CORPUS_SCHEMA, "cases": cases,
                })
                args = argparse.Namespace(corpus=corpus, output_dir=output,
                                          neverd=self.root / "neverd",
                                          library=self.root / "libneverd.so",
                                          threads=1, timeout=1)
                with self.subTest(cases=cases), self.assertRaises(ValueError):
                    bench.run_neverd(args)
                self.assertFalse(output.exists())
            launch.assert_not_called()

    def test_evaluator_rejects_empty_corpus_and_duplicate_case_identities(self):
        corpus, run, _ = self.receipt()
        for cases in ([], [self.case, self.case]):
            self.write(corpus.name, {"schema": bench.CORPUS_SCHEMA, "cases": cases})
            with self.subTest(cases=cases), self.assertRaises(ValueError):
                bench.evaluate(corpus, run)

    def test_changed_input_or_result_is_rejected(self):
        for target in ("primary.bin", "report.json"):
            with self.subTest(target=target):
                corpus, run, _ = self.receipt()
                original = (self.root / target).read_bytes()
                (self.root / target).write_bytes(original + b" ")
                with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                    bench.evaluate(corpus, run)
                (self.root / target).write_bytes(original)

    def test_swapped_native_binding_is_rejected(self):
        self.report["primary"], self.report["secondary"] = self.report["secondary"], self.report["primary"]
        corpus, run, _ = self.receipt()
        with self.assertRaisesRegex(ValueError, "input identity"):
            bench.evaluate(corpus, run)

    def test_partial_native_result_is_rejected_even_with_success_receipt(self):
        for state in ("cancelled", "resource_limited", "deadline"):
            with self.subTest(state=state):
                self.report["state"] = state
                corpus, run, _ = self.receipt()
                with self.assertRaisesRegex(ValueError, "did not complete"):
                    bench.evaluate(corpus, run)

    def test_run_admission(self):
        for field, value, message in (("completed", False, "did not complete"),
                                       ("input_sha256", [], "direction"),
                                       ("track", "invented", "track"),
                                       ("tool", {"name": "NeverD", "version": ""}, "version")):
            with self.subTest(field=field):
                corpus, run_path, run = self.receipt()
                run[field] = value
                self.write("run.json", run)
                with self.assertRaisesRegex(ValueError, message):
                    bench.evaluate(corpus, run_path)

    def test_partial_ground_truth_is_not_treated_as_complete(self):
        self.case["truth_complete"] = False
        corpus, run, _ = self.receipt()
        with self.assertRaisesRegex(ValueError, "complete function truth"):
            bench.evaluate(corpus, run)

    def partial_receipt(self):
        self.case.update(truth_complete=False, truth_kind="unique-source-bodies-v1")
        evidence = self.root / "truth-evidence.json"
        evidence.write_text("{}")
        self.case["truth_artifacts"] = [bench.artifact(evidence, self.root)]
        for side, entry in (("primary", "0x1000"), ("secondary", "0x2000")):
            self.case[side]["image_base"] = "0x0"
            self.report[side]["image_base"] = "0x0"
            self.report.setdefault("functions", {})[side] = [[entry]]
        corpus, run, record = self.receipt()
        self.write(corpus.name, {"schema": bench.PARTIAL_CORPUS_SCHEMA, "cases": [self.case]})
        return corpus, run, record

    def test_partial_source_schema_retains_coverage_and_evidence_binding(self):
        corpus, run, _ = self.partial_receipt()
        result = bench.evaluate(corpus, run)
        self.assertTrue(result["metrics"]["scored_mapping_exact"])
        self.assertIsNone(result["metrics"]["exact_mapping"])
        self.assertEqual(result["analysis_coverage"][0]["discovered_labelled_functions"], 1)
        (self.root / "truth-evidence.json").write_text("changed")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            bench.evaluate(corpus, run)

    def test_partial_source_rejects_unqualified_track_and_missing_native_base(self):
        corpus, run, record = self.partial_receipt()
        record["track"] = "common-input"
        self.write(run.name, record)
        with self.assertRaisesRegex(ValueError, "common-input"):
            bench.evaluate(corpus, run)
        record["track"] = "end-to-end"
        del self.report["primary"]["image_base"]
        path = self.write("report.json", self.report)
        record["result"] = bench.artifact(path, self.root)
        self.write(run.name, record)
        with self.assertRaises(KeyError):
            bench.evaluate(corpus, run)

    def test_stale_runtime_artifact_is_rejected(self):
        corpus, run_path, run = self.receipt()
        tool = self.root / "tool"
        tool.write_bytes(b"version one")
        run["artifacts"] = [bench.artifact(tool, self.root)]
        self.write("run.json", run)
        tool.write_bytes(b"version two")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            bench.evaluate(corpus, run_path)

    def test_ghidra_accepts_functions_only_and_retains_correlator_duplicates(self):
        row = {"source": "4096", "destination": "8192", "source_space": "ram",
               "destination_space": "ram", "type": "Function", "status": "ACCEPTED"}
        report = {"schema": "neverd.baseline.ghidra-vt-smoke.v1", "match_sets": [
            {"matches": [row, dict(row, type="Data"), dict(row, status="AVAILABLE"),
                         dict(row, source_space="EXTERNAL", destination_space="EXTERNAL")]},
            {"matches": [copy.deepcopy(row)]},
        ]}
        path = self.write("ghidra.json", report)
        self.assertEqual(bench.read_pairs("ghidra", path, self.inputs), [(4096, 8192), (4096, 8192)])
        row["source_space"] = "overlay"
        self.write("ghidra.json", report)
        with self.assertRaisesRegex(ValueError, "address-space"):
            bench.read_pairs("ghidra", path, self.inputs)

    def test_diaphora_uses_hex_and_excludes_unreliable_and_multimatch(self):
        path = self.root / "result.diaphora"
        with closing(sqlite3.connect(path)) as db, db:
            db.execute("CREATE TABLE results (type TEXT, address TEXT, address2 TEXT)")
            db.executemany("INSERT INTO results VALUES (?, ?, ?)", [
                ("best", "001000", "002000"), ("partial", "001010", "002010"),
                ("unreliable", "001100", "002100"), ("multimatch", "001200", "002200"),
            ])
        self.assertEqual(bench.read_pairs("diaphora", path, self.inputs), [(4096, 8192), (4112, 8208)])
        with closing(sqlite3.connect(path)) as db, db:
            db.execute("INSERT INTO results VALUES ('new category', '0', '0')")
        with self.assertRaisesRegex(ValueError, "unknown Diaphora"):
            bench.read_pairs("diaphora", path, self.inputs)

    def test_native_ghidra_export_requires_the_actual_input_pair(self):
        report = {
            "schema": "neverd.benchmark.ghidra-vt.v1", "complete": True,
            "source_sha256": bench.digest(self.inputs[0]),
            "destination_sha256": bench.digest(self.inputs[1]),
            "match_sets": [{"matches": [{"source": "4096", "destination": "8192",
                "source_space": "ram", "destination_space": "ram", "type": "Function", "status": "ACCEPTED"}]}],
        }
        path = self.write("native-ghidra.json", report)
        self.assertEqual(bench.read_pairs("ghidra-vt", path, self.inputs), [(4096, 8192)])
        with self.assertRaisesRegex(ValueError, "input identity"):
            bench.read_pairs("ghidra-vt", path, list(reversed(self.inputs)))
        report["complete"] = False
        self.write("native-ghidra.json", report)
        with self.assertRaisesRegex(ValueError, "did not complete"):
            bench.read_pairs("ghidra-vt", path, self.inputs)

    def test_bindiff_uses_metadata_orientation_and_unsigned_addresses(self):
        path = self.root / "result.BinDiff"
        with closing(sqlite3.connect(path)) as db, db:
            db.execute("CREATE TABLE metadata (file1 INT, file2 INT)")
            db.execute("CREATE TABLE file (id INT, hash TEXT)")
            db.execute("CREATE TABLE function (address1 BIGINT, address2 BIGINT)")
            db.execute("INSERT INTO metadata VALUES (8, 7)")
            db.executemany("INSERT INTO file VALUES (?, ?)", [
                (7, bench.digest(self.inputs[1], "sha1")), (8, bench.digest(self.inputs[0]))])
            db.execute("INSERT INTO function VALUES (-1, 8192)")
        self.assertEqual(bench.read_pairs("bindiff", path, self.inputs), [((1 << 64) - 1, 8192)])
        with self.assertRaisesRegex(ValueError, "input identity"):
            bench.read_pairs("bindiff", path, list(reversed(self.inputs)))

    def test_map_keeps_same_named_functions_bound_to_their_object(self):
        path = self.root / "link.map"
        path.write_text(""" VMA LMA Size Align Out In Symbol
 1000 1000 20 16 /fixture with spaces/one.o:(.text)
 1000 1000 10 1 one
 1010 1010 10 1 helper
 1020 1020 20 16 /fixture/two.o:(.text)
 1020 1020 10 1 two
 1030 1030 10 1 helper
""")
        expected = {"one.o": {"one", "helper"}, "two.o": {"two", "helper"}}
        self.assertEqual(bench.linker_truth(path, expected), {
            "one.o:one": 0x1000, "one.o:helper": 0x1010,
            "two.o:two": 0x1020, "two.o:helper": 0x1030,
        })
        with self.assertRaisesRegex(ValueError, "missing authored"):
            bench.linker_truth(path, {"one.o": {"missing"}})

    def test_save_never_overwrites_prior_evidence(self):
        path = self.root / "result.json"
        bench.save(path, {"original": True})
        with self.assertRaises(FileExistsError):
            bench.save(path, {"original": False})
        self.assertEqual(bench.load(path), {"original": True})


if __name__ == "__main__":
    unittest.main()
