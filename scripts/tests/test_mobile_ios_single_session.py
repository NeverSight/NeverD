"""Mutate single-session evidence; only metadata-only process execution is mocked."""
from __future__ import annotations

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "neverd_ios_single_session",
    Path(__file__).resolve().parents[1] / "test_mobile_ios_single_session.py",
)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

TRACE = (
    "[neverd-child-phase] phase=session_load event=begin iteration=0 elapsed_ms=0\n"
    "[neverd-child-phase] phase=session_load event=completed iteration=0 elapsed_ms=5\n"
    "[neverd-child-phase] phase=objc_export event=begin iteration=0 elapsed_ms=0\n"
    "[neverd-child-phase] phase=pipeline event=begin iteration=0 elapsed_ms=0\n"
    "[neverd-child-phase] phase=pipeline event=completed iteration=0 elapsed_ms=7\n"
    "[neverd-child-phase] phase=objc_export event=completed iteration=0 elapsed_ms=8\n"
)
SWIFT = {"status": "recovered", "types": [{"name": "Empty"}],
         "symbols": [{"name": "_$s4Demo5EmptyVMa", "address": "0x1000", "defined": True},
                     {"name": "_$s4Demo5EmptyVMa", "address": "0x1000", "defined": True},
                     {"name": "_$s4Demo7missingyyF", "address": "0x0", "defined": False}]}


class SingleSessionEvidenceTests(unittest.TestCase):
    def fixture(self, variant: Path, corpus="swift", architecture="arm64"):
        variant.mkdir(parents=True)
        binary = variant / ("libSwiftBehavior.dylib" if corpus == "swift" else "original")
        binary.write_bytes(b"owned-input")
        output = variant / "recovered"
        for folder in ("artifacts", "metadata", "logs"):
            (output / folder).mkdir(parents=True)
        (output / "artifacts/selected.macho").write_bytes(b"selected-thin-input")
        (output / "metadata/swift.json").write_text(json.dumps(SWIFT))
        (output / "logs/native.log").write_text(TRACE)
        report = {"schema_version": 1, "status": "success", "platform": "ios",
                  "architecture": architecture, "metadata_only": False,
                  "outputs": {"native_log": "logs/native.log"},
                  "swift_method_recovery": {"method_count": 2 if corpus == "swift" else 0}}
        if corpus == "swift":
            report["outputs"]["swift_native_log"] = "logs/native.log"
        (output / "report.json").write_text(json.dumps(report))
        return output, report

    def metadata(self, neverd, binary, output, architecture, timeout):
        self.assertFalse(output.exists())
        (output / "metadata").mkdir(parents=True)
        (output / "artifacts").mkdir()
        (output / "artifacts/selected.macho").write_bytes(b"selected-thin-input")
        (output / "metadata/swift.json").write_text(json.dumps(SWIFT))
        (output / "report.json").write_text(json.dumps({
            "schema_version": 1, "status": "success", "platform": "ios",
            "architecture": architecture, "metadata_only": True,
            "native_function_count": None, "objc_method_recovery": None,
            "swift_method_recovery": None,
        }))

    def test_all_twelve_variants_require_real_evidence_shapes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for corpus, architecture, fixup in runner.VARIANTS:
                self.fixture(root / corpus / f"{architecture}-{fixup}", corpus, architecture)
            with patch.object(runner, "run_metadata", side_effect=self.metadata) as call, \
                    redirect_stdout(io.StringIO()):
                result = runner.verify(root / "neverd", root, 30)
            self.assertEqual(call.call_count, 12)
            self.assertEqual(result["status"], "success")
            self.assertEqual(result["completed_variants"], 12)
            self.assertEqual(len({r["variant"] for r in result["results"]}), 12)
            self.assertTrue(all(r["whole_swift_metadata_equal"] for r in result["results"]))

    def test_missing_variant_fails_without_reducing_the_matrix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for corpus, architecture, fixup in runner.VARIANTS[:-1]:
                self.fixture(root / corpus / f"{architecture}-{fixup}", corpus, architecture)
            with patch.object(runner, "run_metadata", side_effect=self.metadata), \
                    redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()), \
                    self.assertRaisesRegex(RuntimeError, "11/12"):
                runner.verify(root / "neverd", root, 30)
            result = json.loads((root / "single-session.json").read_text())
            self.assertEqual(result["requested_variants"], 12)
            self.assertEqual(result["failures"][0]["variant"], "swift/x86_64-default")

    def test_duplicate_missing_failed_and_unordered_phases_are_rejected(self):
        traces = [
            TRACE + TRACE,
            TRACE.replace("phase=session_load", "phase=pipeline"),
            TRACE.replace("event=completed iteration=0 elapsed_ms=5\n", ""),
            TRACE.replace("event=completed iteration=0 elapsed_ms=5", "event=failed iteration=0 elapsed_ms=5"),
            TRACE.replace("event=completed iteration=0 elapsed_ms=8", "event=aborted iteration=0 elapsed_ms=8"),
            TRACE.replace("event=begin iteration=0 elapsed_ms=0", "event=begin iteration=0 elapsed_ms=1", 1),
            TRACE.replace("phase=pipeline", "phase=not-a-phase"),
            TRACE.replace("event=completed iteration=0 elapsed_ms=7", "event=completed iteration=1 elapsed_ms=7"),
            "".join(TRACE.splitlines(keepends=True)[0:2] +
                    TRACE.splitlines(keepends=True)[3:5] +
                    [TRACE.splitlines(keepends=True)[2], TRACE.splitlines(keepends=True)[5]]),
            TRACE[TRACE.index("[neverd-child-phase] phase=objc_export"):] + TRACE[:TRACE.index("[neverd-child-phase] phase=objc_export")],
        ]
        for trace in traces:
            with self.subTest(trace=trace), tempfile.TemporaryDirectory() as temporary:
                output, report = self.fixture(Path(temporary) / "variant")
                (output / "logs/native.log").write_text(trace)
                with self.assertRaises(RuntimeError):
                    runner.validate_trace(output, report)

    def test_missing_or_separate_export_logs_are_rejected(self):
        for mutation in ("missing-native", "different-swift", "missing-swift", "extra-log"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                output, report = self.fixture(Path(temporary) / "variant")
                if mutation == "missing-native":
                    report["outputs"].pop("native_log")
                elif mutation == "different-swift":
                    report["outputs"]["swift_native_log"] = "../native.log"
                elif mutation == "missing-swift":
                    report["outputs"].pop("swift_native_log")
                else:
                    (output / "logs/swift-native.log").write_text(TRACE)
                with self.assertRaises(RuntimeError):
                    runner.validate_trace(output, report)

    def test_whole_metadata_keeps_duplicates_undefined_rows_order_and_types(self):
        for mutation in ("duplicate", "undefined", "order", "boolean", "type"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                variant = Path(temporary) / "variant"
                output, _ = self.fixture(variant)
                changed = json.loads(json.dumps(SWIFT))
                if mutation == "duplicate":
                    changed["symbols"].pop(0)
                elif mutation == "undefined":
                    changed["symbols"].pop()
                elif mutation == "order":
                    changed["symbols"].reverse()
                elif mutation == "boolean":
                    changed["symbols"][0]["defined"] = 1
                else:
                    changed["types"][0]["name"] = "Other"
                (output / "metadata/swift.json").write_text(json.dumps(changed))
                with patch.object(runner, "run_metadata", side_effect=self.metadata), \
                        self.assertRaisesRegex(RuntimeError, "Whole Swift metadata"):
                    runner.verify_variant(variant / "neverd", variant, "swift", "arm64", 30)

    def test_binary_drift_and_metadata_only_false_success_fail(self):
        for mutation in ("selected", "original", "metadata-status", "metadata-count"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                variant = Path(temporary) / "variant"
                self.fixture(variant)

                def invoke(*args):
                    self.metadata(*args)
                    _, binary, output, _, _ = args
                    if mutation == "selected":
                        (output / "artifacts/selected.macho").write_bytes(b"different")
                    elif mutation == "original":
                        binary.write_bytes(b"changed original")
                    else:
                        path = output / "report.json"
                        report = json.loads(path.read_text())
                        report["status" if mutation == "metadata-status" else "native_function_count"] = "success" if mutation == "metadata-count" else "error"
                        path.write_text(json.dumps(report))

                with patch.object(runner, "run_metadata", side_effect=invoke), \
                        self.assertRaises(RuntimeError):
                    runner.verify_variant(variant / "neverd", variant, "swift", "arm64", 30)

    def test_preexisting_metadata_evidence_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as temporary:
            variant = Path(temporary) / "variant"
            self.fixture(variant)
            (variant / "metadata-only").mkdir()
            with patch.object(runner, "run_metadata") as call, \
                    self.assertRaisesRegex(RuntimeError, "already exists"):
                runner.verify_variant(variant / "neverd", variant, "swift", "arm64", 30)
            call.assert_not_called()


if __name__ == "__main__":
    unittest.main()
