import hashlib
import copy
import json
import itertools
from pathlib import Path
import signal
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from scripts.prepare_sbf_corpus import prepare
from scripts.run_sbf_corpus import execute, qualify, read_events, sample_process_group_usage, summarize
from scripts.sbf_corpus_common import load_manifest, verify_artifact
from scripts.summarize_sbf_corpus import comparable


class SbfCorpusTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.payload = b"synthetic, not an ELF\0"
        self.row = {"name": "fixture", "file": "fixture.so", "asset_id": 1,
                    "url": "https://github.com/example/corpus/releases/download/snapshot/fixture.so",
                    "size": len(self.payload), "sha256": hashlib.sha256(self.payload).hexdigest(),
                    "onchain_size": len(self.payload) + 3,
                    "onchain_sha256": hashlib.sha256(self.payload + b"\0\0\0").hexdigest(),
                    "loader": "loader-v2"}
        self.manifest = {"schema": 1, "repository": "example/corpus", "release_tag": "snapshot",
                         "profile": {"cluster": "mainnet-beta", "slot": None}, "artifacts": [self.row]}
        self.path = self.root / "manifest.json"

    def save(self):
        self.path.write_text(json.dumps(self.manifest))

    def test_missing_asset_does_not_fetch_without_opt_in(self):
        self.save()
        with patch("urllib.request.urlopen") as fetch:
            with self.assertRaises(FileNotFoundError):
                prepare(self.path, self.root / "cache")
            fetch.assert_not_called()

    def test_exact_bytes_and_reconstructed_padding_are_both_authenticated(self):
        path = self.root / "fixture.so"
        path.write_bytes(self.payload)
        verify_artifact(path, self.row)
        wrong = dict(self.row, onchain_sha256="0" * 64)
        with self.assertRaisesRegex(ValueError, "onchain"):
            verify_artifact(path, wrong)
        path.write_bytes(self.payload[:-1] + b"x")
        with self.assertRaisesRegex(ValueError, "mismatch"):
            verify_artifact(path, self.row)

    def test_stale_single_program_url_is_rejected(self):
        self.row["url"] = self.row["url"].replace("/snapshot/", "/old-program/")
        self.save()
        with self.assertRaisesRegex(ValueError, "pinned snapshot"):
            load_manifest(self.path)

    def test_traversal_duplicate_and_bad_sizes_are_rejected(self):
        for change in ({"file": "../fixture.so"}, {"name": "../escape"}, {"name": ".."},
                       {"size": -1}, {"size": True}, {"onchain_size": 1}):
            with self.subTest(change=change):
                self.manifest["artifacts"] = [dict(self.row, **change)]
                self.save()
                with self.assertRaises(ValueError):
                    load_manifest(self.path)
        self.manifest["artifacts"] = [self.row, self.row]
        self.save()
        with self.assertRaisesRegex(ValueError, "duplicate"):
            load_manifest(self.path)

    def test_truncated_fetch_is_not_published(self):
        self.save()
        from io import BytesIO
        with patch("urllib.request.urlopen", return_value=BytesIO(self.payload[:-1])):
            with self.assertRaisesRegex(ValueError, "mismatch"):
                prepare(self.path, self.root / "cache", fetch=True)
        self.assertEqual(list((self.root / "cache").iterdir()), [])

    @unittest.skipUnless(sys.platform != "win32", "POSIX signal observation")
    def test_signal_retains_completed_decode_and_marks_active_stage(self):
        source = ("import json,os,signal\n"
                  "print(json.dumps({'stage':'decode','state':'passed','details':{'slots':2}}),flush=True)\n"
                  "print(json.dumps({'stage':'solana','state':'started'}),flush=True)\n"
                  "os.kill(os.getpid(),signal.SIGSEGV)\n")
        result = execute([sys.executable, "-c", source], self.root / "signal", 5, 1024 ** 3)
        self.assertEqual(result["signal"], signal.SIGSEGV)
        self.assertEqual(result["stages"]["decode"]["state"], "passed")
        self.assertEqual(result["stages"]["solana"]["state"], "crashed")
        self.assertEqual(result["stages"]["verification"]["state"], "not_run")

    def test_deadline_does_not_erase_prior_evidence(self):
        source = ("import json,time\n"
                  "print(json.dumps({'stage':'load','state':'passed'}),flush=True)\n"
                  "print(json.dumps({'stage':'highir','state':'started'}),flush=True)\n"
                  "time.sleep(60)\n")
        result = execute([sys.executable, "-c", source], self.root / "deadline", 0.3, 1024 ** 3)
        self.assertEqual(result["stop_reason"], "timeout")
        self.assertEqual(result["stages"]["load"]["state"], "passed")
        self.assertEqual(result["stages"]["highir"]["state"], "timeout")

    def test_group_memory_counts_compiler_children_without_other_requests(self):
        with patch("scripts.run_sbf_corpus.subprocess.run",
                   return_value=SimpleNamespace(stdout="1234 10 0:00.01 clang\n1234 20 0:00.30 clang child\n9999 9000 1:20.00 other\n")):
            self.assertEqual(sample_process_group_usage(1234)["rss_bytes"], 30 * 1024)
            self.assertIsNone(sample_process_group_usage(42))

    def test_cpu_observation_retains_only_live_owned_children(self):
        with patch("scripts.run_sbf_corpus.subprocess.run",
                   return_value=SimpleNamespace(stdout="1234 10 0:00.01 clang\n1234 20 0:12.30 compiler child\n9999 9000 1:20.00 other\n")):
            self.assertEqual(sample_process_group_usage(1234)["processes"], [
                {"rss_bytes": 10 * 1024, "cpu_time_text": "0:00.01", "executable": "clang"},
                {"rss_bytes": 20 * 1024, "cpu_time_text": "0:12.30", "executable": "compiler child"}])

    def test_macos_observer_selects_the_owned_group_before_collecting_usage(self):
        with patch("scripts.run_sbf_corpus.sys.platform", "darwin"), patch(
                "scripts.run_sbf_corpus.subprocess.run",
                return_value=SimpleNamespace(stdout="1234 20 0:12.30 compiler child\n")) as run:
            self.assertEqual(sample_process_group_usage(1234)["rss_bytes"], 20 * 1024)
            self.assertEqual(run.call_args.args[0],
                             ["ps", "-g", "1234", "-o", "pgid=,rss=,time=,comm="])

    @unittest.skipUnless(sys.platform != "win32", "POSIX process-group accounting")
    def test_child_memory_exhaustion_keeps_the_completed_prefix(self):
        child = "import time; data=bytearray(96*1024*1024); time.sleep(60)"
        source = ("import json,subprocess,sys,time\n"
                  "print(json.dumps({'stage':'load','state':'passed'}),flush=True)\n"
                  "print(json.dumps({'stage':'highir','state':'started'}),flush=True)\n"
                  f"subprocess.Popen([sys.executable,'-c',{child!r}])\n"
                  "time.sleep(60)\n")
        result = execute([sys.executable, "-c", source], self.root / "child-memory",
                         5, 80 * 1024 * 1024)
        self.assertEqual(result["stop_reason"], "memory_budget_exhausted")
        self.assertEqual(result["stages"]["load"]["state"], "passed")
        self.assertEqual(result["stages"]["highir"]["state"], "memory_budget_exhausted")

    def test_success_exit_without_terminal_event_cannot_pass(self):
        result = execute([sys.executable, "-c", "print('partial garbage')"],
                         self.root / "empty", 5, 1024 ** 3)
        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["stages"]["decode"]["state"], "not_run")

    def test_cancellation_keeps_the_completed_prefix(self):
        directory = self.root / "cancel"
        cancellation = threading.Event()
        def cancel_when_started():
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                path = directory / "events.jsonl"
                if path.exists() and "highir" in path.read_text():
                    break
                time.sleep(0.01)
            cancellation.set()
        thread = threading.Thread(target=cancel_when_started, daemon=True)
        thread.start()
        source = ("import json,time\n"
                  "print(json.dumps({'stage':'decode','state':'passed'}),flush=True)\n"
                  "print(json.dumps({'stage':'highir','state':'started'}),flush=True)\n"
                  "time.sleep(60)\n")
        result = execute([sys.executable, "-c", source], directory, 5, 1024 ** 3, cancellation)
        thread.join()
        self.assertEqual(result["stages"]["decode"]["state"], "passed")
        self.assertEqual(result["stages"]["highir"]["state"], "cancelled")

    def test_explicit_cancellation_is_not_overwritten_by_an_elapsed_deadline(self):
        cancellation = threading.Event()
        cancellation.set()
        clock = itertools.chain([0], itertools.repeat(2))
        with patch("scripts.run_sbf_corpus.time.monotonic", side_effect=lambda: next(clock)):
            result = execute([sys.executable, "-c", "import time; time.sleep(60)"],
                             self.root / "cancel-and-deadline", 1, 1024 ** 3, cancellation)
        self.assertEqual(result["stop_reason"], "cancelled")
        self.assertEqual(result["process_state"], "cancelled")

    def test_report_budget_keeps_already_flushed_events(self):
        path = self.root / "events.jsonl"
        prefix = json.dumps({"stage": "decode", "state": "passed"}) + "\n"
        path.write_text(prefix + "x" * 1000)
        with patch("scripts.run_sbf_corpus.LIMITS", SimpleNamespace(max_report_bytes=len(prefix) + 5)):
            events = read_events(path)
        self.assertEqual(events, [{"stage": "decode", "state": "passed"}])

    def test_wrong_hash_never_starts_worker(self):
        cache = self.root / "cache"
        cache.mkdir()
        (cache / "fixture.so").write_bytes(b"wrong")
        with patch("scripts.run_sbf_corpus.execute") as run:
            result = qualify(self.row, self.manifest, Path("missing-worker"), cache,
                             self.root / "results", ["analysis"], 1, 1024 ** 3)
            run.assert_not_called()
        self.assertEqual(result["integrity"], "failed")
        self.assertTrue((self.root / "results/fixture/result.json").exists())

    def test_export_success_does_not_count_as_native_compilation_success(self):
        rows = [{"integrity": "passed", "requests": {
            "c": {"process_state": "passed", "stages": {"c": {"state": "passed"}},
                  "compilation": {"state": "timeout"}},
            "rust": {"process_state": "passed", "stages": {"rust": {"state": "passed"}},
                     "compilation": {"state": "passed"}}}},
            {"integrity": "failed", "requests": {}}]
        summary = summarize(rows)
        self.assertEqual(summary["requests"]["c"]["c"], {"passed": 1, "not_run": 1})
        self.assertEqual(summary["native_compilation"]["c"], {"timeout": 1, "not_run": 1})
        self.assertEqual(summary["native_compilation"]["rust"], {"passed": 1, "not_run": 1})

    def test_comparison_rejects_changed_inputs_profiles_and_budgets(self):
        report = {"identity": {"manifest_sha256": "pinned", "expected_artifacts": 1,
                               "budgets": {"timeout": 60}},
                  "artifacts": [{"file": "fixture.so", "sha256": "input",
                                 "profile": {"loader": "loader-v2"}, "requests": {"analysis": {}}}]}
        comparable(report, copy.deepcopy(report))
        changed = copy.deepcopy(report)
        changed["identity"]["budgets"]["timeout"] = 120
        with self.assertRaisesRegex(ValueError, "budgets"):
            comparable(report, changed)
        changed = copy.deepcopy(report)
        changed["artifacts"][0]["profile"]["loader"] = "loader-v3"
        with self.assertRaisesRegex(ValueError, "profiles"):
            comparable(report, changed)
        changed = copy.deepcopy(report)
        changed["artifacts"] = []
        with self.assertRaises(ValueError):
            comparable(report, changed)


if __name__ == "__main__":
    unittest.main()
