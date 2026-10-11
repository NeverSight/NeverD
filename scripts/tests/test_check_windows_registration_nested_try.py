import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_nested_try as runner
from scripts import replay_windows_registration_nested_try as replay
from scripts.tests import test_check_windows_registration_realigned_rewrite as fixtures
from scripts.tests.test_windows_registration_runtime import runtime_fixture
from scripts import windows_registration_runtime as runtime


class NestedTryEvidenceTests(unittest.TestCase):
    def capture(self, root):
        with patch.multiple(fixtures.runner, FORMS=runner.FORMS, CASES=runner.CASES,
                            SOURCE=runner.SOURCE, EMITTER=runner.EMITTER,
                            proof_count=lambda name: 1):
            result = fixtures.RealignedRewriteEvidenceTests().capture(root)
        result["evidence"] = "nested-try-source-reconstruction"
        result["proof_sha256"] = runner.file_digest(runner.PROOF)
        source = root / "runtime/x86/Microsoft.VC143.CRT" / runtime.NAME
        source.parent.mkdir(parents=True)
        source.write_bytes(runtime_fixture())
        result["catch_search_runtime"] = runtime.capture_runtime(
            root / "runtime", root / "runtime", "14.44")
        for case in result["cases"]:
            parent = root / case["case"]
            (parent / runtime.NAME).write_bytes(runtime_fixture())
            receipt_path = parent / "contract.json"
            receipt = json.loads(receipt_path.read_text())
            receipt["secondary_search"] = case["case"].startswith("secondary-")
            receipt_path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(receipt_path)
            (parent / "driver.obj").write_bytes(case["case"].encode())
            case["object_sha256"] = runner.file_digest(parent / "driver.obj")
            case["decompilation"] = {}
            for language in ("c", "cpp"):
                text = "highir.structured_regions=2, fallback_regions=0\n"
                for index in range(3):
                    text += f"= __neverd_x86_callback_esp(0x{index:X}); goto L_{index:X};\n"
                    text += "Native x86 callback @\n" if language == "c" else "catch (\n"
                if language == "cpp":
                    text += "try { try {\n"
                source = parent / ("decompiled." + language)
                source.write_text(text)
                case["decompilation"][language] = runner.file_digest(source)
        return result

    def test_matrix_requires_current_proofs_objects_and_routes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 64)
            for mutation in range(17):
                changed = copy.deepcopy(capture)
                case = changed["cases"][0]
                if mutation == 0:
                    changed["passed"] = False
                elif mutation == 1:
                    changed["source_sha256"] = "stale"
                elif mutation == 2:
                    changed["emitter_sha256"] = "stale"
                elif mutation == 3:
                    changed["proof_sha256"] = "stale"
                elif mutation == 4:
                    changed["cases"].pop()
                elif mutation == 5:
                    changed["cases"][-1] = changed["cases"][0]
                elif mutation == 6:
                    case["images"].pop()
                elif mutation == 7:
                    case["images"][-1] = case["images"][0]
                elif mutation == 8:
                    case["images"][0]["image"] = "../original.exe"
                elif mutation == 9:
                    case["images"][0]["expected_exit"] = 1
                elif mutation == 10:
                    case["object_sha256"] = "stale"
                elif mutation == 11:
                    case["contract_sha256"] = "stale"
                elif mutation == 12:
                    case["ir_sha256"] = "stale"
                elif mutation == 13:
                    case["decompilation"].pop("cpp")
                elif mutation == 14:
                    changed["runtime_libraries"]["architecture"] = "x64"
                elif mutation == 15:
                    changed.pop("catch_search_runtime")
                else:
                    changed["catch_search_runtime"]["sha256"] = "a" * 64
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            runtime_path = root / runner.CASES[0] / runtime.NAME
            runtime_path.write_bytes(runtime_fixture() + b"changed")
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)
            runtime_path.write_bytes(runtime_fixture())
            path = root / runner.CASES[0] / "cli-inplace-rebased.exe"
            data = bytearray(path.read_bytes())
            data[0x680] ^= 1
            path.write_bytes(data)
            record = next(r for r in capture["cases"][0]["images"] if r["image"] == path.name)
            record["sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_reconstruction_proof_cannot_be_skipped_or_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            path = root / runner.CASES[0] / "rewrite.xml"
            for xml in ('<testsuites tests="0"/>', '<testsuites tests="2"/>',
                        '<testsuites tests="1" failures="1"/>',
                        '<testsuites tests="1"><testcase><skipped/></testcase></testsuites>'):
                path.write_text(xml)
                with self.subTest(xml=xml), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture)

    def test_decompilation_keeps_both_tries_and_all_callback_returns(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = capture["cases"][0]
            path = root / case["case"] / "decompiled.cpp"
            original = path.read_text()
            variants = [original.replace("regions=2", "regions=1"),
                        original.replace("fallback_regions=0", "fallback_regions=1"),
                        original.replace("try {", "", 1),
                        original.replace("catch (", "", 1),
                        original.replace("goto L_2;", ""),
                        original.replace("__neverd_x86_callback_esp", "missing", 1),
                        original + "e.Value"]
            for text in variants:
                path.write_text(text)
                case["decompilation"]["cpp"] = runner.file_digest(path)
                with self.subTest(text=text), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture)


if __name__ == "__main__":
    unittest.main()
