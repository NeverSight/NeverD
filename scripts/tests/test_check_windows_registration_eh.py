import hashlib
import os
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_eh as runner


def pe32_image(extra: bytes = b"") -> bytes:
    image = bytearray(0x100)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, 0x40)
    image[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", image, 0x44, 0x14C)
    struct.pack_into("<H", image, 0x58, 0x10B)
    return bytes(image) + extra


class WindowsRegistrationEHRunnerTests(unittest.TestCase):
    def test_process_exit_does_not_wait_for_inherited_output_handles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            service = root / "service.py"
            ready = root / "ready"
            stop = root / "stop"
            done = root / "done"
            service.write_text(
                "import pathlib, sys, time\n"
                "pathlib.Path(sys.argv[1]).touch()\n"
                "while not pathlib.Path(sys.argv[2]).exists(): time.sleep(0.01)\n"
                "pathlib.Path(sys.argv[3]).touch()\n")
            probe = root / "probe.py"
            probe.write_text(
                "import os, pathlib, subprocess, sys, time\n"
                f"subprocess.Popen([sys.executable, {str(service)!r}, "
                f"{str(ready)!r}, {str(stop)!r}, {str(done)!r}])\n"
                f"while not pathlib.Path({str(ready)!r}).exists(): time.sleep(0.01)\n"
                "os.write(1, b'passed\\r\\n')\n"
                "os.write(2, b'diagnostic\\r\\n')\n"
                "sys.exit(7)\n")
            try:
                result = runner.run_image(probe, [sys.executable], os.environ.copy(), 5)
                self.assertEqual(result.get("exit_code"), 7, result)
                self.assertEqual(result["stdout"], "passed\n")
                self.assertEqual(result["stderr"], "diagnostic\n")
            finally:
                stop.touch()
                deadline = time.monotonic() + 5
                while not done.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(done.exists(), "output-handle child did not finish")

    def test_live_process_timeout_retains_output_without_an_exit_code(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "probe.py"
            path.write_text("import os, time\nos.write(1, b'before timeout\\r\\n')\n"
                            "os.write(2, b'still running\\r\\n')\ntime.sleep(30)\n")
            result = runner.run_image(path, [sys.executable], os.environ.copy(), 2)
            self.assertNotIn("exit_code", result)
            self.assertIn("timeout", result["error"])
            self.assertEqual(result["stdout"], "before timeout\n")
            self.assertEqual(result["stderr"], "still running\n")

    def test_matrix_covers_both_languages_security_and_optimization(self):
        cases = runner.probe_cases()
        self.assertEqual(len(cases), 8)
        self.assertEqual(len({path for path, _ in cases}), 8)

    def test_rejects_a_64_bit_image_before_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            image = bytearray(pe32_image())
            struct.pack_into("<H", image, 0x44, 0x8664)
            path = Path(directory) / "probe.exe"
            path.write_bytes(image)
            with self.assertRaisesRegex(ValueError, "x86 PE32"):
                runner.image_digest(path)

    def test_absent_replacement_fails_instead_of_running_only_original(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "original").mkdir()
            (root / "original/probe.exe").write_bytes(pe32_image())
            with patch.object(runner, "run_image") as execute:
                result = runner.observe_case(Path("probe.exe"), "passed",
                                             root / "original", root / "patched",
                                             [], {}, 1)
            self.assertFalse(result["passed"])
            execute.assert_not_called()

    def test_identical_copy_cannot_count_as_reconstruction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("original", "patched"):
                (root / name).mkdir()
                (root / name / "probe.exe").write_bytes(pe32_image())
            with patch.object(runner, "run_image") as execute:
                result = runner.observe_case(Path("probe.exe"), "passed",
                                             root / "original", root / "patched",
                                             [], {}, 1)
            self.assertFalse(result["passed"])
            self.assertIn("byte-identical", result["error"])
            execute.assert_not_called()

    def test_mismatched_runtime_result_fails_the_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "original").mkdir()
            (root / "patched").mkdir()
            (root / "original/probe.exe").write_bytes(pe32_image())
            (root / "patched/probe.exe").write_bytes(pe32_image(b"changed"))
            outcomes = [{"exit_code": 0, "stdout": "passed\n"},
                        {"exit_code": 1, "stdout": "passed\n"}]
            with patch.object(runner, "run_image", side_effect=outcomes):
                result = runner.observe_case(Path("probe.exe"), "passed",
                                             root / "original", root / "patched",
                                             [], {}, 1)
            self.assertFalse(result["passed"])
            self.assertIn("differs", result["error"])

    def test_successful_pair_retains_both_hashes_and_outcomes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, data in (("original", pe32_image()),
                               ("patched", pe32_image(b"changed"))):
                (root / name).mkdir()
                (root / name / "probe.exe").write_bytes(data)
            with patch.object(runner, "run_image",
                              return_value={"exit_code": 0, "stdout": "passed\n"}):
                result = runner.observe_case(Path("probe.exe"), "passed",
                                             root / "original", root / "patched",
                                             [], {}, 1)
            self.assertTrue(result["passed"])
            self.assertEqual(result["original_sha256"],
                             hashlib.sha256(pe32_image()).hexdigest())
            self.assertIn("patched", result)


if __name__ == "__main__":
    unittest.main()
