"""Failure and completeness checks for the real CLI byte comparison runner."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "tools/neverd-bench/radare2_cli_compare.py"
SPEC = importlib.util.spec_from_file_location("radare2_cli_compare", SCRIPT)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.case = {"address": 0x401000, "size": 3, "expected": bytes([0, 127, 255])}

    def test_both_complete_encodings_match_an_independent_oracle(self):
        neverd = BENCH.validate_result("neverd", b'{"addr":"0x401000","size":3,"bytes":"007fff"}', self.case)
        radare2 = BENCH.validate_result("radare2", b'[0,127,255]', self.case)
        self.assertEqual(neverd, radare2)
        self.assertEqual(neverd["bytes"], 3)

    def test_partial_wrong_or_malformed_success_is_not_comparable(self):
        values = {"neverd": [b'{}', b'{"addr":"0x401001","size":3,"bytes":"007fff"}',
                              b'{"addr":"0x401000","size":2,"bytes":"007f"}',
                              b'{"addr":"0x401000","size":3,"bytes":"007f00"}'],
                  "radare2": [b'[]', b'[0,127]', b'[0,127,254]', b'[false,127,255]',
                              b'[0,127,256]', b'{"error":"cannot read"}']}
        for tool, outputs in values.items():
            for output in outputs:
                with self.subTest(tool=tool, output=output), self.assertRaises((ValueError, TypeError)):
                    BENCH.validate_result(tool, output, self.case)

    def test_manifest_checks_identity_and_bounds_before_running_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "sample"
            binary.write_bytes(b'prefix' + self.case["expected"])
            item = {"name": "known", "input": "sample", "input_sha256": BENCH.identity(binary)["sha256"],
                    "address": "0x401000", "size": 3, "file_offset": 6}
            manifest = root / "cases.json"
            manifest.write_text(json.dumps({"schema": 1, "cases": [item]}))
            self.assertEqual(BENCH.load_cases(manifest)[0]["expected"], self.case["expected"])
            item["size"] = 4
            manifest.write_text(json.dumps({"schema": 1, "cases": [item]}))
            with self.assertRaises(ValueError):
                BENCH.load_cases(manifest)
            item["size"] = 3
            binary.write_bytes(b'changed input')
            manifest.write_text(json.dumps({"schema": 1, "cases": [item]}))
            with self.assertRaises(ValueError):
                BENCH.load_cases(manifest)

    def test_tool_commands_preserve_one_binary_argument(self):
        binary = Path('/a path/with; metacharacters')
        for tool in ("neverd", "radare2"):
            command = BENCH.command_for(tool, Path('/tool'), binary, self.case)
            self.assertEqual(command[-1], str(binary))
            self.assertEqual(command.count(str(binary)), 1)

    @unittest.skipUnless(sys.platform == "linux", "Linux process accounting")
    def test_wait4_retains_entire_output_and_observes_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = BENCH.measure([sys.executable, '-c', 'print("x" * 100000)'],
                                   os.environ.copy(), root / 'ok', 5)
            self.assertGreater(result["wall_ms"], 0)
            self.assertEqual(result["stdout"]["bytes"], 100001)
            with self.assertRaises(RuntimeError):
                BENCH.measure([sys.executable, '-c', 'raise SystemExit(7)'],
                              os.environ.copy(), root / 'failed', 5)
            self.assertTrue((root / 'failed/stderr').exists())

    @unittest.skipUnless(sys.platform == "linux", "Linux process groups and alarms")
    def test_timeout_stops_only_the_owned_process_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / 'timeout'
            script = 'import os,time; print(os.getpid(),flush=True); time.sleep(30)'
            with self.assertRaises(TimeoutError):
                BENCH.measure([sys.executable, '-c', script], os.environ.copy(), directory, 0.5)
            pid = int((directory / 'stdout').read_text())
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_warmups_do_not_enter_the_comparison(self):
        records = [{"case": "a", "warmup": warmup,
                    "tools": {"neverd": {"wall_ms": n}, "radare2": {"wall_ms": r}}}
                   for warmup, n, r in [(True, 1000, 1), (False, 10, 20), (False, 14, 24)]]
        result = BENCH.summarize(records)["a"]
        self.assertEqual(result["pairs"], 2)
        self.assertEqual(result["wall_median_ms"], {"neverd": 12, "radare2": 22})
        self.assertEqual(result["neverd_faster_pairs"], 2)


if __name__ == "__main__":
    unittest.main()
