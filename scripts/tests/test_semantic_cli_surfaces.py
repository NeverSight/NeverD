"""End-to-end checks for the proof-gated text-only CLI surfaces."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


def built_tool() -> Path | None:
    named = os.environ.get("NEVERD_BUILD_DIR")
    candidates = [Path(named)] if named else [ROOT / "build"]
    executable = "neverd.exe" if sys.platform == "win32" else "neverd"
    for candidate in candidates:
        tool = candidate / "bin" / executable
        if tool.is_file():
            return tool
    return None


TOOL = built_tool()


@unittest.skipIf(TOOL is None, "no built neverd tool to exercise")
class SemanticCLISurfaceTests(unittest.TestCase):
    def run_tool(
        self, *arguments: str, input_text: str | None = None
    ) -> subprocess.CompletedProcess[str]:
        assert TOOL is not None
        return subprocess.run(
            [str(TOOL), *arguments],
            input=input_text,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_synthesis_reports_the_equivalence_gate(self) -> None:
        completed = self.run_tool(
            "simplify",
            "--synthesize",
            "--json",
            "--max-cost=5",
            "--max-work=262144",
            "(x >> 4) + ((x >> 2) >> 2)",
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertEqual(len(report), 1)
        self.assertEqual(report[0]["schemaVersion"], 1)
        self.assertTrue(report[0]["changed"])
        self.assertEqual(report[0]["proofStatus"], "equivalent")
        self.assertGreater(report[0]["proofQueries"], 0)
        self.assertGreater(report[0]["candidateCost"], 0)
        self.assertGreater(report[0]["searchWork"], 0)
        self.assertNotIn("work", report[0])

    def test_incomplete_synthesis_has_a_distinct_exit_status(self) -> None:
        completed = self.run_tool(
            "simplify",
            "--synthesize",
            "--json",
            "--max-cost=5",
            "--max-work=1",
            "(x >> 4) + ((x >> 2) >> 2)",
        )
        self.assertEqual(completed.returncode, 3, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertEqual(report[0]["outcome"], "search-budget-exhausted")
        self.assertEqual(report[0]["proofStatus"], "not-run")

    def test_expression_syntax_error_is_an_input_error(self) -> None:
        completed = self.run_tool("simplify", "--synthesize", "--json", "(x +")
        self.assertEqual(completed.returncode, 2)
        report = json.loads(completed.stdout)
        self.assertEqual(report[0]["schemaVersion"], 1)
        self.assertFalse(report[0]["ok"])

    def test_optimize_ir_returns_only_the_committed_module(self) -> None:
        ir = """
define internal i32 @helper() {
entry:
  ret i32 42
}
define i32 @entry() {
entry:
  %value = call i32 @helper()
  ret i32 %value
}
"""
        completed = self.run_tool("optimize-ir", "--json", "-", input_text=ir)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertEqual(report["schemaVersion"], 1)
        self.assertTrue(report["ok"])
        self.assertEqual(report["stop"], "stable")
        self.assertIn("ret i32 42", report["outputIR"])

    def test_invalid_ir_has_a_structured_diagnostic_and_fails(self) -> None:
        completed = self.run_tool(
            "optimize-ir", "--json", "-", input_text="define i32 @broken( {"
        )
        self.assertEqual(completed.returncode, 2)
        report = json.loads(completed.stdout)
        self.assertFalse(report["ok"])
        self.assertEqual(report["stop"], "input-invalid")
        self.assertIn("error", report)

    def test_synthesis_only_expression_options_are_rejected_without_mode(self) -> None:
        completed = self.run_tool("simplify", "--max-cost=5", "x + 0")
        self.assertEqual(completed.returncode, 2)
        self.assertIn("require --synthesize", completed.stderr)

    def test_legacy_expression_options_cannot_leak_into_synthesis(self) -> None:
        completed = self.run_tool(
            "simplify", "--synthesize", "--shallow", "x + 0"
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("cannot be combined with --synthesize", completed.stderr)

    def test_optimize_ir_rejects_inactive_synthesis_budgets(self) -> None:
        completed = self.run_tool(
            "optimize-ir",
            "--synthesis-max-cost=5",
            "-",
            input_text="define i32 @f() { ret i32 0 }\n",
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("require --synthesize", completed.stderr)

    def test_conservative_mode_cannot_claim_synthesis(self) -> None:
        completed = self.run_tool(
            "optimize-ir",
            "--mode=conservative",
            "--synthesize",
            "-",
            input_text="define i32 @f() { ret i32 0 }\n",
        )
        self.assertEqual(completed.returncode, 2)
        self.assertIn("cannot be combined", completed.stderr)

    def test_json_and_stdout_ir_are_not_interleaved(self) -> None:
        completed = self.run_tool(
            "optimize-ir",
            "--json",
            "-o",
            "-",
            "-",
            input_text="define i32 @f() { ret i32 0 }\n",
        )
        self.assertEqual(completed.returncode, 2)
        self.assertEqual(completed.stdout, "")
        self.assertIn("--json cannot be combined with -o -", completed.stderr)

    def test_json_can_write_the_committed_ir_to_a_separate_file(self) -> None:
        ir = "define i32 @f() { ret i32 0 }\n"
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "optimized.ll"
            completed = self.run_tool(
                "optimize-ir", "--json", "-o", str(output), "-", input_text=ir
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads(completed.stdout)
            self.assertTrue(report["ok"])
            self.assertEqual(output.read_text(encoding="utf-8"), report["outputIR"])


if __name__ == "__main__":
    unittest.main()
