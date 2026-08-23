"""Behavioral tests for the standalone NeverD benchmark harness scripts."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
RUN_ALL_PATH = ROOT / "tools" / "neverd-bench" / "run_all.py"
COMPARE_PATH = ROOT / "tools" / "neverd-bench" / "compare.py"


def load_script(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_comparable_sample(
    bench_dir: Path, stem: str = "sample", *, nd_ms: int = 20, ref_ms: int = 100
) -> None:
    (bench_dir / f"{stem}.nd.bench.json").write_text(
        json.dumps(
            {
                "functions": [
                    {"name": "function", "entry": "0x1000", "blocks": 1, "ops": 1}
                ],
                "audit_functions": [
                    {
                        "entry": "0x1000",
                        "low_ir": True,
                        "med_ir": True,
                        "med_ir_verified": True,
                        "llvm_definition": True,
                    }
                ],
                "total_time_ms": nd_ms,
            }
        ),
        encoding="utf-8",
    )
    (bench_dir / f"{stem}.nd.imports.json").write_text("[]", encoding="utf-8")
    (bench_dir / f"{stem}.nd.strings.json").write_text("[]", encoding="utf-8")
    (bench_dir / f"{stem}.ref.functions.json").write_text(
        json.dumps([{"addr": 0x1000}]), encoding="utf-8"
    )
    (bench_dir / f"{stem}.ref.imports.json").write_text("[]", encoding="utf-8")
    (bench_dir / f"{stem}.ref.strings.json").write_text("[]", encoding="utf-8")
    (bench_dir / f"{stem}.ref.timings.json").write_text(
        json.dumps({"total_ms": ref_ms}), encoding="utf-8"
    )


class RunAllCLITests(unittest.TestCase):
    def test_failed_reference_run_cannot_reuse_stale_reference_results(self) -> None:
        run_all = load_script("neverd_bench_run_all_failed_reference", RUN_ALL_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "inputs"
            out_dir = root / "output"
            bin_dir.mkdir()
            out_dir.mkdir()
            (bin_dir / "sample.exe").write_bytes(b"fixture")
            for suffix in (
                ".ref.functions.json",
                ".ref.imports.json",
                ".ref.strings.json",
                ".ref.timings.json",
            ):
                (out_dir / f"sample{suffix}").write_text("[]", encoding="utf-8")

            def fake_run(command: list[str], timeout: int):
                if str(run_all.REF_DUMP) in command:
                    return 7, "", "reference failed"
                return 0, "ok", ""

            argv = [
                str(RUN_ALL_PATH),
                "--bin-dir",
                str(bin_dir),
                "--out-dir",
                str(out_dir),
            ]
            with mock.patch.object(run_all, "run", side_effect=fake_run), \
                 mock.patch.object(sys, "argv", argv), \
                 self.assertRaises(SystemExit) as raised:
                run_all.main()

            self.assertEqual(raised.exception.code, 1)
            self.assertFalse((out_dir / "sample.ref.functions.json").exists())
            progress = json.loads((out_dir / "progress.json").read_text())
            self.assertEqual(progress["sample"]["status"], "ref_failed")

    def test_report_is_scoped_to_selected_binary_stems(self) -> None:
        run_all = load_script("neverd_bench_run_all_selected_stems", RUN_ALL_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "inputs"
            out_dir = root / "output"
            bin_dir.mkdir()
            out_dir.mkdir()
            (bin_dir / "selected.exe").write_bytes(b"fixture")
            (out_dir / "unselected.nd.bench.json").write_text(
                "{not-json", encoding="utf-8"
            )

            commands: list[list[str]] = []

            def fake_run(command: list[str], timeout: int):
                commands.append(command)
                return 0, "ok", ""

            argv = [
                str(RUN_ALL_PATH),
                "--bin-dir",
                str(bin_dir),
                "--out-dir",
                str(out_dir),
                "--skip-ref",
            ]
            with mock.patch.object(run_all, "run", side_effect=fake_run), \
                 mock.patch.object(sys, "argv", argv):
                run_all.main()

            compare_command = commands[-1]
            self.assertEqual(compare_command.count("--stem"), 1)
            self.assertIn("selected", compare_command)
            self.assertNotIn("unselected", compare_command)
            self.assertIn("--schema-only", compare_command)

    def test_failed_bench_run_cannot_reuse_that_stems_stale_sample(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "inputs"
            out_dir = root / "output"
            bin_dir.mkdir()
            out_dir.mkdir()
            (bin_dir / "sample.exe").write_bytes(b"fixture")
            (out_dir / "sample.nd.bench.json").write_text(
                json.dumps(
                    {
                        "functions": [
                            {"name": "old-run", "entry": "0x1000", "blocks": 1, "ops": 1}
                        ],
                        "audit_functions": [
                            {
                                "entry": "0x1000",
                                "low_ir": True,
                                "med_ir": True,
                                "med_ir_verified": True,
                                "llvm_definition": True,
                            }
                        ],
                        "total_time_ms": 1,
                    }
                ),
                encoding="utf-8",
            )
            (out_dir / "sample.ref.functions.json").write_text(
                json.dumps([{"addr": 0x1000}]), encoding="utf-8"
            )
            (out_dir / "sample.ref.imports.json").write_text("[]", encoding="utf-8")
            (out_dir / "sample.ref.strings.json").write_text("[]", encoding="utf-8")
            (out_dir / "sample.ref.timings.json").write_text(
                json.dumps({"total_ms": 100}), encoding="utf-8"
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RUN_ALL_PATH),
                    "--bin-dir",
                    str(bin_dir),
                    "--out-dir",
                    str(out_dir),
                    "--bench",
                    sys.executable,
                    "--force",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertFalse((out_dir / "sample.nd.bench.json").exists())
            progress = json.loads((out_dir / "progress.json").read_text())
            self.assertEqual(progress["sample"]["status"], "nd_failed")

    def test_no_selected_binaries_cannot_reuse_stale_bench_json(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "empty-inputs"
            out_dir = root / "output"
            bin_dir.mkdir()
            out_dir.mkdir()
            (out_dir / "stale.nd.bench.json").write_text(
                json.dumps(
                    {
                        "functions": [
                            {
                                "name": "old-run",
                                "entry": "0x1000",
                                "blocks": 1,
                                "ops": 1,
                            }
                        ],
                        "audit_functions": [
                            {
                                "entry": "0x1000",
                                "low_ir": True,
                                "med_ir": True,
                                "med_ir_verified": True,
                                "llvm_definition": True,
                            }
                        ],
                        "total_time_ms": 1,
                    }
                ),
                encoding="utf-8",
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RUN_ALL_PATH),
                    "--bin-dir",
                    str(bin_dir),
                    "--out-dir",
                    str(out_dir),
                    "--skip-ref",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("no input binaries selected", completed.stderr)

    def test_build_dir_selects_its_bin_neverd_bench(self) -> None:
        run_all = load_script("neverd_bench_run_all_build_dir", RUN_ALL_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "inputs"
            build_dir = root / "chosen-build"
            out_dir = root / "output"
            bin_dir.mkdir()
            build_dir.mkdir()
            (bin_dir / "sample.exe").write_bytes(b"fixture")

            commands: list[list[str]] = []

            def fake_run(command: list[str], timeout: int):
                commands.append(command)
                return 0, "ok", ""

            argv = [
                str(RUN_ALL_PATH),
                "--bin-dir",
                str(bin_dir),
                "--out-dir",
                str(out_dir),
                "--build-dir",
                str(build_dir),
                "--skip-ref",
            ]
            with mock.patch.object(run_all, "run", side_effect=fake_run), \
                 mock.patch.object(sys, "argv", argv):
                run_all.main()

            self.assertGreaterEqual(len(commands), 1)
            bench_name = (
                "neverd-bench.exe" if sys.platform == "win32" else "neverd-bench"
            )
            self.assertEqual(
                Path(commands[0][0]),
                build_dir.resolve() / "bin" / bench_name,
            )

    def test_explicit_bench_overrides_build_dir_default(self) -> None:
        run_all = load_script("neverd_bench_run_all_explicit_bench", RUN_ALL_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "inputs"
            build_dir = root / "ignored-build"
            explicit_bench = root / "custom" / "bench-driver"
            out_dir = root / "output"
            bin_dir.mkdir()
            (bin_dir / "sample.exe").write_bytes(b"fixture")

            commands: list[list[str]] = []

            def fake_run(command: list[str], timeout: int):
                commands.append(command)
                return 0, "ok", ""

            argv = [
                str(RUN_ALL_PATH),
                "--bin-dir",
                str(bin_dir),
                "--out-dir",
                str(out_dir),
                "--build-dir",
                str(build_dir),
                "--bench",
                str(explicit_bench),
                "--skip-ref",
            ]
            with mock.patch.object(run_all, "run", side_effect=fake_run), \
                 mock.patch.object(sys, "argv", argv):
                run_all.main()

            self.assertGreaterEqual(len(commands), 1)
            self.assertEqual(Path(commands[0][0]), explicit_bench.resolve())

    def test_compare_failure_makes_run_all_fail(self) -> None:
        run_all = load_script("neverd_bench_run_all_compare_failure", RUN_ALL_PATH)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bin_dir = root / "inputs"
            out_dir = root / "output"
            bin_dir.mkdir()
            (bin_dir / "sample.exe").write_bytes(b"fixture")

            def fake_run(command: list[str], timeout: int):
                if command[0] == sys.executable:
                    return 7, "", "no valid samples"
                return 0, "ok", ""

            argv = [
                str(RUN_ALL_PATH),
                "--bin-dir",
                str(bin_dir),
                "--out-dir",
                str(out_dir),
                "--skip-ref",
            ]
            with mock.patch.object(run_all, "run", side_effect=fake_run), \
                 mock.patch.object(sys, "argv", argv), \
                 self.assertRaises(SystemExit) as raised:
                run_all.main()

            self.assertEqual(raised.exception.code, 7)


class CompareCLITests(unittest.TestCase):
    def test_shared_string_content_below_threshold_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            write_comparable_sample(bench_dir)
            (bench_dir / "sample.nd.strings.json").write_text(
                json.dumps([{"addr": 0x1000, "content": "actual"}]),
                encoding="utf-8",
            )
            (bench_dir / "sample.ref.strings.json").write_text(
                json.dumps([{"addr": 0x1000, "content": "expected"}]),
                encoding="utf-8",
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)

    def test_missing_reference_strings_fails_address_recall(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            write_comparable_sample(bench_dir)
            (bench_dir / "sample.ref.strings.json").write_text(
                json.dumps([{"addr": 0x1000, "content": "expected"}]),
                encoding="utf-8",
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)

    def test_import_mismatch_fails_the_overall_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            write_comparable_sample(bench_dir)
            (bench_dir / "sample.ref.imports.json").write_text(
                json.dumps([{"dll": "kernel32.dll", "symbol": "CreateFileW"}]),
                encoding="utf-8",
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)

    def test_schema_only_validates_neverd_output_without_reference_results(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            write_comparable_sample(bench_dir)
            for path in bench_dir.glob("sample.ref.*.json"):
                path.unlink()

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                    "--stem",
                    "sample",
                    "--schema-only",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertIn("validated 1 benchmark sample", completed.stdout)

    def test_explicit_stem_ignores_unselected_historical_samples(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            write_comparable_sample(bench_dir, "selected")
            (bench_dir / "unselected.nd.bench.json").write_text(
                "{not-json", encoding="utf-8"
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                    "--stem",
                    "selected",
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertNotIn("unselected", completed.stderr)

    def test_failed_comparison_thresholds_return_nonzero(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            report = bench_dir / "report.md"
            write_comparable_sample(bench_dir, nd_ms=200, ref_ms=100)

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                    "--out",
                    str(report),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("**Overall pass:** 0/1", report.read_text(encoding="utf-8"))

    def test_one_invalid_sample_makes_a_mixed_comparison_fail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            report = bench_dir / "report.md"
            write_comparable_sample(bench_dir, "good")
            (bench_dir / "broken.nd.bench.json").write_text(
                "{not-json", encoding="utf-8"
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                    "--out",
                    str(report),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("invalid benchmark sample broken", completed.stderr)
            self.assertTrue(report.exists())

    def test_consolidated_bench_json_drives_current_report(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            report = bench_dir / "report.md"
            (bench_dir / "sample.nd.bench.json").write_text(
                json.dumps(
                    {
                        "func_count": 2,
                        "low_time_ms": 10,
                        "total_time_ms": 20,
                        "functions": [
                            {"name": "first", "entry": "0x1000", "blocks": 1, "ops": 2},
                            {"name": "second", "entry": "0x2000", "blocks": 1, "ops": 3},
                        ],
                        "audit_functions": [
                            {
                                "entry": "0x1000",
                                "low_ir": True,
                                "med_ir": True,
                                "med_ir_verified": True,
                                "llvm_definition": True,
                            },
                            {
                                "entry": "0x2000",
                                "low_ir": True,
                                "med_ir": True,
                                "med_ir_verified": True,
                                "llvm_definition": True,
                            },
                        ],
                    }
                ),
                encoding="utf-8",
            )
            (bench_dir / "sample.nd.imports.json").write_text("[]", encoding="utf-8")
            (bench_dir / "sample.nd.strings.json").write_text("[]", encoding="utf-8")
            (bench_dir / "sample.ref.functions.json").write_text(
                json.dumps([{"addr": 0x1000}, {"addr": 0x2000}]),
                encoding="utf-8",
            )
            (bench_dir / "sample.ref.imports.json").write_text("[]", encoding="utf-8")
            (bench_dir / "sample.ref.strings.json").write_text("[]", encoding="utf-8")
            (bench_dir / "sample.ref.timings.json").write_text(
                json.dumps({"total_ms": 100}), encoding="utf-8"
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                    "--out",
                    str(report),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            text = report.read_text(encoding="utf-8")
            self.assertIn("**Binaries:** 1", text)
            self.assertIn("| sample | 2 | 2 | 100.0% | 100.0%", text)
            self.assertIn("| 20 | 100 | 5.0x |", text)

    def test_empty_bench_directory_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    directory,
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("no .nd.bench.json samples", completed.stderr)

    def test_only_invalid_bench_samples_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            (bench_dir / "broken.nd.bench.json").write_text(
                "{not-json", encoding="utf-8"
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("no valid .nd.bench.json samples", completed.stderr)

    def test_zero_function_bench_sample_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            bench_dir = Path(directory)
            (bench_dir / "empty.nd.bench.json").write_text(
                json.dumps(
                    {
                        "functions": [],
                        "audit_functions": [],
                        "total_time_ms": 1,
                    }
                ),
                encoding="utf-8",
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(COMPARE_PATH),
                    "--bench-dir",
                    str(bench_dir),
                ],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("no valid .nd.bench.json samples", completed.stderr)


if __name__ == "__main__":
    unittest.main()
