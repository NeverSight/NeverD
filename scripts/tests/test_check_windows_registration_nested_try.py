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
        result["rethrow_proof_sha256"] = runner.file_digest(runner.RETHROW_PROOF)
        result["direct_proof_sha256"] = runner.file_digest(runner.DIRECT_PROOF)
        result["catch_proof_sha256"] = runner.file_digest(runner.CATCH_PROOF)
        result["receipt_proof_sha256"] = runner.file_digest(runner.RECEIPT_PROOF)
        result["cleanup_proof_sha256"] = runner.file_digest(runner.CLEANUP_PROOF)
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
            receipt.update(runner.search_context(case["case"]))
            receipt_path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(receipt_path)
            (parent / "driver.obj").write_bytes(case["case"].encode())
            case["object_sha256"] = runner.file_digest(parent / "driver.obj")
            case["decompilation"] = {}
            for language in ("c", "cpp"):
                regions, callbacks = (3, 4) if receipt["catch_try"] else (2, 3)
                text = f"highir.structured_regions={regions}, fallback_regions=0\n"
                for index in range(callbacks):
                    text += f"= __neverd_x86_callback_esp(0x{index:X}); goto L_{index:X};\n"
                    text += "Native x86 callback @\n" if language == "c" else "catch (\n"
                if language == "cpp":
                    text += "try { " * regions + "\n"
                    if receipt["inline_rethrow"]:
                        text += "throw;\n"
                    if receipt["direct_throw"]:
                        text += "throw (int)value; throw (unsigned int)value; throw (float)value;\n"
                if receipt["catch_cleanup"]:
                    for index in range(2 if "-o0-" in case["case"] else 1):
                        text += (f"/* Native x86 cleanup @ 0x401{index}00; state={index}, "
                                 "to-state=-1, object-offset=0 */\n" if language == "cpp" else
                                 f" * cleanup @ 0x0, action @ 0x401{index}00, "
                                 f"type descriptor @ 0x0, state={index}, catch-object offset=0\n")
                source = parent / ("decompiled." + language)
                source.write_text(text)
                case["decompilation"][language] = runner.file_digest(source)
        return result

    def test_matrix_requires_current_proofs_objects_and_routes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 288)
            for mutation in range(22):
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
                elif mutation == 16:
                    changed["catch_search_runtime"]["sha256"] = "a" * 64
                elif mutation == 17:
                    changed["rethrow_proof_sha256"] = "stale"
                elif mutation == 18:
                    changed["direct_proof_sha256"] = "stale"
                elif mutation == 19:
                    changed["catch_proof_sha256"] = "stale"
                elif mutation == 20:
                    changed["receipt_proof_sha256"] = "stale"
                else:
                    changed["cleanup_proof_sha256"] = "stale"
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

    def test_rethrow_cannot_be_substituted_with_a_new_exception(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = next(c for c in capture["cases"] if c["case"].startswith("rethrow-"))
            path = root / case["case"] / "contract.json"
            receipt = json.loads(path.read_text())
            receipt["rethrow_search"] = False
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_cleanup_decompilation_keeps_every_native_action(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = next(c for c in capture["cases"] if c["case"].startswith("catch-cleanup-o0-"))
            for language in ("c", "cpp"):
                source = root / case["case"] / ("decompiled." + language)
                original = source.read_text()
                for changed in (original.replace("state=0", "missing=0"),
                                original.replace("401100", "401000").replace("state=1", "state=0"),
                                original + "__unwind {}\n"):
                    source.write_text(changed)
                    case["decompilation"][language] = runner.file_digest(source)
                    with self.subTest(language=language, source=changed), self.assertRaises(ValueError):
                        replay.validate_capture(root, capture)
                source.write_text(original)
                case["decompilation"][language] = runner.file_digest(source)

    def test_inline_rethrow_requires_its_argument_proof_and_spelling(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = next(c for c in capture["cases"] if c["case"].startswith("inline-rethrow-"))
            path = root / case["case"] / "contract.json"
            receipt = json.loads(path.read_text())
            receipt["inline_rethrow"] = False
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)
            receipt["inline_rethrow"] = True
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            source = root / case["case"] / "decompiled.cpp"
            source.write_text(source.read_text().replace("throw;", "throw 18;"))
            case["decompilation"]["cpp"] = runner.file_digest(source)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_direct_throw_cannot_lose_its_source_objects_or_output_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = next(c for c in capture["cases"] if c["case"].startswith("direct-nested-"))
            path = root / case["case"] / "contract.json"
            receipt = json.loads(path.read_text())
            receipt["direct_throw"] = False
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)
            receipt["direct_throw"] = True
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            source = root / case["case"] / "decompiled.cpp"
            source.write_text(source.read_text().replace("throw (unsigned int)value;", "throw;"))
            case["decompilation"]["cpp"] = runner.file_digest(source)
            with self.assertRaises(ValueError):
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

    def test_catch_try_cannot_be_substituted_with_an_ordinary_nested_search(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = next(c for c in capture["cases"] if c["case"].startswith("catch-try-"))
            path = root / case["case"] / "contract.json"
            receipt = json.loads(path.read_text())
            receipt["catch_try"] = False
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)
            receipt["catch_try"] = True
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            source = root / case["case"] / "decompiled.cpp"
            original = source.read_text()
            for text in (original.replace("regions=3", "regions=2"),
                         original.replace("try {", "", 1),
                         original.replace("catch (", "", 1),
                         original.replace("goto L_3;", "")):
                source.write_text(text)
                case["decompilation"]["cpp"] = runner.file_digest(source)
                with self.subTest(text=text), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture)

    def test_cleanup_receipt_cannot_drop_its_destruction_oracle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = next(c for c in capture["cases"] if c["case"].startswith("catch-cleanup-"))
            path = root / case["case"] / "contract.json"
            receipt = json.loads(path.read_text())
            receipt["catch_cleanup"] = False
            path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)


if __name__ == "__main__":
    unittest.main()
