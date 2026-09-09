"""Host-independent mutations of the real Objective-C acceptance runner.

Only its external command boundary is simulated. Inventory validation, behavior
comparison, requested-matrix accounting and evidence publication run normally.
These tests are executed in GitHub Actions, alongside the actual macOS corpus.
"""
from __future__ import annotations

from collections import Counter
from contextlib import redirect_stderr, redirect_stdout
import errno
import importlib.util
import io
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "neverd_objc_acceptance_runner",
    Path(__file__).resolve().parents[1] / "test_mobile_ios_backend.py",
)
backend = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backend)


class ObjCBackendAcceptanceTests(unittest.TestCase):
    def verify_matrix(self, work, *, arch="all", fixups="both", unavailable=None,
                      error_number=errno.ENOEXEC, failed_program="original",
                      mutation=None, missing=None, changed_program=None,
                      setup_only=False, attempts=None):
        attempts = [] if attempts is None else attempts
        compiler = str(work / "mock-clang")
        arguments = SimpleNamespace(arch=arch, fixups=fixups, timeout=30,
                                    setup_only=setup_only, neverd=work / "mock-neverd")
        expected = backend.expected_results()

        def tool(argv, *, timeout=120):
            if argv[0] == compiler:
                output = Path(argv[argv.index("-o") + 1])
                output.write_bytes(struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0,
                                               2, 0, 0, 0, 0))
                return ""
            if len(argv) > 1 and argv[1] == "mobile":
                output = Path(argv[argv.index("-o") + 1])
                output.mkdir()
                (output / "metadata").mkdir()
                (output / "sources").mkdir()
                rows = [{"class_name": backend.CLASS_NAME, "selector": selector,
                         "class_method": class_method, "status": "recovered"}
                        for selector, class_method in sorted(backend.METHODS)]
                coverage = {"schema_version": 1, "status": "recovered",
                            "method_count": len(rows), "recovered_method_count": len(rows),
                            "unrecovered_method_count": 0, "methods": rows}
                if mutation:
                    mutation(coverage)
                report = {"status": "success", "platform": "ios",
                          "architecture": output.parent.name.split("-", 1)[0],
                          "objc_method_recovery": coverage}
                if missing != "report":
                    (output / "report.json").write_text(json.dumps(report))
                if missing != "coverage":
                    (output / "metadata/objc-methods.json").write_text(json.dumps(coverage))
                if missing != "source":
                    (output / "sources/objc.m").write_text("// mocked compiler input\n")
                return ""
            executable = Path(argv[0])
            if executable.name not in ("original", "rebuilt") or len(argv) != 1:
                raise AssertionError(f"Unexpected external command: {argv!r}")
            label = executable.parent.name
            attempts.append((label, executable.name))
            if label.startswith(f"{unavailable}-") and executable.name == failed_program:
                raise OSError(error_number, "unsupported executable architecture")
            values = dict(expected)
            if executable.name == changed_program:
                values["constant42:0"] += 1
            return "\n".join(f"{key}={value}" for key, value in values.items()) + "\n"

        stdout = io.StringIO()
        with patch.object(backend.sys, "platform", "darwin"), \
                patch.object(backend.platform, "machine", return_value="arm64"), \
                patch.object(backend.shutil, "which", return_value=compiler), \
                patch.object(backend, "run", side_effect=tool), \
                redirect_stdout(stdout), redirect_stderr(io.StringIO()):
            backend.verify(arguments, work)
        return attempts, stdout.getvalue()

    def test_every_explicit_requested_matrix_must_execute_originals_and_rebuilt(self):
        for arch in ("all", "arm64", "x86_64"):
            for fixups in ("both", "classic", "default"):
                with self.subTest(arch=arch, fixups=fixups), tempfile.TemporaryDirectory() as temporary:
                    work = Path(temporary)
                    attempts, stdout = self.verify_matrix(work, arch=arch, fixups=fixups)
                    architectures = ("arm64", "x86_64") if arch == "all" else (arch,)
                    variants = ("classic", "default") if fixups == "both" else (fixups,)
                    labels = sorted(f"{architecture}-{fixup}"
                                    for architecture in architectures for fixup in variants)
                    self.assertEqual(Counter(attempts), Counter((label, program)
                                     for label in labels for program in ("original", "rebuilt")))
                    self.assertIn(f"Verified {len(labels)} recovered variants", stdout)
                    self.assertNotIn("SKIP", stdout)
                    summary = json.loads((work / "acceptance.json").read_text())
                    self.assertEqual(summary["status"], "success")
                    self.assertEqual(summary["mode"], "recovered")
                    self.assertEqual(summary["requested"], labels)
                    self.assertEqual(summary["completed"], labels)
                    self.assertEqual(summary["failures"], {})
                    self.assertEqual((work / "original-fixture.m").read_text(), backend.FIXTURE.read_text())
                    for label in labels:
                        for program in ("original", "rebuilt"):
                            path = work / label / f"{program}-results.json"
                            self.assertEqual(json.loads(path.read_text()), backend.expected_results())

    def test_unexecutable_original_or_rebuilt_cannot_shrink_all_both(self):
        for error_number in (errno.ENOEXEC, 86):
            for failed_program in ("original", "rebuilt"):
                with self.subTest(error_number=error_number, program=failed_program), \
                        tempfile.TemporaryDirectory() as temporary:
                    work = Path(temporary)
                    attempts = []
                    with self.assertRaisesRegex(RuntimeError, r"2/4 completed.*\n.*cannot execute"):
                        self.verify_matrix(work, unavailable="x86_64", error_number=error_number,
                                           failed_program=failed_program, attempts=attempts)
                    summary = json.loads((work / "acceptance.json").read_text())
                    self.assertEqual(summary["status"], "error")
                    self.assertEqual(summary["completed"], ["arm64-classic", "arm64-default"])
                    self.assertEqual(set(summary["failures"]), {"x86_64-classic", "x86_64-default"})
                    for fixup in ("classic", "default"):
                        label = f"x86_64-{fixup}"
                        self.assertIn((label, failed_program), attempts)
                        self.assertTrue((work / label / "original").is_file())
                        self.assertIn("cannot execute", (work / label / "failure.txt").read_text())

    def test_zero_completed_variants_is_failure_even_for_an_explicit_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            with self.assertRaisesRegex(RuntimeError, r"0/1 completed.*\n.*cannot execute"):
                self.verify_matrix(work, arch="x86_64", fixups="default", unavailable="x86_64")
            summary = json.loads((work / "acceptance.json").read_text())
            self.assertEqual(summary["completed"], [])
            self.assertEqual(summary["requested"], ["x86_64-default"])
            self.assertEqual(summary["status"], "error")

    def test_empty_or_duplicated_requested_matrix_is_not_a_vacuous_pass(self):
        for variants in ([], [("arm64", "classic"), ("arm64", "classic")]):
            with self.subTest(variants=variants), tempfile.TemporaryDirectory() as temporary, \
                    patch.object(backend, "requested_variants", return_value=variants):
                with self.assertRaisesRegex(RuntimeError, "matrix is empty or duplicated"):
                    self.verify_matrix(Path(temporary))

    def test_empty_method_or_behavior_oracle_cannot_pass(self):
        for name, value in (("METHODS", set()), ("expected_results", lambda: {})):
            with self.subTest(oracle=name), tempfile.TemporaryDirectory() as temporary, \
                    patch.object(backend, name, value):
                with self.assertRaisesRegex(RuntimeError, "no required behavior or method inventory"):
                    self.verify_matrix(Path(temporary))

    def test_partial_coverage_is_not_a_completed_variant(self):
        def partial(coverage):
            coverage["status"] = "partial"
            coverage["recovered_method_count"] -= 1
            coverage["unrecovered_method_count"] = 1
            coverage["methods"][0]["status"] = "unrecovered"

        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            with self.assertRaisesRegex(RuntimeError, r"0/4 completed.*\n.*Incomplete Objective-C method coverage"):
                self.verify_matrix(work, mutation=partial)
            summary = json.loads((work / "acceptance.json").read_text())
            self.assertEqual(len(summary["failures"]), 4)
            self.assertTrue((work / "x86_64-default/recovered/report.json").is_file())

    def test_omitted_method_cannot_shrink_the_independent_denominator(self):
        def omitted(coverage):
            coverage["methods"].pop()
            coverage["method_count"] -= 1
            coverage["recovered_method_count"] -= 1

        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "omitted or duplicated fixture methods"):
                self.verify_matrix(Path(temporary), arch="arm64", fixups="classic", mutation=omitted)

    def test_method_role_change_cannot_pass_with_unchanged_counts(self):
        def role_changed(coverage):
            row = coverage["methods"][0]
            row["class_method"] = not row["class_method"]

        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "omitted or duplicated fixture methods"):
                self.verify_matrix(Path(temporary), arch="arm64", fixups="classic", mutation=role_changed)

    def test_absent_report_coverage_or_source_is_never_complete(self):
        for missing in ("report", "coverage", "source"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                with self.assertRaisesRegex(RuntimeError, "0/1 completed"):
                    self.verify_matrix(work, arch="arm64", fixups="classic", missing=missing)
                summary = json.loads((work / "acceptance.json").read_text())
                self.assertEqual(summary["status"], "error")
                self.assertEqual(summary["completed"], [])
                self.assertTrue((work / "arm64-classic/original-results.json").is_file())
                self.assertTrue((work / "arm64-classic/failure.txt").is_file())

    def test_behavior_mismatch_retains_the_failing_result_and_original_input(self):
        for program in ("original", "rebuilt"):
            with self.subTest(program=program), tempfile.TemporaryDirectory() as temporary:
                work = Path(temporary)
                with self.assertRaisesRegex(RuntimeError, "changed fixture behavior"):
                    self.verify_matrix(work, arch="arm64", fixups="default", changed_program=program)
                actual = json.loads((work / f"arm64-default/{program}-results.json").read_text())
                self.assertEqual(actual["constant42:0"], 43)
                self.assertTrue((work / "original-fixture.m").is_file())
                self.assertEqual(json.loads((work / "acceptance.json").read_text())["status"], "error")

    def test_setup_only_is_explicitly_original_evidence_not_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            attempts, stdout = self.verify_matrix(work, arch="arm64", fixups="classic", setup_only=True)
            self.assertEqual(attempts, [("arm64-classic", "original")])
            self.assertIn("Verified 1 original-only variants", stdout)
            summary = json.loads((work / "acceptance.json").read_text())
            self.assertEqual(summary["mode"], "original-only")
            self.assertFalse((work / "arm64-classic/recovered").exists())


if __name__ == "__main__":
    unittest.main()
