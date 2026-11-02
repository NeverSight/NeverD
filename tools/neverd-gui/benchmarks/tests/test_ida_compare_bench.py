"""Exercise complete-source measurement without an installed IDA or worker."""

import hashlib
import importlib.util
from pathlib import Path
import json
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "ida_compare_bench", Path(__file__).resolve().parents[1] / "ida_compare_bench.py")
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class Client:
    def __init__(self, pages):
        self.pages = pages
        self.process = SimpleNamespace(pid=-1)
        self.requests = []
        self.pending = None
        self.closed = False

    def send(self, operation, payload):
        self.requests.append((operation, payload))
        request = len(self.requests)
        if operation == "functions":
            value = {"items": [{"address": "0x1000"}, {"address": "0x2000"}]}
        elif operation == "decompile":
            value = self.pages[(payload["address"], payload["offset"])]
        else:
            value = {}
        self.pending = dict(type="response", request_id=request, status="ok", payload=value)
        return request

    def next(self, predicate, timeout):
        value = self.pending or dict(type="heartbeat", background={
            "warm": {"state": "complete", "prepared": 2}})
        self.pending = None
        if not predicate(value):
            raise AssertionError(value)
        return value

    def close(self):
        self.closed = True


class CompleteSourceMeasurement(unittest.TestCase):
    def run_worker(self, pages, functions=(0x1000,), warm=False):
        client = Client(pages)
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "input"
            binary.write_bytes(b"fixture")
            result = BENCH.run_neverd(SimpleNamespace(Client=lambda worker: client),
                                     Path("worker"), binary, functions, 1, warm=warm)
        self.assertTrue(client.closed)
        return result, client

    def test_counts_and_hashes_the_entire_source(self):
        result, client = self.run_worker({
            ("0x1000", 0): {"text": "first\n", "next_offset": 512},
            ("0x1000", 512): {"text": "last\n", "next_offset": None},
        })
        record, = result["decompile_records"]
        self.assertEqual(record["bytes"], 11)
        self.assertEqual(record["pages"], 2)
        self.assertEqual(record["sha256"], hashlib.sha256(b"first\nlast\n").hexdigest())
        self.assertLessEqual(record["first_page_ms"], record["ms"])
        self.assertEqual(result["decompile_failures"], [])
        self.assertEqual([p["offset"] for op, p in client.requests if op == "decompile"], [0, 512])
        self.assertFalse(client.requests[0][1]["warm"])

    def test_a_bad_following_page_cannot_count_as_success(self):
        for following in (0, -1, True, "512"):
            with self.subTest(following=following):
                result, _ = self.run_worker({
                    ("0x1000", 0): {"text": "partial", "next_offset": following},
                    ("0x2000", 0): {"text": "complete", "next_offset": None},
                }, functions=(0x1000, 0x2000))
                self.assertEqual(len(result["decompile_ms"]), 1)
                self.assertEqual(len(result["decompile_failures"]), 1)
                self.assertEqual(result["decompile_failures"][0]["entry"], "0x1000")
                self.assertNotIn("sha256", result["decompile_records"][0])
                self.assertIn("sha256", result["decompile_records"][1])

    def test_empty_output_is_a_failure(self):
        result, _ = self.run_worker({("0x1000", 0): {"text": ""}})
        self.assertEqual(result["decompile_ms"], [])
        self.assertEqual(result["decompile_failures"][0]["error"], "empty C source")

    def test_missing_entry_cannot_measure_a_containing_function(self):
        result, client = self.run_worker({}, functions=(0x3000,))
        self.assertEqual(result["decompile_ms"], [])
        self.assertEqual(result["decompile_failures"][0]["entry"], "0x3000")
        self.assertFalse(any(op == "decompile" for op, _ in client.requests))

    def test_warming_is_explicit_and_reported_separately(self):
        result, client = self.run_worker({("0x1000", 0): {"text": "body"}}, warm=True)
        self.assertTrue(client.requests[0][1]["warm"])
        self.assertEqual(result["warm_state"], {"state": "complete", "prepared": 2})
        self.assertGreaterEqual(result["ready_ms"], result["open_ms"])

    def test_ida_none_is_not_successful_c_text(self):
        modules = {
            "idapro": SimpleNamespace(open_database=lambda *args: 0, close_database=lambda save: None),
            "ida_auto": SimpleNamespace(auto_wait=lambda: None),
            "ida_kernwin": SimpleNamespace(get_kernel_version=lambda: "9.5"),
            "ida_hexrays": SimpleNamespace(init_hexrays_plugin=lambda: True,
                                           decompile=lambda entry: None if entry == 0x1000 else "body"),
            "idautils": SimpleNamespace(Functions=lambda: [0x1000, 0x2000]),
        }
        with tempfile.TemporaryDirectory() as directory, patch.dict("sys.modules", modules):
            functions = Path(directory) / "entries.json"
            output = Path(directory) / "report.json"
            functions.write_text(json.dumps([0x1000, 0x2000]))
            BENCH.ida_child(Path("input"), str(functions), output)
            result = json.loads(output.read_text())
        self.assertEqual(result["kernel_version"], "9.5")
        self.assertEqual(len(result["decompile_ms"]), 1)
        self.assertEqual(result["decompile_failures"][0]["entry"], "0x1000")
        self.assertNotIn("sha256", result["decompile_records"][0])
        self.assertEqual(result["decompile_records"][1]["bytes"], 4)


if __name__ == "__main__":
    unittest.main()
