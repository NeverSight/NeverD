"""Independent byte layouts and fail-closed reference benchmark validation."""
import contextlib
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
import zlib

from scripts import benchmark_mobile_references as bench
from scripts.tests.test_benchmark_mobile_inventory import leb, uint


def first_code(data):
    map_at = uint(data, 52)
    for index in range(uint(data, map_at)):
        kind, _, _, offset = struct.unpack_from("<HHII", data, map_at + 4 + index * 12)
        if kind == 0x2001:
            size = uint(data, offset + 12)
            return offset, list(struct.unpack_from("<" + "H" * size, data, offset + 16))
    raise AssertionError("missing code map")


def report_for(rows, **extra):
    return {"status": "success", "code_scan_complete": True, "reference_count": len(rows), "references": rows, **extra}


class ReferenceFixtureTests(unittest.TestCase):
    def test_hand_counted_sites_skip_immediate_and_payload_lookalikes(self):
        data, expected, stats = bench.make_reference_dex(class_count=1, methods_per_class=1, matching_methods=1)
        self.assertEqual(data[:8], b"dex\n035\0")
        self.assertEqual(uint(data, 32), len(data))
        self.assertEqual(data[12:32], hashlib.sha1(data[32:]).digest())
        self.assertEqual(uint(data, 8), zlib.adler32(data[12:]) & 0xFFFFFFFF)
        self.assertEqual(uint(data, 104) + uint(data, 108), len(data))
        offset, words = first_code(data)
        self.assertEqual(struct.unpack_from("<HHHHI", data, offset), (2, 0, 0, 0, 0))
        self.assertEqual(len(words), 79)
        self.assertEqual(words[0:3], [0x0014, 0x001A, expected["string"][0]["target_index"]])
        self.assertEqual(words[3:8], [0x0018, 0x0071, expected["method"][0]["target_index"],
                                      0x0060, expected["field"][0]["target_index"]])
        self.assertEqual(words[8:11], [0x0014, 0x001C, expected["type"][0]["target_index"]])
        for kind, pcs in {"string": [11, 68], "type": [13, 71], "field": [15, 73], "method": [17, 75]}.items():
            self.assertEqual([row["pc_code_units"] for row in expected[kind]], pcs)
            self.assertTrue(all(row["method"] == "Lbench/d000/C000000;->probe0000()V" for row in expected[kind]))
        self.assertEqual(words[24:27], [0x0026, 14, 0])
        self.assertEqual(words[28:31], [0x002B, 24, 0])
        self.assertEqual(words[31:34], [0x002C, 27, 0])
        self.assertEqual(words[34:37], [0x002A, 34, 0])
        self.assertEqual(words[38:42], [0x0300, 2, 10, 0])
        self.assertEqual(words[42], 0x001B)
        self.assertEqual(words[52:56], [0x0100, 1, 0x001A, expected["string"][0]["target_index"]])
        self.assertEqual(words[56:58], [40, 0])
        self.assertEqual(words[58:60], [0x0200, 2])
        self.assertEqual(words[64:68], [37, 0, 37, 0])
        self.assertEqual(stats["lookalike_count"], 11)
        self.assertEqual(stats["payload_code_units"], 30)

    def test_plain_payloads_preserve_layout_and_true_occurrences(self):
        options = dict(class_count=2, methods_per_class=3, matching_methods=1)
        full, expected, stats = bench.make_reference_dex(**options)
        plain, plain_expected, plain_stats = bench.make_reference_dex(**options, payload_lookalikes=False)
        self.assertEqual(len(full), len(plain))
        self.assertEqual(expected, plain_expected)
        self.assertEqual(stats["code_units"], plain_stats["code_units"])
        self.assertEqual(stats["lookalike_count"], 66)
        self.assertEqual(plain_stats["lookalike_count"], 24)
        _, words = first_code(plain)
        self.assertEqual(words[42:52], [0] * 10)
        self.assertEqual(words[54:56], [0, 0])
        self.assertEqual(words[60:64], [0, 0, 1, 0])

    def test_many_nonmatching_methods_keep_pool_matches_and_exact_ownership(self):
        data, expected, stats = bench.make_reference_dex(class_count=4, methods_per_class=8,
                                                        matching_methods=2, references_per_method=3)
        self.assertEqual(uint(data, 88), 34)
        self.assertEqual(stats["defined_method_count"], 32)
        for kind, rows in expected.items():
            self.assertEqual(len(rows), 6)
            self.assertEqual(stats["matching_pool_entries"][kind], 1)
            self.assertEqual({row["method"] for row in rows}, {
                "Lbench/d000/C000000;->probe0000()V", "Lbench/d000/C000000;->probe0001()V"})
        class_table = uint(data, 100)
        for index in range(4):
            cursor = uint(data, class_table + index * 32 + 24)
            counts = []
            for _ in range(4):
                count, cursor = leb(data, cursor)
                counts.append(count)
            self.assertEqual(counts, [0, 0, 8, 0])
            method_ids = []
            previous = 0
            for _ in range(8):
                delta, cursor = leb(data, cursor)
                access, cursor = leb(data, cursor)
                code_at, cursor = leb(data, cursor)
                previous += delta
                method_ids.append(previous)
                self.assertEqual(access, 9)
                self.assertEqual(code_at % 4, 0)
            self.assertEqual(method_ids, sorted(set(method_ids)))

    def test_target_in_pool_without_real_sites_remains_empty(self):
        _, expected, stats = bench.make_reference_dex(class_count=1, methods_per_class=2, matching_methods=0)
        self.assertTrue(all(not rows for rows in expected.values()))
        self.assertTrue(all(count == 1 for count in stats["matching_pool_entries"].values()))

    def test_jumbo_uses_full_32_bit_index(self):
        data, expected, _ = bench.make_reference_dex(class_count=1, methods_per_class=1,
                                                    matching_methods=1, extra_strings=65536)
        row = expected["string"][0]
        self.assertGreater(row["target_index"], 65535)
        self.assertEqual(row["opcode"], "const-string/jumbo")
        _, words = first_code(data)
        self.assertEqual(words[11], 0x001B)
        self.assertEqual(words[12] | words[13] << 16, row["target_index"])

    def test_utf16_expectations_preserve_nul_supplementary_and_lone_surrogate(self):
        text = "A\0λ😀\ud800"
        _, expected, _ = bench.make_reference_dex(class_count=1, methods_per_class=1,
                                                 matching_methods=1, needle_text=text)
        row = expected["string"][0]
        self.assertEqual(row["target_utf16"], [65, 0, 955, 0xD83D, 0xDE00, 0xD800])
        self.assertIsNone(row["target"])
        self.assertEqual(bench.mutf8("\0\ud800"), b"\x02\xc0\x80\xed\xa0\x80\0")
        self.assertEqual(bench.json_string("😀"), "😀")

    def test_queries_have_exact_substring_and_target_owner_semantics(self):
        query = {"kind": "method", "text": "call", "exact": False, "owner": bench.NOISE}
        _, expected, _ = bench.make_reference_dex(class_count=1, methods_per_class=2, matching_methods=1, queries=[query])
        self.assertEqual(len(expected["method"]), 2)
        self.assertTrue(all(row["target"].startswith(bench.NOISE + "->") for row in expected["method"]))
        query["exact"] = True
        self.assertFalse(bench.matches(query, "method", bench.NOISE + "->call()V"))
        query["text"] = bench.NOISE + "->call()V"
        self.assertTrue(bench.matches(query, "method", query["text"]))
        self.assertFalse(bench.matches(query, "type", bench.NOISE))

    def test_manifest_multidex_selection_hashes_and_fresh_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "first"
            options = dict(queries=bench.default_queries(), class_count=1, methods_per_class=1,
                           matching_methods=1, selected_workloads=["dex", "multidex-deflated"], resource_bytes=20)
            manifest = bench.generate_workloads(root, **options)
            self.assertEqual([workload["name"] for workload in manifest["workloads"]], ["dex", "multidex-deflated"])
            for workload in manifest["workloads"]:
                self.assertEqual(bench.inventory.sha256_file(root / workload["input"]), workload["sha256"])
                for expectation in workload["expectations"].values():
                    self.assertEqual(bench.inventory.sha256_file(root / expectation["file"]), expectation["sha256"])
                    data = json.loads((root / expectation["file"]).read_text())
                    count = 2 if workload["name"].startswith("multidex") else 1
                    self.assertEqual(len(data["references"]), count * 2)
                    self.assertEqual(data["counts"]["dex_count"], count)
                    self.assertEqual(data["counts"]["scanned_method_count"], count)
                    self.assertEqual({row["dex_entry"] for row in data["references"]},
                                     {"classes.dex", "classes2.dex"} if count == 2 else {"classes.dex"})
            with zipfile.ZipFile(root / "multidex-deflated.apk") as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(len(archive.read("assets/unrelated.bin")), 20)
            previous = {path.name: path.read_bytes() for path in root.iterdir()}
            with self.assertRaises(FileExistsError):
                bench.generate_workloads(root, **options)
            self.assertEqual(previous, {path.name: path.read_bytes() for path in root.iterdir()})
            again = bench.generate_workloads(Path(temporary) / "second", **options)
            self.assertEqual([row["sha256"] for row in manifest["workloads"]], [row["sha256"] for row in again["workloads"]])


class ReferenceRunnerTests(unittest.TestCase):
    def setUp(self):
        _, expected, _ = bench.make_reference_dex(class_count=1, methods_per_class=1, matching_methods=1)
        self.rows = [{"dex_entry": "classes.dex", **row} for row in expected["string"]]

    def test_complete_occurrences_compare_pc_identity_utf16_and_multiplicity(self):
        bench.validate_output(json.dumps(report_for(list(reversed(self.rows)))).encode(), self.rows)
        changes = [self.rows[:1], self.rows + self.rows[:1]]
        for key, value in (("pc_code_units", 1), ("method", "Lother;->x()V"), ("opcode", "const-class"),
                           ("dex_entry", "classes2.dex"), ("target_utf16", [65])):
            changed = copy.deepcopy(self.rows)
            changed[0][key] = value
            changes.append(changed)
        for rows in changes:
            with self.subTest(rows=rows), self.assertRaises(RuntimeError):
                bench.validate_output(json.dumps(report_for(rows)).encode(), self.rows)

    def test_neverd_requires_complete_scan_and_truthful_counts(self):
        for extra in ({"code_scan_complete": False}, {"code_scan_complete": None}, {"reference_count": 999}, {"status": "error"}):
            with self.assertRaises(RuntimeError):
                bench.validate_output(json.dumps(report_for(self.rows, **extra)).encode(), self.rows)
        with self.assertRaisesRegex(RuntimeError, "count mismatch"):
            bench.validate_output(json.dumps(report_for(self.rows, class_count=7)).encode(), self.rows,
                                  expected_counts={"class_count": 1})
        partial = report_for(self.rows, code_scan_complete=False, validation_scope="candidate-query")
        observed = bench.validate_output(json.dumps(partial).encode(), self.rows, require_full_scan=False)
        self.assertFalse(observed["code_scan_complete"])
        self.assertEqual(observed["validation_scope"], "candidate-query")

    def test_surrogates_are_not_replaced_or_silently_normalized(self):
        row = dict(self.rows[0], target=None, target_utf16=[0xD800])
        bench.validate_output(json.dumps(report_for([row])).encode(), [row])
        bad = dict(row, target="�")
        with self.assertRaisesRegex(RuntimeError, "UTF-16"):
            bench.validate_output(json.dumps(report_for([bad])).encode(), [row])

    def test_process_rejection_timeout_and_validation(self):
        output = json.dumps(report_for(self.rows))
        command = [sys.executable, "-c", "import sys; print(sys.argv[1])", output]
        sample = bench.run_once(command, self.rows, timeout=5)
        self.assertGreater(sample["wall_seconds"], 0)
        with self.assertRaisesRegex(RuntimeError, "mismatch"):
            bench.run_once(command, [], timeout=5)
        with self.assertRaisesRegex(RuntimeError, "exited 5"):
            bench.run_once([sys.executable, "-c", "raise SystemExit(5)"], [], timeout=5)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            bench.run_once([sys.executable, "-c", "import time; time.sleep(10)"], [], timeout=0.05)

    def test_every_kind_and_repetition_validates_same_selected_workload(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "fixtures"
            manifest = bench.generate_workloads(root, queries=bench.default_queries(), class_count=1,
                                                methods_per_class=1, matching_methods=1, selected_workloads=["dex"])
            command = [sys.executable, "-c", "import json,sys; d=json.load(open(sys.argv[1])); "
                       "print(json.dumps(dict(d['counts'],status='success',code_scan_complete=True,references=d['references'])))",
                       str(root / "dex.{kind}.expected.json")]
            report = bench.benchmark(root, manifest, {"neverd": command, "peer": command},
                                     repetitions=2, warmups=1, timeout=5, measure_rss=False)
            self.assertEqual(len(report["results"]), 8)
            self.assertEqual({row["kind"] for row in report["results"]}, set(bench.KINDS))
            self.assertTrue(all(row["workload"] == "dex" and len(row["samples"]) == 2 for row in report["results"]))
            (root / "classes.dex").write_bytes(b"changed")
            with self.assertRaisesRegex(RuntimeError, "fixture changed"):
                bench.benchmark(root, manifest, {"neverd": command}, repetitions=1, warmups=0,
                                timeout=5, measure_rss=False)

    def test_cli_generate_only_and_failed_run_do_not_publish_results(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            common = ["--class-count", "1", "--methods-per-class", "1", "--matching-methods", "1", "--workload", "dex"]
            generated = Path(temporary) / "generated"
            self.assertEqual(bench.main(["--output-dir", str(generated), *common, "--generate-only"]), 0)
            self.assertFalse((generated / "results.json").exists())
            self.assertEqual(bench.main(["--output-dir", str(generated), *common, "--generate-only"]), 1)
            failed = Path(temporary) / "failed"
            self.assertEqual(bench.main(["--output-dir", str(failed), *common, "--neverd", sys.executable]), 1)
            self.assertTrue((failed / "manifest.json").is_file())
            self.assertFalse((failed / "results.json").exists())


@unittest.skipUnless(os.environ.get("NEVERD_REFERENCE_TEST_BINARY"), "optional built CLI not requested")
class BuiltCLIReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.data, self.expected, _ = bench.make_reference_dex(
            class_count=1, methods_per_class=1, matching_methods=1)
        self.source = self.root / "classes.dex"
        self.source.write_bytes(self.data)

    def invoke(self, *options, source=None):
        return subprocess.run(
            [os.environ["NEVERD_REFERENCE_TEST_BINARY"], "mobile", str(source or self.source), *options],
            cwd=self.root, capture_output=True, timeout=20)

    def test_all_kinds_raw_and_multidex_with_and_without_payload_lookalikes(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stdout(io.StringIO()):
            for profile in ("lookalikes", "plain"):
                argv = ["--output-dir", str(Path(temporary) / profile), "--neverd", os.environ["NEVERD_REFERENCE_TEST_BINARY"],
                        "--class-count", "2", "--methods-per-class", "3", "--matching-methods", "1",
                        "--workload", "dex", "--workload", "multidex-deflated", "--exact",
                        "--repetitions", "1", "--warmups", "0"]
                if profile == "plain":
                    argv.append("--no-payload-lookalikes")
                self.assertEqual(bench.main(argv), 0)

    def test_json_lines_relative_output_and_existing_file_preservation(self):
        query = ["--find-refs", "string", "--query", "benchmark needle", "--exact"]
        result = self.invoke(*query)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        expected = [{"dex_entry": "classes.dex", **row} for row in self.expected["string"]]
        self.assertEqual(bench.canonical_rows(rows), bench.canonical_rows(expected))
        saved = self.invoke(*query, "-o", "refs.jsonl")
        self.assertEqual(saved.returncode, 0, saved.stderr)
        self.assertEqual(saved.stdout, b"")
        self.assertEqual((self.root / "refs.jsonl").read_bytes(), result.stdout)
        again = self.invoke(*query, "-o", "refs.jsonl", "--json")
        self.assertNotEqual(again.returncode, 0)
        self.assertEqual(json.loads(again.stdout)["status"], "error")
        self.assertEqual((self.root / "refs.jsonl").read_bytes(), result.stdout)

    def test_option_conflicts_and_required_query_fail_before_publication(self):
        valid = ["--find-refs", "string", "--query", "needle"]
        cases = [["--query", "needle"], ["--exact"], ["--owner", "Lbench/Needle;"],
                 ["--find-refs", "string"], ["--find-refs", "string", "--query", ""],
                 ["--find-refs", "bogus", "--query", "needle"],
                 [*valid, "--owner", "Lbench/Needle;"],
                 [*valid, "--list-classes"], [*valid, "--class-prefix", "bench"],
                 [*valid, "--metadata-only"], [*valid, "--arch", "arm64"],
                 [*valid, "--jadx", "unused"], [*valid, "--platform", "ios"]]
        for options in cases:
            with self.subTest(options=options):
                result = self.invoke(*options, "--json", "-o", "must-not-exist")
                self.assertNotEqual(result.returncode, 0)
                report = json.loads(result.stdout)
                self.assertEqual(report["status"], "error")
                self.assertNotIn("references", report)
                self.assertFalse((self.root / "must-not-exist").exists())

    def test_multidex_failures_and_aggregate_limits_publish_no_partial_rows(self):
        other, _, _ = bench.make_reference_dex(
            class_count=1, methods_per_class=1, matching_methods=1, dex_index=1)
        corrupt = bytearray(other)
        corrupt[-1] ^= 1
        cases = [("duplicate", self.data, []), ("malformed", corrupt, []),
                 ("site-limit", other, ["--max-files", "3"])]
        for label, second, extra in cases:
            path = self.root / (label + ".apk")
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("classes.dex", self.data)
                archive.writestr("classes2.dex", second)
            with self.subTest(label=label):
                result = self.invoke("--find-refs", "string", "--query", "needle", "--json",
                                     "-o", "must-not-exist", *extra, source=path)
                self.assertNotEqual(result.returncode, 0)
                report = json.loads(result.stdout)
                self.assertEqual(report["status"], "error")
                self.assertNotIn("references", report)
                self.assertFalse((self.root / "must-not-exist").exists())
        too_small = self.invoke("--find-refs", "string", "--query", "needle", "--json",
                                "--max-bytes", str(len(self.data) - 1))
        self.assertNotEqual(too_small.returncode, 0)
        self.assertEqual(json.loads(too_small.stdout)["status"], "error")

        # Empty results must not hide duplicate definitions or aggregate method
        # limits. Each individual DEX stays below the three-method limit.
        path = self.root / "method-limit.apk"
        with zipfile.ZipFile(path, "w") as archive:
            for index in range(2):
                data, _, _ = bench.make_reference_dex(
                    class_count=1, methods_per_class=2, matching_methods=0, dex_index=index)
                archive.writestr("classes.dex" if index == 0 else "classes2.dex", data)
        limited = self.invoke("--find-refs", "string", "--query", "absent", "--json",
                              "--max-files", "3", source=path)
        self.assertNotEqual(limited.returncode, 0)
        self.assertIn("defined method count", json.loads(limited.stdout)["error"])
        duplicate = self.invoke("--find-refs", "string", "--query", "absent", "--json",
                                source=self.root / "duplicate.apk")
        self.assertNotEqual(duplicate.returncode, 0)
        self.assertIn("duplicate Android class", json.loads(duplicate.stdout)["error"])

    def test_query_metadata_remains_charged_across_method_bodies(self):
        data, _, _ = bench.make_reference_dex(
            class_count=1, methods_per_class=8192, matching_methods=0,
            payload_lookalikes=False)
        self.source.write_bytes(data)
        query = ["--find-refs", "string", "--query", "absent", "--json",
                 "--max-files", "100000"]
        # Compact query indexes fit this complete scan in 4 MB; the 2 MB
        # bound still allows the input and rejects retained metadata growth.
        limited = self.invoke(*query, "--max-bytes", "2000000",
                              "-o", "must-not-exist")
        self.assertNotEqual(limited.returncode, 0)
        report = json.loads(limited.stdout)
        self.assertEqual(report["status"], "error")
        self.assertIn("reference query storage exceeds byte limit", report["error"])
        self.assertNotIn("references", report)
        self.assertFalse((self.root / "must-not-exist").exists())
        complete = self.invoke(*query, "--max-bytes", "4000000")
        self.assertEqual(complete.returncode, 0, complete.stdout)
        report = json.loads(complete.stdout)
        self.assertEqual(report["status"], "success")
        self.assertTrue(report["code_scan_complete"])
        for key in ("defined_method_count", "scanned_method_count", "scanned_code_item_count"):
            self.assertEqual(report[key], 8192)
        self.assertEqual(report["matching_pool_entries"], 0)
        self.assertEqual(report["reference_count"], 0)
        self.assertEqual(report["references"], [])

    def test_literal_utf16_and_real_jumbo_indices_are_lossless(self):
        text = "A\0λ😀\ud800"
        query = {"kind": "string", "text": "A", "exact": False, "owner": None}
        data, expected, _ = bench.make_reference_dex(
            class_count=1, methods_per_class=1, matching_methods=1,
            needle_text=text, extra_strings=65536, queries=[query])
        self.source.write_bytes(data)
        result = self.invoke("--find-refs", "string", "--query", "A", "--json")
        self.assertEqual(result.returncode, 0, result.stdout)
        rows = [{"dex_entry": "classes.dex", **row} for row in expected["string"]]
        bench.validate_output(result.stdout, rows)
        report = json.loads(result.stdout)
        self.assertTrue(all(row["target"] is None for row in report["references"]))
        self.assertTrue(all(row["target_index"] > 65535 for row in report["references"]))


if __name__ == "__main__":
    unittest.main()
