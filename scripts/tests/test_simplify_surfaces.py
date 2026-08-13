"""The three ways to reach the optimiser must give the same answer.

`neverd simplify`, the typed C entry point the Python SDK drives, and the older
JSON entry point are three surfaces over one implementation.  That is easy to
state and easy to lose: a default that drifts on one side, a field read from the
wrong place, an option the CLI translates differently.  Nothing in a unit test
of any one surface would notice, because each would still be self-consistent.

So this runs the same corpus through all three and compares.  It is an
integration test -- it needs the built library and tool -- and skips rather than
fails when they are absent, because not every checkout has been built.
"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
CORPUS = REPO_ROOT / "tools" / "neverd-bench" / "mba-corpus.txt"

_LIBRARY_NAMES = ("libneverd.dylib", "libneverd.so", "neverd.dll")


def build_directory() -> Path | None:
    """Where the tool and library were built, if they were.

    An explicit setting wins, because a checkout commonly carries several build
    trees and only the caller knows which one is current.
    """

    named = os.environ.get("NEVERD_BUILD_DIR")
    candidates = [Path(named)] if named else [REPO_ROOT / "build"]
    for candidate in candidates:
        binaries = candidate / "bin"
        tool = binaries / ("neverd.exe" if sys.platform == "win32" else "neverd")
        if tool.exists() and any((binaries / n).exists() for n in _LIBRARY_NAMES):
            return candidate
    return None


def corpus_expressions() -> list[str]:
    lines = CORPUS.read_text(encoding="utf-8").splitlines()
    return [
        stripped
        for line in lines
        if (stripped := line.strip()) and not stripped.startswith("#")
    ]


BUILD = build_directory()


@unittest.skipIf(BUILD is None, "no built neverd tool and library to compare")
class SimplifySurfaceAgreementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        assert BUILD is not None
        cls.binaries = BUILD / "bin"
        cls.expressions = corpus_expressions()
        library = next(
            path
            for name in _LIBRARY_NAMES
            if (path := cls.binaries / name).exists()
        )
        cls.library = ctypes.CDLL(str(library))

    def run_cli(self, *extra: str) -> object:
        tool = self.binaries / ("neverd.exe" if sys.platform == "win32" else "neverd")
        completed = subprocess.run(
            [str(tool), "simplify", "-f", str(CORPUS), "--json", *extra],
            capture_output=True,
            text=True,
            check=False,
        )
        # A corpus line that will not parse is a corpus bug, and the exit code
        # is how the tool says so.
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return json.loads(completed.stdout)

    def cli_results(self) -> list[dict[str, object]]:
        results = self.run_cli()
        self.assertIsInstance(results, list)
        return results

    def python_results(self) -> list[dict[str, object]]:
        sys.path.insert(0, str(REPO_ROOT / "pluginsdk" / "python"))
        try:
            from neverd_plugin import simplify_expression
            from neverd_plugin.ffi import HostAPI
        finally:
            sys.path.pop(0)

        host = HostAPI(self.library)
        out: list[dict[str, object]] = []
        for expression in self.expressions:
            result = simplify_expression(expression, host=host)
            out.append(
                {
                    "input": result.input,
                    "output": result.output,
                    "changed": result.changed,
                    "costBefore": result.cost_before,
                    "costAfter": result.cost_after,
                    "inputs": result.inputs,
                    "work": result.work,
                    "outcome": result.outcome.name.lower().replace("_", "-"),
                    "evidence": result.evidence.name.lower(),
                }
            )
        return out

    def json_api_results(self) -> list[dict[str, object]]:
        entry = self.library.neverd_simplify_expr_json
        entry.restype = ctypes.c_void_p
        entry.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_int]
        release = self.library.neverd_free_string
        release.argtypes = [ctypes.c_char_p]

        out: list[dict[str, object]] = []
        for expression in self.expressions:
            pointer = entry(expression.encode("utf-8"), 32, 1)
            self.assertTrue(pointer)
            try:
                out.append(json.loads(ctypes.string_at(pointer).decode("utf-8")))
            finally:
                release(ctypes.cast(ctypes.c_void_p(pointer), ctypes.c_char_p))
        return out

    def test_the_three_surfaces_agree(self) -> None:
        cli = self.cli_results()
        python = self.python_results()
        native = self.json_api_results()

        self.assertEqual(len(cli), len(self.expressions))
        self.assertEqual(len(python), len(self.expressions))
        self.assertEqual(len(native), len(self.expressions))

        compared = (
            "input",
            "output",
            "changed",
            "costBefore",
            "costAfter",
            "inputs",
            "work",
            "outcome",
            "evidence",
        )
        for index, expression in enumerate(self.expressions):
            with self.subTest(expression=expression):
                for field in compared:
                    self.assertEqual(
                        cli[index][field],
                        python[index][field],
                        f"CLI and Python disagree on {field}",
                    )
                    self.assertEqual(
                        cli[index][field],
                        native[index][field],
                        f"CLI and the JSON entry point disagree on {field}",
                    )

    def test_the_corpus_still_exercises_the_optimiser(self) -> None:
        """A corpus nothing collapses would let the test above pass on nothing.

        The summary is printed rather than only asserted on, because the numbers
        are the point of having a fixed corpus: a build log that carries them is
        what makes a change in the rate or the tail visible at the commit that
        caused it, instead of months later.
        """

        report = self.run_cli("--stats")
        self.assertIsInstance(report, dict)
        summary = report["summary"]
        print(f"\nsimplify corpus: {json.dumps(summary, sort_keys=True)}")

        self.assertEqual(summary["expressions"], len(self.expressions))
        self.assertEqual(summary["rejected"], 0)
        self.assertGreater(summary["simplified"], len(self.expressions) // 2)
        # A rewrite is only made when it is shorter, so this cannot creep up
        # without something having gone wrong in the size guard.
        self.assertLess(summary["costAfter"], summary["costBefore"])


if __name__ == "__main__":
    unittest.main()
