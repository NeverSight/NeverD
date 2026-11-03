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

SDK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SDK))
from neverd_gui_plugin import MAX_FRAME, QueryError, Task, read_frame, write_frame


class FramingTests(unittest.TestCase):
    def test_fragmented_utf8_roundtrip(self):
        stream = io.BytesIO()
        write_frame(stream, dict(type="log", text="中文路径 🚀"))
        stream.seek(0)

        class Fragmented:
            def read(self, size):
                return stream.read(min(1, size))
        self.assertEqual(read_frame(Fragmented())["text"], "中文路径 🚀")

    def test_refuses_invalid_sizes_truncation_and_version(self):
        for frame in [struct.pack(">I", 0), struct.pack(">I", MAX_FRAME + 1), b"\0\0",
                      struct.pack(">I", 10) + b"{}", struct.pack(">I", 2) + b"[]"]:
            with self.subTest(frame=frame), self.assertRaises((ValueError, EOFError)):
                read_frame(io.BytesIO(frame))
        with self.assertRaises(ValueError):
            write_frame(io.BytesIO(), dict(type="log", text="x" * MAX_FRAME))

    def test_query_reply_identity(self):
        reply = io.BytesIO()
        write_frame(reply, dict(type="query_reply", task_id="wrong", id="1", reply=dict(status="ok")))
        reply.seek(0)
        task = Task(dict(type="start", task_id="one", action="test", context={}, parameters={}, output_dir="."), reply, io.BytesIO())
        with self.assertRaises(ValueError):
            task.query("metadata")

    def test_query_error_retains_host_status_and_code(self):
        for status, code in [("budget_exceeded", "budget_exceeded"), ("error", "stale_revision")]:
            with self.subTest(status=status, code=code):
                response = dict(status=status, error=dict(code=code, message="host diagnostic"))
                reply = io.BytesIO()
                write_frame(reply, dict(type="query_reply", task_id="one", id="1", reply=response))
                reply.seek(0)
                task = Task(dict(type="start", task_id="one", action="test", context={}, parameters={}, output_dir="."), reply, io.BytesIO())
                with self.assertRaises(QueryError) as raised:
                    task.query("cfg")
                self.assertEqual(raised.exception.status, status)
                self.assertEqual(raised.exception.code, code)
                self.assertEqual(raised.exception.reply, response)
                self.assertIn("host diagnostic", str(raised.exception))


class ArtifactTests(unittest.TestCase):
    def task(self, directory):
        return Task(dict(type="start", task_id="artifact", action="report", context={},
                         parameters={}, output_dir=str(directory)))

    def test_nested_artifact_uses_relative_path_and_content_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            report = root / "reports" / "报告.json"
            report.parent.mkdir()
            data = '{"name":"函数"}'.encode("utf-8")
            report.write_bytes(data)
            artifact = self.task(root).artifact("reports/报告.json", "Function report")
            self.assertEqual(artifact, dict(title="Function report", path="reports/报告.json",
                                            sha256=hashlib.sha256(data).hexdigest()))

    def test_output_directory_and_sibling_artifacts_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "job"
            root.mkdir()
            sibling = Path(directory) / "job-other" / "report.json"
            sibling.parent.mkdir()
            sibling.write_text("outside", encoding="utf-8")
            task = self.task(root)
            for path in (".", "../job-other/report.json", str(sibling)):
                with self.subTest(path=path), self.assertRaisesRegex(ValueError, "output directory"):
                    task.artifact(path, "Invalid artifact")

    def test_reserved_receipt_name_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "RECEIPT.JSON").write_text("{}", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Reserved artifact"):
                self.task(root).artifact("RECEIPT.JSON", "Reserved")

    def test_resolved_symlink_cannot_escape_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "job"
            root.mkdir()
            outside = Path(directory) / "outside.json"
            outside.write_text("outside", encoding="utf-8")
            link = root / "report.json"
            try:
                link.symlink_to(outside)
            except OSError as error:
                self.skipTest(f"Symlink creation unavailable: {error}")
            with self.assertRaisesRegex(ValueError, "output directory"):
                self.task(root).artifact("report.json", "Outside link")


class HostServicesTests(unittest.TestCase):
    def task(self, *frames, extensions=True):
        replies, output = io.BytesIO(), io.BytesIO()
        for frame in frames:
            write_frame(replies, frame)
        replies.seek(0)
        task = Task(dict(type="start", task_id="one", action="test", context={},
                         parameters={}, output_dir=".",
                         extensions=["host_services", "host_events"] if extensions else []), replies, output)
        return task, output

    def test_event_interleaves_with_legacy_query(self):
        event = dict(type="event", task_id="one", subscription="7",
                     event=dict(id="project.revision_changed", version=1, sequence="1"))
        task, _ = self.task(event, dict(type="query_reply", task_id="one", id="1",
                                      reply=dict(status="ok", payload=dict(value=42))))
        self.assertEqual(task.query("metadata"), dict(value=42))
        self.assertEqual(task.next_event()["event"], event["event"])

    def test_service_calls_use_discovered_registration(self):
        task, output = self.task(
            dict(type="host_reply", task_id="one", id="1", reply=dict(status="ok", payload=dict(
                services=[dict(id="sample", version=1, declared=True, registration="123")], missing=[]))),
            dict(type="host_reply", task_id="one", id="2", reply=dict(status="ok", payload=42)))
        self.assertEqual(task.service("sample", value=41), 42)
        output.seek(0)
        self.assertEqual(read_frame(output)["operation"], "services.list")
        request = read_frame(output)
        self.assertEqual(request["payload"], dict(registration="123", payload=dict(value=41)))
        with self.assertRaises(QueryError):
            Task._binding(dict(services=[dict(id="sample", version=1, declared=True)]), "services", "sample", 2)

    def test_host_extension_is_required(self):
        task, output = self.task(extensions=False)
        with self.assertRaises(RuntimeError):
            task.services()
        self.assertEqual(output.getvalue(), b"")

    def test_handler_pump_returns_after_one_notification(self):
        task, _ = self.task(dict(type="event", task_id="one", subscription="7",
                                 event=dict(id="task.changed", version=1)))
        handled = []
        task._event_handlers["7"] = handled.append
        event = task.next_event()
        self.assertEqual(handled, [event["event"]])


class ExternalExampleTests(unittest.TestCase):
    def test_prepare_scripts_print_unicode_paths_under_ascii_stdout(self):
        examples = [SDK / "examples/workbench-explorer",
                    SDK.parent / "common/examples/host-observer",
                    SDK.parent / "common/examples/tool-roundtrip"]
        with tempfile.TemporaryDirectory(prefix="neverd-prepare-") as directory:
            for index, source in enumerate(examples):
                with self.subTest(example=source.name):
                    destination = Path(directory) / ("插件 " + str(index))
                    response = subprocess.run(
                        [sys.executable, str(source / "prepare.py"), str(destination)],
                        capture_output=True,
                        env=dict(os.environ, PYTHONIOENCODING="ascii", PYTHONUTF8="0"))
                    self.assertEqual(response.returncode, 0, response.stderr.decode("ascii", errors="replace"))
                    self.assertEqual(response.stdout.decode("utf-8").strip(),
                                     str(destination.resolve() / "plugin.json"))
                    self.assertTrue((destination / "plugin.json").is_file())

    def test_prepared_copy_runs_outside_repository(self):
        with tempfile.TemporaryDirectory(prefix="neverd gui 中文 ") as directory:
            destination = Path(directory) / "external plugin"
            subprocess.run([sys.executable, str(SDK / "examples/workbench-explorer/prepare.py"), str(destination)], check=True, capture_output=True)
            manifest = json.loads((destination / "plugin-developer.json").read_text(encoding="utf-8"))
            output = Path(directory) / "job"
            output.mkdir()
            process = subprocess.Popen([manifest["program"], *manifest["arguments"]], cwd=destination,
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                self.assertEqual(read_frame(process.stdout)["type"], "hello")
                write_frame(process.stdin, dict(type="start", task_id="external", action="background",
                            context=dict(epoch="1", loaded=False), output_dir=str(output),
                            parameters=dict(steps=2, delay_ms=0, message="中文", letter_case="original", write_report=True)))
                messages = []
                while True:
                    message = read_frame(process.stdout)
                    messages.append(message)
                    if message["type"] in {"result", "error"}:
                        break
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(messages[-1]["type"], "result")
                progress = [item["progress"] for item in messages if item["type"] == "progress"]
                self.assertNotIn("total", progress[0])
                self.assertEqual(progress[-1]["done"], 2)
                artifact = messages[-1]["result"]["artifacts"][0]
                self.assertEqual(hashlib.sha256((output / artifact["path"]).read_bytes()).hexdigest(), artifact["sha256"])
                self.assertTrue((destination / "neverd_gui_plugin.py").is_file())
                self.assertTrue((destination / "README.md").is_file())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=5)
                process.stdin.close()
                process.stdout.close()
                process.stderr.close()


if __name__ == "__main__":
    unittest.main()
