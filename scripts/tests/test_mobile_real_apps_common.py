"""CI guards for command lifetime and durable real-application evidence.

All process, clock, and signal boundaries are mocked. No test here launches
an application, SDK tool, compiler, or child process.
"""
from __future__ import annotations

import hashlib
import itertools
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts import mobile_real_apps_common as common


class ProcessDouble:
    """Write real evidence files while replacing only OS process behavior."""

    def __init__(self, *, code=0, running=False, stdout=b"complete stdout\n",
                 stderr=b"complete stderr\n", background=False, wait_error=None,
                 group_error=None, reap_error=None, grow_on_wait=b""):
        self.code, self.running = code, running
        self.stdout_bytes, self.stderr_bytes = stdout, stderr
        self.background, self.wait_error = background, wait_error
        self.group_error, self.reap_error = group_error, reap_error
        self.grow_on_wait = grow_on_wait
        self.pid, self.returncode, self.killed = 987654, None, False
        self.waits, self.signals, self.kwargs = [], [], None

    def start(self, argv, **kwargs):
        self.argv, self.kwargs = argv, kwargs
        for stream, data in ((kwargs["stdout"], self.stdout_bytes),
                             (kwargs["stderr"], self.stderr_bytes)):
            stream.buffer.write(data)
            stream.flush()
        return self

    def wait(self, timeout=None):
        self.waits.append(timeout)
        if self.killed:
            if self.reap_error:
                raise self.reap_error
            self.returncode = -9
            return self.returncode
        if self.wait_error:
            raise self.wait_error
        if self.grow_on_wait:
            self.kwargs["stdout"].buffer.write(self.grow_on_wait)
            self.kwargs["stdout"].flush()
            self.grow_on_wait = b""
        if self.running:
            raise subprocess.TimeoutExpired(self.argv, timeout)
        self.returncode = self.code
        return self.returncode

    def kill(self):
        self.killed = True

    def killpg(self, pid, sig):
        self.signals.append((pid, sig))
        if self.group_error:
            raise self.group_error
        if self.returncode is None or self.background:
            self.killed, self.background = True, False
            return
        raise ProcessLookupError("The isolated group has exited")


class EvidenceContextTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        ticks = itertools.count(100.0, 0.25)
        clock = patch.object(common.time, "monotonic", side_effect=lambda: next(ticks))
        clock.start()
        self.addCleanup(clock.stop)
        self.source = self.root / "source"
        self.source.mkdir()

    def context(self, *, work="evidence", timeout=3):
        return common.CaseContext(
            app={"source_commit": "a" * 40},
            variant={"id": "owned-release", "app": "owned", "platform": "android"},
            source=self.source, work=self.root / work, neverd=self.root / "neverd",
            timeout=timeout, consumer_commit="b" * 40, manifest_sha256="c" * 64)

    def invoke(self, ctx, process, **kwargs):
        with patch.object(common.subprocess, "Popen", side_effect=process.start), \
             patch.object(common.os, "killpg", side_effect=process.killpg, create=True):
            return ctx.command("owned-command", ["owned-tool", "literal argument"], **kwargs)

    def complete_stages(self, ctx):
        ctx.write_json("proof.json", {"independent": True})
        for name in common.STAGES:
            ctx.stage(name, "success", evidence=["proof.json"])

    def assert_saved_record(self, ctx, expected_status):
        result = common.load_json(ctx.work / "result.json")
        self.assertEqual(len(result["commands"]), 1)
        record = result["commands"][0]
        self.assertEqual(record["status"], expected_status)
        self.assertIn("ended_at", record)
        self.assertEqual(common.load_json(ctx.work / "commands" / (record["id"] + ".json")), record)
        return record

    def test_stale_evidence_is_rejected_without_overwriting_it(self):
        old = self.root / "evidence"
        old.mkdir()
        receipt = old / "result.json"
        receipt.write_text('{"old": true}\n', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "empty.*stale"):
            self.context()
        self.assertEqual(receipt.read_text(encoding="utf-8"), '{"old": true}\n')

    def test_nonzero_command_keeps_full_logs_and_failed_result(self):
        ctx = self.context()
        process = ProcessDouble(code=7, stdout=b"first\nlast\n", stderr=b"reason\n")
        with self.assertRaisesRegex(RuntimeError, "exited 7"):
            self.invoke(ctx, process)
        record = self.assert_saved_record(ctx, "failed")
        self.assertEqual(record["exitcode"], 7)
        self.assertEqual((ctx.work / record["stdout"]).read_bytes(), b"first\nlast\n")
        self.assertEqual((ctx.work / record["stderr"]).read_bytes(), b"reason\n")
        self.complete_stages(ctx)
        self.assertEqual(ctx.finish()["status"], "failed")

    def test_allow_failure_returns_diagnostics_without_greening_command(self):
        ctx = self.context()
        result = self.invoke(ctx, ProcessDouble(code=4), allow_failure=True)
        self.assertEqual(result.returncode, 4)
        self.assertEqual(result.stdout, "complete stdout\n")
        self.assert_saved_record(ctx, "failed")
        self.complete_stages(ctx)
        self.assertEqual(ctx.finish()["status"], "failed")

    @unittest.skipUnless(os.name == "posix", "Actions process-group checks target Linux/macOS")
    def test_timeout_kills_the_group_and_reaps_before_saving_logs(self):
        ctx = self.context()
        process = ProcessDouble(running=True)
        with self.assertRaisesRegex(RuntimeError, "timeout"):
            self.invoke(ctx, process)
        record = self.assert_saved_record(ctx, "timeout")
        self.assertEqual(record["exitcode"], -9)
        self.assertIn((process.pid, signal.SIGKILL), process.signals)
        self.assertTrue(process.killed)
        self.assertTrue(all(limit <= 1 for limit in process.waits[:-1]))
        self.assertEqual(process.waits[-1], 10)
        self.assertEqual((ctx.work / record["stderr"]).read_bytes(), process.stderr_bytes)

    @unittest.skipUnless(os.name == "posix", "Actions process-group checks target Linux/macOS")
    def test_successful_leader_with_background_writer_is_failed_and_stopped(self):
        ctx = self.context()
        process = ProcessDouble(background=True)
        with self.assertRaisesRegex(RuntimeError, "background process group"):
            self.invoke(ctx, process)
        record = self.assert_saved_record(ctx, "failed")
        self.assertEqual(record["exitcode"], 0)
        self.assertTrue(record["leftover_process_group"])
        self.assertFalse(process.background)
        self.complete_stages(ctx)
        self.assertEqual(ctx.finish()["status"], "failed")

    def test_live_output_limit_stops_a_still_running_command(self):
        ctx = self.context()
        process = ProcessDouble(running=True, stdout=b"start", stderr=b"",
                                grow_on_wait=b" more output beyond the bound")
        with patch.object(common, "MAX_COMMAND_OUTPUT", 10):
            with self.assertRaisesRegex(RuntimeError, "output-limit"):
                self.invoke(ctx, process)
        record = self.assert_saved_record(ctx, "output-limit")
        self.assertEqual(record["exitcode"], -9)
        self.assertEqual(process.waits, [1, 10])
        self.assertEqual((ctx.work / record["stdout"]).read_bytes(), b"start more output beyond the bound")
        self.assertTrue(process.killed)

    def test_fast_exit_cannot_bypass_output_limit(self):
        ctx = self.context()
        process = ProcessDouble(stdout=b"", stderr=b"", grow_on_wait=b"oversized")
        with patch.object(common, "MAX_COMMAND_OUTPUT", 2):
            with self.assertRaisesRegex(RuntimeError, "output-limit"):
                self.invoke(ctx, process)
        record = self.assert_saved_record(ctx, "output-limit")
        self.assertEqual(record["exitcode"], 0)
        self.assertEqual((ctx.work / record["stdout"]).read_bytes(), b"oversized")

    @unittest.skipUnless(os.name == "posix", "Actions process-group checks target Linux/macOS")
    def test_cleanup_and_reap_errors_preserve_the_original_timeout_record(self):
        ctx = self.context()
        process = ProcessDouble(running=True, group_error=PermissionError("group denied"),
                                reap_error=subprocess.TimeoutExpired("owned-tool", 10))
        with self.assertRaisesRegex(RuntimeError, "timeout"):
            self.invoke(ctx, process)
        record = self.assert_saved_record(ctx, "timeout")
        self.assertIn("group denied", record["cleanup_error"])
        self.assertIn("reap_error", record)
        self.assertTrue(process.killed)  # The leader-kill fallback was used.
        self.assertTrue((ctx.work / record["stdout"]).is_file())

    def test_spawn_error_still_has_terminal_command_and_empty_logs(self):
        ctx = self.context()
        with patch.object(common.subprocess, "Popen", side_effect=FileNotFoundError("tool missing")):
            with self.assertRaisesRegex(RuntimeError, "tool missing"):
                ctx.command("missing-command", ["missing-tool"])
        record = self.assert_saved_record(ctx, "failed")
        self.assertEqual((ctx.work / record["stdout"]).read_bytes(), b"")
        self.assertEqual((ctx.work / record["stderr"]).read_bytes(), b"")

    def test_wait_error_also_stops_the_process_before_persisting_failure(self):
        ctx = self.context()
        process = ProcessDouble(wait_error=OSError("wait failed"))
        with self.assertRaisesRegex(RuntimeError, "wait failed"):
            self.invoke(ctx, process)
        record = self.assert_saved_record(ctx, "failed")
        self.assertTrue(process.killed)
        self.assertEqual(record["exitcode"], -9)
        self.assertEqual((ctx.work / record["stdout"]).read_bytes(), process.stdout_bytes)

    def test_exhausted_case_deadline_never_starts_another_command(self):
        ctx = self.context()
        ctx.deadline = 0
        with patch.object(common.subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(TimeoutError, "deadline exhausted"):
                ctx.command("too-late", ["owned-tool"])
            spawn.assert_not_called()
        self.assertFalse(ctx.result["commands"])

    def test_missing_stage_never_becomes_a_success(self):
        ctx = self.context()
        self.invoke(ctx, ProcessDouble())
        ctx.stage("provenance", "success", evidence=["proof.json"])
        result = ctx.finish()
        self.assertEqual(result["status"], "incomplete")
        self.assertEqual(set(result["stages"]), set(common.STAGES))
        self.assertEqual(result["stages"]["behavior"]["status"], "incomplete")

    def test_artifact_hashes_bind_exact_bytes_and_source_provenance(self):
        ctx = self.context()
        self.invoke(ctx, ProcessDouble())
        self.complete_stages(ctx)
        evidence = ctx.work / "original.bin"
        content = b"original application bytes\x00\xff"
        evidence.write_bytes(content)
        result = ctx.finish()
        self.assertEqual(result["status"], "success")
        self.assertEqual(result["consumer_commit"], "b" * 40)
        self.assertEqual(result["source_commit"], "a" * 40)
        self.assertEqual(result["manifest_sha256"], "c" * 64)
        artifacts = {row["path"]: row for row in result["artifacts"]}
        self.assertEqual(artifacts["original.bin"]["sha256"], hashlib.sha256(content).hexdigest())
        self.assertEqual(artifacts["original.bin"]["size"], len(content))
        self.assertNotIn("result.json", artifacts)  # No recursive self-hash claim.
        self.assertEqual(common.load_json(ctx.work / "result.json"), result)

    def test_hash_failure_retains_other_evidence_and_a_failed_result(self):
        ctx = self.context()
        self.invoke(ctx, ProcessDouble())
        self.complete_stages(ctx)
        (ctx.work / "unreadable.bin").write_bytes(b"bytes")
        original_digest = common.digest
        def hash_or_fail(path):
            if path.name == "unreadable.bin":
                raise PermissionError("cannot read original")
            return original_digest(path)
        with patch.object(common, "digest", side_effect=hash_or_fail):
            result = ctx.finish()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(any("unreadable.bin" in error for error in result["failures"]))
        self.assertIn("proof.json", {row["path"] for row in result["artifacts"]})
        self.assertEqual(common.load_json(ctx.work / "result.json"), result)

    def test_unreadable_subtree_marks_result_failed_without_losing_other_hashes(self):
        ctx = self.context()
        self.invoke(ctx, ProcessDouble())
        self.complete_stages(ctx)
        original_walk = common.os.walk
        def walk_with_failure(path, **kwargs):
            kwargs["onerror"](PermissionError("missing subtree"))
            yield from original_walk(path, **kwargs)
        with patch.object(common.os, "walk", side_effect=walk_with_failure):
            result = ctx.finish()
        self.assertEqual(result["status"], "failed")
        self.assertTrue(any("missing subtree" in error for error in result["failures"]))
        self.assertIn("proof.json", {row["path"] for row in result["artifacts"]})
        self.assertEqual(common.load_json(ctx.work / "result.json"), result)

    def test_environment_keeps_sdk_settings_but_does_not_leak_credentials(self):
        ctx = self.context()
        process = ProcessDouble()
        with patch.dict(os.environ, {"GITHUB_TOKEN": "secret-fixture", "ANDROID_HOME": "/sdk"}):
            self.invoke(ctx, process, env={"CUSTOM_API_KEY": "another-secret", "JAVA_HOME": "/jdk"})
        child_env = process.kwargs["env"]
        self.assertEqual(child_env["ANDROID_HOME"], "/sdk")
        self.assertEqual(child_env["JAVA_HOME"], "/jdk")
        self.assertNotIn("GITHUB_TOKEN", child_env)
        self.assertNotIn("CUSTOM_API_KEY", child_env)
        self.assertNotIn("secret-fixture", json.dumps(common.load_json(ctx.work / "result.json")))


if __name__ == "__main__":
    unittest.main()
