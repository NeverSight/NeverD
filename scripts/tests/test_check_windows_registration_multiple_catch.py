import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_multiple_catch as runner
from scripts import replay_windows_registration_multiple_catch as replay
from scripts.tests import test_check_windows_registration_realigned_rewrite as fixtures


class MultipleCatchEvidenceTests(unittest.TestCase):
    def capture(self, root):
        # Share the synthetic PE layout, while keeping this suite's exact
        # source, object and profile identities independent of the single catch.
        with patch.multiple(fixtures.runner, FORMS=runner.FORMS, CASES=runner.CASES,
                            SOURCE=runner.SOURCE, EMITTER=runner.EMITTER,
                            proof_count=lambda name: 1):
            result = fixtures.RealignedRewriteEvidenceTests().capture(root)
        result["evidence"] = "ordered-catch-source-reconstruction"
        for record in result["cases"]:
            record["decompilation"] = {}
            for language in ("c", "cpp"):
                source = root / record["case"] / ("decompiled." + language)
                text = "highir.structured_regions=1, fallback_regions=0\n"
                for index in range(3):
                    text += f"= __neverd_x86_callback_esp(0x{index:X}); goto L_{index:X};\n"
                    text += "Native x86 callback @\n" if language == "c" else "catch (\n"
                source.write_text(text)
                record["decompilation"][language] = runner.file_digest(source)
        return result

    def test_exact_matrix_and_file_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 32)
            for mutation in range(12):
                changed = copy.deepcopy(capture)
                case = changed["cases"][0]
                if mutation == 0:
                    changed["passed"] = False
                elif mutation == 1:
                    changed["source_sha256"] = "stale"
                elif mutation == 2:
                    changed["emitter_sha256"] = "stale"
                elif mutation == 3:
                    changed["cases"].pop()
                elif mutation == 4:
                    changed["cases"][-1] = changed["cases"][0]
                elif mutation == 5:
                    case["images"].pop()
                elif mutation == 6:
                    case["images"][-1] = case["images"][0]
                elif mutation == 7:
                    case["images"][0]["image"] = "../original.exe"
                elif mutation == 8:
                    case["images"][0]["expected_exit"] = 1
                elif mutation == 9:
                    changed["objects"]["ordered"] = "stale"
                elif mutation == 10:
                    case["contract_sha256"] = "stale"
                else:
                    case["ir_sha256"] = "stale"
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            path = root / runner.CASES[0] / "cli-section-rebased.exe"
            data = bytearray(path.read_bytes())
            data[0x680] ^= 1
            path.write_bytes(data)
            record = next(r for r in capture["cases"][0]["images"] if r["image"] == path.name)
            record["sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_proofs_must_execute_without_skips(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            for path in (root / "ordered-emit.xml", root / "ordered/rewrite.xml"):
                for xml in ('<testsuites tests="0"/>', '<testsuites tests="2"/>',
                            '<testsuites tests="1" failures="1"/>',
                            '<testsuites tests="1"><testcase><skipped/></testcase></testsuites>'):
                    path.write_text(xml)
                    with self.subTest(path=path, xml=xml), self.assertRaises(ValueError):
                        replay.validate_capture(root, capture)
                path.write_text('<testsuites tests="1"/>')

    def test_changed_or_incomplete_decompilation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            path = root / "ordered/decompiled.cpp"
            path.write_text(path.read_text() + "e.Value")
            record = next(c for c in capture["cases"] if c["case"] == "ordered")
            record["decompilation"]["cpp"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_runtime_observations_bind_every_clause_and_caller(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.capture(root)
            path = root / "ordered/original.exe"
            contract = json.loads((path.parent / "contract.json").read_text())
            correct = [17, 28, 39, 7, 18, 39, 1, 12, 0x401010, 0x401020, 0x401030]
            def runtime(values, exit_code=0):
                return {"exit_code": exit_code,
                        "stdout": "MULTICATCH " + " ".join(f"{v:08X}" for v in values) + "\n"}
            with patch.object(runner, "run_image", return_value=runtime(correct)):
                runner.observe(path, "ordered", "original", contract, [], {}, 1)
            for index in range(len(correct)):
                changed = correct.copy()
                changed[index] = 0
                with self.subTest(index=index), patch.object(runner, "run_image", return_value=runtime(changed)), self.assertRaises(ValueError):
                    runner.observe(path, "ordered", "original", contract, [], {}, 1)
            with patch.object(runner, "run_image", return_value=runtime(correct)), self.assertRaises(ValueError):
                runner.observe(path, "ordered-control", "original", contract, [], {}, 1)


if __name__ == "__main__":
    unittest.main()
