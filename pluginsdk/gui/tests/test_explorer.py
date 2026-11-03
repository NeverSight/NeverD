import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SDK = Path(__file__).resolve().parents[1]
EXAMPLE = SDK / "examples/workbench-explorer"
sys.path.insert(0, str(SDK))
from neverd_gui_plugin import QueryError, Task, read_frame, write_frame

spec = importlib.util.spec_from_file_location("explorer", EXAMPLE / "explorer.py")
explorer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(explorer)

BASE = 0xffff800012340000
SLOT = BASE + 0x2000


class FixtureTask:
    action = "report"
    parameters = {}

    def __init__(self, directory, cfg_error=None, reference_error=None):
        self.output_dir = Path(directory)
        self.context = dict(epoch="1", function_address=hex(BASE), address=hex(BASE))
        self.cfg_error = cfg_error
        self.reference_error = reference_error
        self.operations = []

    def log(self, text):
        pass

    def progress(self, *args):
        pass

    def artifact(self, path, title):
        return Task.artifact(self, path, title)

    def query(self, operation, **payload):
        self.operations.append((operation, payload))
        if operation == "functions":
            return dict(items=[dict(address=hex(BASE), name="sample_function", size=16)], complete=True)
        if operation == "disasm":
            return dict(items=[
                dict(address=hex(BASE), size=5, mnemonic="call", operands="qword ptr [rip + 0x2000]"),
                dict(address=hex(BASE + 5), size=7, mnemonic="mov", operands="rax, 1"),
                dict(address=hex(BASE + 12), size=1, mnemonic="ret", operands=""),
                dict(address=hex(BASE + 13), size=3, mnemonic="nop", operands=""),
                dict(address=hex(BASE + 16), size=1, mnemonic="ret", operands="")], complete=True)
        if operation == "imports":
            return dict(items=[dict(address=hex(SLOT), name="ExFreePool", module="ntoskrnl.exe")], complete=True)
        if operation == "xrefs":
            if self.reference_error:
                raise self.reference_error
            return dict(items=[dict(to=hex(SLOT), kind="read", text="import storage")], complete=True)
        if operation == "cfg":
            if self.cfg_error:
                raise self.cfg_error
            nodes = [dict(id=str(index), start=hex(BASE + index), address=hex(BASE + index), label=f"block {index}") for index in range(300)]
            return dict(nodes=nodes, edges=[dict(from_=str(index), to=str(index + 1)) for index in range(299)])
        raise AssertionError(operation)


class FunctionReportTests(unittest.TestCase):
    def test_generic_image_addition_emits_real_x64_without_tvm(self):
        with tempfile.TemporaryDirectory() as directory:
            task = FixtureTask(directory)
            task.id = "01234567-abcd"
            task.action = "image-code"
            task.parameters = dict(address="0x150000000", value=123)
            source = Path(directory) / "input.bin"
            source.write_bytes(b"original input")
            task.context["input_path"] = str(source)
            task.query = lambda operation: dict(architecture="x86_64", bitness=64,
                                                analysis_sections_supported=True)
            result = explorer.handle(task)
            self.assertEqual((Path(directory) / "returns-value.bin").read_bytes(),
                             bytes.fromhex("b87b000000c3"))
            addition = result["image_addition"]
            self.assertEqual(addition["artifact"], 0)
            self.assertEqual(addition["description"]["functions"][0]["size"], 6)
            self.assertEqual(addition["description"]["address"], "0x150000000")

    def test_import_call_identity_and_exact_linear_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            task = FixtureTask(directory)
            result = explorer.handle(task)
            self.assertEqual(len(result["views"]), 4)
            self.assertIn("Call instructions: 1", result["views"][0]["text"])
            self.assertIn("ntoskrnl.exe!ExFreePool", result["views"][1]["rows"][0]["cells"])
            self.assertEqual(len(result["views"][3]["rows"]), 4)
            self.assertIn("import slot", result["views"][1]["rows"][0]["cells"][3])
            report = json.loads((Path(directory) / "function-report.json").read_text(encoding="utf-8"))
            self.assertTrue(report["linear_coverage_complete"])
            self.assertEqual(len(result["artifacts"]), 2)

    def test_optional_cfg_budget_does_not_discard_a_valid_report(self):
        with tempfile.TemporaryDirectory() as directory:
            error = QueryError(dict(status="budget_exceeded", error=dict(code="budget_exceeded", message="graph too large")))
            task = FixtureTask(directory, cfg_error=error)
            result = explorer.function_report(task, include_graph=True)
            self.assertEqual(len(result["views"]), 4)
            self.assertIn("graph too large", result["views"][0]["text"])
            self.assertIn("Call instructions: 1", result["views"][0]["text"])

    def test_large_cfg_preview_is_explicit_and_export_is_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            task = FixtureTask(directory)
            original_query = task.query

            def query(operation, **payload):
                result = original_query(operation, **payload)
                if operation == "cfg":
                    result["edges"] = [{"from": edge["from_"], "to": edge["to"]} for edge in result["edges"]]
                return result
            task.query = query
            result = explorer.function_report(task, include_graph=True)
            self.assertIn("128/300", result["views"][-1]["title"])
            self.assertEqual(len(result["views"][-1]["nodes"]), 128)
            report = json.loads((Path(directory) / "function-report.json").read_text(encoding="utf-8"))
            self.assertEqual(len(report["cfg"]["nodes"]), 300)

    def test_stale_query_is_never_masked_as_optional_data(self):
        with tempfile.TemporaryDirectory() as directory:
            error = QueryError(dict(status="error", error=dict(code="stale_revision", message="revision changed")))
            with self.assertRaises(QueryError):
                explorer.function_report(FixtureTask(directory, reference_error=error))

    def test_default_manifest_contains_useful_actions_only(self):
        manifest = json.loads((EXAMPLE / "plugin.json").read_text(encoding="utf-8"))
        self.assertEqual({action["id"] for action in manifest["actions"]}, {"report", "overview", "instructions", "graph"})

    def test_source_manifest_program_works_without_preparation_or_pythonpath(self):
        with tempfile.TemporaryDirectory() as directory:
            request = io.BytesIO()
            write_frame(request, dict(type="start", task_id="source", action="context", context={}, parameters={}, output_dir=directory))
            process = subprocess.run([sys.executable, str(EXAMPLE / "explorer.py")], cwd=EXAMPLE,
                                     input=request.getvalue(), capture_output=True, timeout=5)
            self.assertEqual(process.returncode, 0, process.stderr.decode(errors="replace"))
            stream = io.BytesIO(process.stdout)
            self.assertEqual(read_frame(stream)["type"], "hello")
            while True:
                event = read_frame(stream)
                if event["type"] == "result":
                    break
            self.assertEqual(event["task_id"], "source")


if __name__ == "__main__":
    unittest.main()
