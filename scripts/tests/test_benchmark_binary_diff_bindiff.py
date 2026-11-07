"""External exporter admission and process-failure contracts without licensed tools."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from scripts import benchmark_binary_diff as bench
from scripts import benchmark_binary_diff_bindiff as runner
from scripts import benchmark_binary_diff_external as external


class ExportAdmissionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source, self.exported = self.root / "input.elf", self.root / "input.BinExport"
        self.source.write_bytes(b"owned native input")
        self.exported.write_bytes(b"exported graph")
        self.receipt = self.root / "export.json"
        self.value = {"schema": runner.EXPORT_SCHEMA, "complete": True,
                      "input_sha256": bench.digest(self.source), "ida_version": "9.4",
                      "export_sha256": bench.digest(self.exported)}

    def check(self):
        self.receipt.write_text(json.dumps(self.value))
        return runner.checked_export(self.receipt, self.exported, self.source)

    def test_complete_native_hash_bound_export_is_admitted(self):
        self.assertEqual(self.check(), self.value)

    def test_incomplete_and_wrong_input_exports_are_refused(self):
        for change in ({"complete": False}, {"input_sha256": "0" * 64},
                       {"ida_version": ""}, {"schema": "unknown"}):
            original = dict(self.value)
            self.value.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.check()
            self.value = original

    def test_stale_and_empty_exports_are_refused(self):
        for content in (b"changed graph", b""):
            self.exported.write_bytes(content)
            with self.subTest(content=content), self.assertRaises(ValueError):
                self.check()


class ProcessContractTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_process_failure_retains_exit_and_output(self):
        command = [sys.executable, "-c", "print('analysis failed'); raise SystemExit(7)"]
        result = external.stage(self.root, "export", command, dict(os.environ), 30)
        self.assertEqual(result["exit"], 7)
        self.assertIsNone(result["error"])
        self.assertIn("analysis failed", (self.root / "export.log").read_text())
        self.assertEqual(bench.load(self.root / "export.json"), result)

    def test_nonlinux_process_does_not_receive_gnu_time_options(self):
        command = [sys.executable, "-c", "print('analysis failed'); raise SystemExit(7)"]
        with mock.patch.object(external.sys, "platform", "darwin"), \
                mock.patch.object(external.subprocess, "Popen", wraps=subprocess.Popen) as start:
            result = external.stage(self.root, "export", command, dict(os.environ), 30)
        self.assertEqual(start.call_args.args[0], command)
        self.assertEqual(result["exit"], 7)
        self.assertIsNone(result["error"])
        self.assertIsNone(result["resources"])
        self.assertIn("analysis failed", (self.root / "export.log").read_text())
        self.assertEqual(bench.load(self.root / "export.json"), result)

    def test_missing_tool_retains_failure_receipt(self):
        result = external.stage(self.root, "missing", [str(self.root / "missing-tool")], {}, 1)
        self.assertIsNone(result["exit"])
        self.assertTrue(result["error"])
        self.assertEqual(bench.load(self.root / "missing.json"), result)

    def test_tool_archive_survives_source_updates_and_same_names(self):
        tools = [self.root / "one" / "tool", self.root / "two" / "tool"]
        for index, path in enumerate(tools):
            path.parent.mkdir()
            path.write_text("version " + str(index))
        copies = external.archive_tools(tools, self.root / "frozen tools")
        records = [bench.artifact(copy, self.root) for copy in copies]
        for path in tools:
            path.write_text("updated installation")
        self.assertEqual([bench.checked_artifact(record, self.root).read_text()
                          for record in records], ["version 0", "version 1"])

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux process-group contract")
    def test_cancellation_retains_receipt_and_stops_the_pipeline(self):
        child = mock.Mock(pid=12345)
        child.wait.side_effect = [KeyboardInterrupt, -9]
        with mock.patch.object(external.subprocess, "Popen", return_value=child), \
                mock.patch.object(external.os, "killpg") as kill:
            with self.assertRaises(KeyboardInterrupt):
                external.stage(self.root, "cancelled", ["tool"], {}, 1)
        kill.assert_called_once_with(child.pid, external.signal.SIGKILL)
        result = bench.load(self.root / "cancelled.json")
        self.assertIsNone(result["exit"])
        self.assertEqual(result["error"], "cancelled")

    @unittest.skipUnless(sys.platform.startswith("linux"), "Linux process-group contract")
    def test_timeout_terminates_only_the_owned_process_group(self):
        child = mock.Mock(pid=12345)
        child.wait.side_effect = [subprocess.TimeoutExpired(["tool"], 1), -9]
        with mock.patch.object(external.subprocess, "Popen", return_value=child) as start, \
                mock.patch.object(external.os, "killpg") as kill:
            result = external.stage(self.root, "timeout", ["tool"], {}, 1)
        self.assertTrue(start.call_args.kwargs["start_new_session"])
        kill.assert_called_once_with(child.pid, external.signal.SIGKILL)
        self.assertEqual(child.wait.call_count, 2)
        self.assertIsNone(result["exit"])
        self.assertEqual(result["error"], "timeout")


if __name__ == "__main__":
    unittest.main()
