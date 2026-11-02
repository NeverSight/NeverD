"""Reject timing reports that did not deliver a current listing of the input."""
import copy
import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "startup_bench.py"
SPEC = importlib.util.spec_from_file_location("startup_bench", SCRIPT)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class StartupReportTests(unittest.TestCase):
    def setUp(self):
        self.report = {
            "schema_version": 4, "success": True, "input_path": str(Path("/sample.elf")),
            "milestones": {"worker_ready_ms": 20, "metadata_ms": 30,
                           "window_created_ms": 10, "first_frame_ms": 15,
                           "useful_frame_ms": 40, "useful_frame_flushed_ms": 41},
            "listing": {"current_address": "0xffff800000400078",
                        "top_address": "0xffff800000400078",
                        "rows_current": True, "jump_pending": False,
                        "loaded_text": "mov w0, #7\nret"}}

    def test_current_listing_preserves_high_and_zero_addresses(self):
        self.assertIsNone(BENCH.validate_report(self.report, 0, Path('/sample.elf')))
        self.report["listing"]["current_address"] = "0x0"
        self.assertIsNone(BENCH.validate_report(self.report, 0, Path('/sample.elf')))

    def test_stale_pending_or_empty_content_cannot_qualify_a_fast_paint(self):
        for key, value in (("rows_current", False), ("jump_pending", True),
                           ("loaded_text", " \n"), ("current_address", 18446603336225390712),
                           ("top_address", "0x10000000000000000")):
            with self.subTest(key=key):
                report = copy.deepcopy(self.report)
                report["listing"][key] = value
                self.assertIsNotNone(BENCH.validate_report(report, 0, Path('/sample.elf')))

    def test_another_input_or_failed_process_does_not_qualify(self):
        self.assertIsNotNone(BENCH.validate_report(self.report, 0, Path('/other.elf')))
        self.assertIsNotNone(BENCH.validate_report(self.report, 1, Path('/sample.elf')))

    def test_qt_forward_slash_paths_are_compared_as_paths(self):
        binary = (Path.cwd() / "sample.elf").resolve()
        self.report["input_path"] = binary.as_posix()
        self.assertIsNone(BENCH.validate_report(self.report, 0, binary))
        self.report["input_path"] = None
        self.assertIsNotNone(BENCH.validate_report(self.report, 0, binary))

    def test_old_or_missing_listing_evidence_does_not_qualify(self):
        self.report["schema_version"] = 3
        self.assertIsNotNone(BENCH.validate_report(self.report, 0))
        self.report["schema_version"] = 4
        del self.report["listing"]
        self.assertIsNotNone(BENCH.validate_report(self.report, 0))

    def test_invalid_or_out_of_order_milestones_cannot_qualify(self):
        for value in (True, -1, float("inf"), float("nan"), "40", 14, 42):
            with self.subTest(value=value):
                report = copy.deepcopy(self.report)
                report["milestones"]["useful_frame_ms"] = value
                self.assertIsNotNone(BENCH.validate_report(report, 0))


if __name__ == "__main__":
    unittest.main()
