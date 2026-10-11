import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_entry as runner
from scripts import replay_windows_registration_entry as replay
from scripts.tests import test_check_windows_registration_realigned_rewrite as fixtures
from scripts.tests.test_windows_registration_runtime import runtime_fixture
from scripts import windows_registration_runtime as runtime


class EntryEvidenceTests(unittest.TestCase):
    def capture(self, root):
        with patch.multiple(fixtures.runner, FORMS=runner.FORMS, CASES=runner.CASES,
                            SOURCE=runner.SOURCE, EMITTER=runner.PROOF,
                            source_frame=lambda _: "fixed-displaced", proof_count=lambda _: 1):
            result = fixtures.RealignedRewriteEvidenceTests().capture(root)
        result.update(evidence="entry-abi-source-reconstruction",
                      proof_sha256=runner.file_digest(runner.PROOF),
                      receipt_sha256=runner.file_digest(runner.RECEIPT))
        dll = root / "runtime/x86/Microsoft.VC143.CRT" / runtime.NAME
        dll.parent.mkdir(parents=True)
        dll.write_bytes(runtime_fixture())
        result["catch_search_runtime"] = runtime.capture_runtime(
            root / "runtime", root / "runtime", "14.44")
        for case in result["cases"]:
            parent = root / case["case"]
            (parent / runtime.NAME).write_bytes(runtime_fixture())
            registers, pop, export = runner.entry_context(case["case"])
            original = bytearray((parent / "original.exe").read_bytes())
            product = bytearray((parent / "product.exe").read_bytes())
            for data in (original, product):
                data[0x350:0x350 + len(export) + 1] = export + b"\0"
            receipt_path = parent / "contract.json"
            receipt = json.loads(receipt_path.read_text())
            receipt.update(entry_registers=registers, entry_pop=pop,
                           incoming_reads=3, incoming_writes=0,
                           source_image_sha256=hashlib.sha256(original).hexdigest(),
                           image_sha256=hashlib.sha256(product).hexdigest())
            receipt_path.write_text(json.dumps(receipt))
            case["contract_sha256"] = runner.file_digest(receipt_path)
            (parent / "driver.obj").write_bytes(case["case"].encode())
            case["object_sha256"] = runner.file_digest(parent / "driver.obj")
            for record in case["images"]:
                image = runner.PE32(original if record["route"] == "original" else product)
                data = image.data if record["base"] == image.base else image.rebase(record["base"])
                path = parent / record["image"]
                path.write_bytes(data)
                record["sha256"] = runner.file_digest(path)
            case["decompilation"] = {}
            for language in ("c", "cpp"):
                text = "highir.structured_regions=2, fallback_regions=0\n"
                for index in range(3):
                    text += f"= __neverd_x86_callback_esp(0x{index:X}); goto L_{index:X};\n"
                    text += "Native x86 callback @\n" if language == "c" else "catch (\n"
                if language == "cpp":
                    text += "try { try {\n"
                path = parent / ("decompiled." + language)
                path.write_text(text)
                case["decompilation"][language] = runner.file_digest(path)
        return result

    def test_requires_all_conventions_optimizations_routes_and_proofs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 128)
            for mutation in range(14):
                changed = copy.deepcopy(capture)
                case = changed["cases"][0]
                if mutation == 0:
                    changed["passed"] = False
                elif mutation < 4:
                    changed[("source_sha256", "proof_sha256", "receipt_sha256")[mutation - 1]] = "stale"
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
                    case["decompilation"].pop("cpp")
                elif mutation == 12:
                    changed["catch_search_runtime"]["sha256"] = "a" * 64
                else:
                    case["ir_sha256"] = "stale"
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)

    def test_physical_stack_and_register_contract_cannot_be_substituted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = next(c for c in capture["cases"] if c["case"] == "fastcall-o0")
            path = root / case["case"] / "contract.json"
            receipt = json.loads(path.read_text())
            for key, value in (("entry_pop", 12), ("entry_registers", 1),
                               ("incoming_reads", 0), ("source_frame", "direct")):
                changed = dict(receipt) | {key: value}
                path.write_text(json.dumps(changed))
                case["contract_sha256"] = runner.file_digest(path)
                with self.subTest(key=key), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture)

    def test_proof_must_run_without_skips(self):
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

    def test_rebased_executable_must_equal_the_captured_transaction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = capture["cases"][0]
            record = next(r for r in case["images"] if r["image"] == "cli-inplace-rebased.exe")
            path = root / case["case"] / record["image"]
            data = bytearray(path.read_bytes())
            data[0x680] ^= 1
            path.write_bytes(data)
            record["sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_runtime_checks_arguments_stack_balance_chain_and_caller_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = capture["cases"][0]
            parent = root / case["case"]
            receipt = json.loads((parent / "contract.json").read_text())
            values = list(runner.EXPECTED) + [runner.BASES[0] + 0x1004] * 3
            def output(v):
                return "ENTRY " + " ".join(f"{item:08X}" for item in v) + "\n"
            with patch.object(runner, "run_image", return_value={"exit_code": 0, "stdout": output(values)}):
                runner.observe(parent / "original.exe", case["case"], "original", receipt, [], {}, 1)
            for index in range(14):
                changed = values.copy()
                changed[index] = 0
                with self.subTest(index=index), patch.object(
                        runner, "run_image", return_value={"exit_code": 0, "stdout": output(changed)}):
                    with self.assertRaises(ValueError):
                        runner.observe(parent / "original.exe", case["case"], "original", receipt, [], {}, 1)
            for text in (output(values) * 2, output(values)[:-2], "noise\n" + output(values)):
                with patch.object(runner, "run_image", return_value={"exit_code": 0, "stdout": text}):
                    with self.assertRaises(ValueError):
                        runner.observe(parent / "original.exe", case["case"], "original", receipt, [], {}, 1)


if __name__ == "__main__":
    unittest.main()
