"""Admission, direction and failure contracts for retained external DIFF evidence."""
from contextlib import closing
import json
import os
from pathlib import Path
import runpy
import shutil
import sqlite3
import sys
import tempfile
import threading
import unittest
from unittest import mock

from scripts import benchmark_binary_diff as bench
from scripts import benchmark_binary_diff_diaphora as diaphora
from scripts import benchmark_binary_diff_external as external
from scripts import benchmark_binary_diff_ghidra as ghidra


class NativeReceiptTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.inputs = [self.root / name for name in ("before.bin", "after.bin")]
        for index, path in enumerate(self.inputs):
            path.write_bytes(b"owned input " + bytes([index]))

    def write(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value))
        return path

    def native(self):
        result = {"schema": "neverd.benchmark.diaphora-match.v1", "complete": True,
                  "input_sha256": [bench.digest(path) for path in self.inputs], "diaphora_version": "3.4"}
        exports = []
        for side, source in zip(("primary", "secondary"), self.inputs):
            path = self.root / (side + ".sqlite")
            with closing(sqlite3.connect(path)) as database, database:
                database.execute("CREATE TABLE program (md5sum)")
                database.execute("INSERT INTO program VALUES (?)", (bytes.fromhex(bench.digest(source, "md5")),))
                database.execute("CREATE TABLE version (value)")
                database.execute("INSERT INTO version VALUES ('3.4')")
            declaration = {"schema": "neverd.benchmark.diaphora-export.v1", "complete": True,
                           "input_sha256": bench.digest(source), "export_sha256": bench.digest(path),
                           "ida_version": "9.4", "diaphora_version": "3.4", "database_path": str(path)}
            receipt = self.write(side + ".json", declaration)
            result[side] = {"database": bench.artifact(path, self.root),
                            "export_receipt": bench.artifact(receipt, self.root)}
            exports.append(path)
        path = self.root / "matches.diaphora"
        with closing(sqlite3.connect(path)) as database, database:
            database.execute("CREATE TABLE config (main_db, diff_db, version)")
            database.execute("INSERT INTO config VALUES (?, ?, '3.4')", tuple(map(str, exports)))
            database.execute("CREATE TABLE results (type, address, address2)")
            database.executemany("INSERT INTO results VALUES (?, ?, ?)",
                                 [("best", "10", "20"), ("partial", "30", "40"),
                                  ("unreliable", "50", "60"), ("multimatch", "70", "80")])
        result["result"] = bench.artifact(path, self.root)
        return result

    def read_native(self, value):
        return bench.read_pairs("diaphora-native", self.write("match.json", value), self.inputs)

    def test_native_diaphora_keeps_only_selected_function_pairs(self):
        self.assertEqual(self.read_native(self.native()), [(0x10, 0x20), (0x30, 0x40)])

    def test_diaphora_rejects_failed_match_and_reversed_inputs(self):
        value = self.native()
        for change in ({"complete": False}, {"schema": "unknown"},
                       {"input_sha256": list(reversed(value["input_sha256"]))},
                       {"worker_errors": [{"thread": "Loop count", "error": "database or disk is full"}]},
                       {"diaphora_version": "unknown"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.read_native(value | change)

    def test_diaphora_hexadecimal_native_md5_is_also_admitted(self):
        value = self.native()
        for side, source in zip(("primary", "secondary"), self.inputs):
            record = value[side]
            path = self.root / record["database"]["path"]
            with closing(sqlite3.connect(path)) as database, database:
                database.execute("UPDATE program SET md5sum=?", (bench.digest(source, "md5").upper(),))
            declaration = self.root / record["export_receipt"]["path"]
            content = bench.load(declaration)
            content["export_sha256"] = bench.digest(path)
            declaration.write_text(json.dumps(content))
            record.update(database=bench.artifact(path, self.root), export_receipt=bench.artifact(declaration, self.root))
        self.assertEqual(self.read_native(value), [(16, 32), (48, 64)])

    def test_diaphora_native_direction_survives_resealed_result_hash(self):
        value = self.native()
        path = self.root / value["result"]["path"]
        with closing(sqlite3.connect(path)) as database, database:
            database.execute("UPDATE config SET main_db=diff_db, diff_db=main_db")
        value["result"] = bench.artifact(path, self.root)
        with self.assertRaisesRegex(ValueError, "direction"):
            self.read_native(value)

    def test_diaphora_native_export_identity_survives_resealed_hashes(self):
        value = self.native()
        record = value["primary"]
        path = self.root / record["database"]["path"]
        with closing(sqlite3.connect(path)) as database, database:
            database.execute("UPDATE program SET md5sum=?", (bench.digest(self.inputs[1], "md5"),))
        declaration = self.root / record["export_receipt"]["path"]
        content = bench.load(declaration)
        content["export_sha256"] = bench.digest(path)
        declaration.write_text(json.dumps(content))
        record.update(database=bench.artifact(path, self.root), export_receipt=bench.artifact(declaration, self.root))
        with self.assertRaisesRegex(ValueError, "native program identity"):
            self.read_native(value)

    def test_diaphora_rejects_failed_export_even_with_valid_native_metadata(self):
        value = self.native()
        record = value["primary"]
        path = self.root / record["export_receipt"]["path"]
        content = bench.load(path)
        content["complete"] = False
        path.write_text(json.dumps(content))
        record["export_receipt"] = bench.artifact(path, self.root)
        with self.assertRaisesRegex(ValueError, "export did not complete"):
            self.read_native(value)

    def test_diaphora_rejects_modified_export(self):
        value = self.native()
        (self.root / value["primary"]["database"]["path"]).write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            self.read_native(value)

    def test_diaphora_retained_evidence_can_move_without_live_original_databases(self):
        value = self.native()
        receipt = self.write("match.json", value)
        moved = self.root / "moved evidence"
        moved.mkdir()
        for path in list(self.root.iterdir()):
            if path.is_file():
                shutil.move(path, moved / path.name)
        self.assertEqual(bench.read_pairs("diaphora-native", moved / receipt.name,
                                         [moved / p.name for p in self.inputs]), [(16, 32), (48, 64)])

    def test_ghidra_analysis_requires_completion_identity_and_program(self):
        value = {"schema": "neverd.benchmark.ghidra-analysis.v1", "complete": True,
                 "analysis_timed_out": False, "input_sha256": bench.digest(self.inputs[0]),
                 "program": "before.bin", "ghidra_version": "12.1.3"}
        self.assertEqual(ghidra.checked_analysis(self.write("analysis.json", value),
                                                self.inputs[0], "before.bin"), value)
        for change in ({"complete": False}, {"analysis_timed_out": True}, {"ghidra_version": ""},
                       {"input_sha256": bench.digest(self.inputs[1])}, {"program": "after.bin"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                ghidra.checked_analysis(self.write("analysis.json", value | change), self.inputs[0], "before.bin")

    def test_ghidra_requires_completed_task_before_export_is_admitted(self):
        value = {"schema": "neverd.benchmark.ghidra-task.v1", "complete": True,
                 "input_sha256": [bench.digest(path) for path in self.inputs], "ghidra_version": "12.1.3",
                 "profile": ghidra.PROFILE, "session": "matches",
                 "status": "Auto Version Tracking Command completed successfully"}
        self.assertEqual(ghidra.checked_task(self.write("task.json", value), self.inputs, "12.1.3"), value)
        for change in ({"complete": False}, {"input_sha256": list(reversed(value["input_sha256"]))},
                       {"status": "cancelled"}, {"status": None}, {"profile": "different"}, {"ghidra_version": "other"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                ghidra.checked_task(self.write("task.json", value | change), self.inputs, "12.1.3")

    def test_ambient_diaphora_settings_cannot_change_selected_profile(self):
        with mock.patch.dict(os.environ, {"DIAPHORA_PROJECT_SCRIPT": "unexpected.py", "DIAPHORA_CPU_COUNT": "40",
                                          "PYTHONPATH": "unexpected", "PYTHONHOME": "unexpected"}):
            env = diaphora.environment([self.root], 2)
        self.assertNotIn("DIAPHORA_PROJECT_SCRIPT", env)
        self.assertNotIn("PYTHONHOME", env)
        self.assertEqual(env["DIAPHORA_CPU_COUNT"], "2")
        self.assertEqual(env["PYTHONPATH"], str(self.root))

    def test_scored_mismatch_and_incomplete_run_have_distinct_exit_codes(self):
        for expected, cases in ((0, [{"metrics": {"exact_mapping": True}}]),
                                (1, [{"metrics": {"exact_mapping": False}}]),
                                (2, [{"error": "analysis timed out"}])):
            folder = self.root / str(expected)
            folder.mkdir()
            self.assertEqual(external.finish(folder, cases), expected)
            self.assertEqual(bench.load(folder / "results.json")["cases"], cases)

    def test_diaphora_export_api_failure_is_never_reported_complete(self):
        script = Path(__file__).resolve().parents[2] / "tools/neverd-bench/run_diaphora.py"
        output = self.root / "failed.sqlite"
        exporter = mock.Mock()
        exporter.export.return_value = False
        fake = {"diaphora": mock.Mock(VERSION_VALUE="3.4"),
                "diaphora_ida": mock.Mock(CIDABinDiff=mock.Mock(return_value=exporter)),
                "ida_auto": mock.Mock(auto_wait=mock.Mock(return_value=True)),
                "ida_hexrays": mock.Mock(), "ida_kernwin": mock.Mock(get_kernel_version=lambda: "9.4"),
                "ida_nalt": mock.Mock(retrieve_input_file_sha256=lambda: bytes.fromhex(bench.digest(self.inputs[0])), get_imagebase=lambda: 0),
                "ida_pro": mock.Mock(qexit=mock.Mock(side_effect=SystemExit(1)))}
        env = {"NEVERD_BENCH_DIAPHORA_MODE": "export", "NEVERD_BENCH_DIAPHORA_OUTPUT": str(output),
               "NEVERD_BENCH_DIAPHORA_SOURCE": str(self.root),
               "NEVERD_BENCH_DIAPHORA_PROFILE": json.dumps({"decompiler": False, "microcode": False,
                                                          "include_library_thunks": True})}
        with mock.patch.dict(sys.modules, fake), mock.patch.dict(os.environ, env), \
                mock.patch.object(sys, "path", list(sys.path)), self.assertRaises(SystemExit):
            runpy.run_path(str(script))
        receipt = bench.load(Path(str(output) + ".json"))
        self.assertFalse(receipt["complete"])
        self.assertIn("export did not complete", receipt["error"])
        fake["ida_pro"].qexit.assert_called_once_with(1)

    def test_diaphora_false_match_api_return_cannot_save_a_success_result(self):
        self.native()
        script = Path(__file__).resolve().parents[2] / "tools/neverd-bench/run_diaphora.py"
        for side in ("primary", "secondary"):
            shutil.copyfile(self.root / (side + ".json"), self.root / (side + ".sqlite.json"))
        matcher = mock.Mock()
        matcher.diff.return_value = False
        fake = mock.Mock(VERSION_VALUE="3.4", CBinDiff=mock.Mock(return_value=matcher))
        output = self.root / "failed.diaphora"
        env = {"NEVERD_BENCH_DIAPHORA_MODE": "match", "NEVERD_BENCH_DIAPHORA_OUTPUT": str(output),
               "NEVERD_BENCH_DIAPHORA_SOURCE": str(self.root),
               "NEVERD_BENCH_DIAPHORA_PRIMARY": str(self.root / "primary.sqlite"),
               "NEVERD_BENCH_DIAPHORA_SECONDARY": str(self.root / "secondary.sqlite")}
        with mock.patch.dict(sys.modules, {"diaphora": fake}), mock.patch.dict(os.environ, env), \
                mock.patch.object(sys, "path", list(sys.path)), self.assertRaises(SystemExit) as stop:
            runpy.run_path(str(script))
        self.assertEqual(stop.exception.code, 1)
        self.assertFalse(bench.load(Path(str(output) + ".json"))["complete"])
        matcher.save_results.assert_not_called()
        matcher.db_close.assert_called_once()

    def test_assembly_profile_does_not_require_the_decompiler_module(self):
        script = Path(__file__).resolve().parents[2] / "tools/neverd-bench/run_diaphora.py"
        output = self.root / "assembly.sqlite"
        exporter = mock.Mock()
        def export():
            output.write_bytes(b"completed export")
            return True
        exporter.export.side_effect = export
        fake = {"diaphora": mock.Mock(VERSION_VALUE="3.4"),
                "diaphora_ida": mock.Mock(CIDABinDiff=mock.Mock(return_value=exporter)),
                "ida_auto": mock.Mock(auto_wait=lambda: True), "ida_hexrays": None,
                "ida_kernwin": mock.Mock(get_kernel_version=lambda: "9.4", user_cancelled=lambda: False),
                "ida_nalt": mock.Mock(retrieve_input_file_sha256=lambda: bytes.fromhex(bench.digest(self.inputs[0])), get_imagebase=lambda: 0),
                "ida_pro": mock.Mock(qexit=mock.Mock(side_effect=SystemExit(0)))}
        env = {"NEVERD_BENCH_DIAPHORA_MODE": "export", "NEVERD_BENCH_DIAPHORA_OUTPUT": str(output),
               "NEVERD_BENCH_DIAPHORA_SOURCE": str(self.root),
               "NEVERD_BENCH_DIAPHORA_PROFILE": json.dumps({"decompiler": False, "microcode": False,
                                                          "include_library_thunks": True})}
        with mock.patch.dict(sys.modules, fake), mock.patch.dict(os.environ, env), \
                mock.patch.object(sys, "path", list(sys.path)), self.assertRaises(SystemExit) as stop:
            runpy.run_path(str(script))
        self.assertEqual(stop.exception.code, 0)
        self.assertTrue(bench.load(Path(str(output) + ".json"))["complete"])
        self.assertFalse(exporter.use_decompiler)
        self.assertFalse(exporter.export_microcode)

    def test_diaphora_worker_failure_cannot_save_a_success_result(self):
        self.native()
        script = Path(__file__).resolve().parents[2] / "tools/neverd-bench/run_diaphora.py"
        for side in ("primary", "secondary"):
            shutil.copyfile(self.root / (side + ".json"), self.root / (side + ".sqlite.json"))
        for failed in (True, False):
            with self.subTest(failed=failed):
                output = self.root / ("thread-failed.diaphora" if failed else "thread-ok.diaphora")
                matcher = mock.Mock()
                def worker():
                    if failed:
                        raise sqlite3.OperationalError("database or disk is full")
                def compare(_):
                    thread = threading.Thread(target=worker, name="Loop count")
                    thread.start()
                    thread.join()
                    return True
                matcher.diff.side_effect = compare
                matcher.save_results.side_effect = lambda path: Path(path).write_bytes(b"complete result")
                fake = mock.Mock(VERSION_VALUE="3.4", CBinDiff=mock.Mock(return_value=matcher))
                env = {"NEVERD_BENCH_DIAPHORA_MODE": "match", "NEVERD_BENCH_DIAPHORA_OUTPUT": str(output),
                       "NEVERD_BENCH_DIAPHORA_SOURCE": str(self.root),
                       "NEVERD_BENCH_DIAPHORA_PRIMARY": str(self.root / "primary.sqlite"),
                       "NEVERD_BENCH_DIAPHORA_SECONDARY": str(self.root / "secondary.sqlite")}
                original_hook = threading.excepthook
                with mock.patch.dict(sys.modules, {"diaphora": fake}), mock.patch.dict(os.environ, env), \
                        mock.patch.object(sys, "path", list(sys.path)), self.assertRaises(SystemExit) as stop:
                    runpy.run_path(str(script))
                self.assertIs(threading.excepthook, original_hook)
                self.assertEqual(stop.exception.code, 1 if failed else 0)
                receipt = bench.load(Path(str(output) + ".json"))
                self.assertEqual(receipt["complete"], not failed)
                matcher.db_close.assert_called_once()
                if failed:
                    matcher.save_results.assert_not_called()
                    self.assertFalse(output.exists())
                    self.assertEqual(receipt["worker_errors"][0]["thread"], "Loop count")
                    self.assertIn("database or disk is full", receipt["worker_errors"][0]["error"])
                    self.assertIn("worker failed", receipt["error"])
                else:
                    matcher.save_results.assert_called_once_with(str(output))
                    self.assertEqual(receipt["worker_errors"], [])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux external runner")
    def test_zero_exit_without_receipt_and_cancellation_remain_incomplete(self):
        root, java = self.root / "ghidra", self.root / "jdk"
        for path in (root / "Ghidra/application.properties", root / "Ghidra/Features/Base/lib/Base.jar",
                     root / "Ghidra/Features/VersionTracking/lib/VersionTracking.jar",
                     java / "bin/java", java / "release"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"test runtime")
        launcher = root / "support/analyzeHeadless"
        launcher.parent.mkdir()
        launcher.write_text("#!/bin/sh\nexit 0\n")
        launcher.chmod(0o700)
        case = {"id": "missing-receipt", "category": "fixture", "truth_complete": True,
                "truth": [{"id": "function", "primary": "0x10", "secondary": "0x20"}],
                "primary": bench.artifact(self.inputs[0], self.root),
                "secondary": bench.artifact(self.inputs[1], self.root)}
        corpus = self.write("corpus.json", {"schema": bench.CORPUS_SCHEMA, "cases": [case]})
        output = self.root / "result"
        status = ghidra.main(["--corpus", str(corpus), "--ghidra", str(root), "--java-home", str(java),
                              "--output-dir", str(output)])
        self.assertEqual(status, 2)
        record = bench.load(output / "missing-receipt/run.json")
        self.assertFalse(record["completed"])
        self.assertEqual(record["stages"][0]["exit"], 0)
        self.assertIn("before-analysis.json", record["error"])
        output = self.root / "cancelled"
        with mock.patch.object(external, "stage", side_effect=KeyboardInterrupt) as start:
            status = ghidra.main(["--corpus", str(corpus), "--ghidra", str(root), "--java-home", str(java),
                                  "--output-dir", str(output)])
        self.assertEqual(status, 130)
        self.assertEqual(start.call_count, 1)
        record = bench.load(output / "missing-receipt/run.json")
        self.assertFalse(record["completed"])
        self.assertEqual(record["error"], "cancelled")
        self.assertIn("error", bench.load(output / "results.json")["cases"][0])


if __name__ == "__main__":
    unittest.main()
