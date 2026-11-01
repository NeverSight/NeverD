"""Fail-closed checks for the alternating command comparison helper."""
import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "sigprof" / "ab.py"
spec = importlib.util.spec_from_file_location("sigprof_ab", SCRIPT)
ab = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ab)


class ComparisonTests(unittest.TestCase):
    def command(self, source):
        return shlex.join([sys.executable, "-c", source])

    def compare(self, source_a, source_b=None, runs=1):
        with tempfile.TemporaryDirectory() as scratch:
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--runs", str(runs),
                 "--a", self.command(source_a),
                 "--b", self.command(source_a if source_b is None else source_b),
                 "--", "{out}"],
                text=True, capture_output=True, timeout=20,
                env={**os.environ, "TMPDIR": scratch})
            self.assertEqual(list(Path(scratch).iterdir()), [])
            return result

    def assert_invalid(self, result):
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("B/A", result.stdout)

    def test_matching_outputs_report_full_sha256(self):
        result = self.compare("import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(b'ok')", runs=2)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(hashlib.sha256(b"ok").hexdigest(), result.stdout)
        self.assertIn("B/A wall:", result.stdout)

    def test_failed_command_with_matching_output_is_invalid(self):
        result = self.compare(
            "import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(b'ok'); sys.exit(3)",
            "import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(b'ok')")
        self.assert_invalid(result)

    def test_missing_outputs_are_invalid_even_when_both_exit_zero(self):
        self.assert_invalid(self.compare("pass"))

    def test_different_outputs_are_invalid(self):
        self.assert_invalid(self.compare(
            "import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(b'a')",
            "import pathlib, sys; pathlib.Path(sys.argv[1]).write_bytes(b'b')"))

    def test_nonpositive_runs_are_rejected(self):
        for runs in (0, -1):
            with self.subTest(runs=runs):
                self.assert_invalid(self.compare("raise AssertionError('must not run')", runs=runs))

    def test_all_runs_must_agree_not_just_each_pair(self):
        with patch.object(sys, "argv", [str(SCRIPT), "--runs", "2", "--a", "a", "--b", "b"]), \
             patch.object(ab, "run", side_effect=[(0, 1., 1., "a"), (0, 1., 1., "a"),
                                                (0, 1., 1., "b"), (0, 1., 1., "b")]), \
             contextlib.redirect_stdout(io.StringIO()) as output, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ab.main(), 1)
            self.assertNotIn("B/A", output.getvalue())

    def test_zero_cpu_baseline_does_not_divide_by_zero(self):
        with patch.object(sys, "argv", [str(SCRIPT), "--runs", "1", "--a", "a", "--b", "b"]), \
             patch.object(ab, "run", return_value=(0, 0., 1., "same")), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(ab.main(), 0)
            self.assertIn("B/A cpu: unavailable", output.getvalue())
            self.assertIn("B/A wall:", output.getvalue())


if __name__ == "__main__":
    unittest.main()
