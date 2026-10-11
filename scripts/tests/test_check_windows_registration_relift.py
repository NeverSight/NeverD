import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_entry as entry
from scripts import check_windows_registration_relift as runner
from scripts import replay_windows_registration_relift as replay
from scripts.tests import test_check_windows_registration_entry as first_fixture
from scripts.tests import test_check_windows_registration_nested_try as nested_fixture
from scripts import check_windows_registration_multiple_catch as catches


class ReliftEvidenceTests(unittest.TestCase):
    def capture(self, root, profile_name="entry"):
        profile = runner.get_profile(profile_name)
        first_root = root / "first"
        first_root.mkdir()
        first = (first_fixture.EntryEvidenceTests().capture(first_root) if profile_name == "entry"
                 else nested_fixture.NestedTryEvidenceTests().capture(first_root))
        first_path = first_root / profile.first_capture
        first_path.write_text(json.dumps(first))
        result = {"schema": 1, "evidence": profile.evidence, "profile": profile_name, "passed": True,
                  "proof_sha256": runner.file_digest(runner.PROOF),
                  "receipt_sha256": runner.file_digest(runner.RECEIPT),
                  "first_capture_sha256": runner.file_digest(first_path),
                  "catch_search_runtime": first["catch_search_runtime"], "cases": []}
        for name in profile.cases:
            previous, parent = first_root / name, root / name
            parent.mkdir()
            shutil.copyfile(previous / "vcruntime140.dll", parent / "vcruntime140.dll")
            original = (previous / "product.exe").read_bytes()
            product = bytearray(original)
            product.extend(bytes(0xa00 - len(product)))
            struct.pack_into("<H", product, 0x86, 3)
            section = 0x98 + 224 + 80
            product[section:section + 8] = b".ndtext\0"
            struct.pack_into("<4I", product, section + 8, 0x200, 0x3000, 0x200, 0x800)
            struct.pack_into("<I", product, section + 36, 0x60000020)
            product[0x600] = 0xe9
            struct.pack_into("<i", product, 0x601, 0x3000 - 0x2005)
            receipt = json.loads((previous / "contract.json").read_text())
            receipt.update(source_frame="realigned", source_begin=0x2000, source_end=0x2080,
                           generated_begin=0x3000, generated_end=0x3080,
                           source_image_sha256=hashlib.sha256(original).hexdigest(),
                           image_sha256=hashlib.sha256(product).hexdigest())
            if profile_name == "catch-cleanup":
                receipt.update(entry_registers=0, entry_pop=0, incoming_reads=0, incoming_writes=0,
                               cleanup_actions=2 if "-o0-" in name else 1, cleanup_calls=2)
            (parent / "contract.json").write_text(json.dumps(receipt))
            (parent / "source.ll").write_text("second-generation IR")
            (parent / "rewrite.xml").write_text('<testsuites tests="1"/>')
            record = {"case": name, "images": [],
                      "contract_sha256": runner.file_digest(parent / "contract.json"),
                      "ir_sha256": runner.file_digest(parent / "source.ll")}
            for route in runner.ROUTES:
                image = runner.PE32(original if route == "original" else product)
                for suffix, base in (("", runner.BASES[0]), ("-rebased", runner.BASES[1])):
                    path = parent / (route + suffix + ".exe")
                    path.write_bytes(image.data if not suffix else image.rebase(base))
                    record["images"].append({"image": path.name, "base": base, "route": route,
                                             "expected_exit": int(name.endswith("-control")),
                                             "sha256": runner.file_digest(path)})
            result["cases"].append(record)
        return result

    def test_cleanup_profile_requires_its_nested_capture_and_ordered_calls(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root, "catch-cleanup")
            self.assertEqual(len(replay.validate_capture(root, capture)), 32)
            for wrong in ("entry", "unknown"):
                with self.subTest(profile=wrong), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture | {"profile": wrong})
            profile = runner.get_profile("catch-cleanup")
            for case in capture["cases"]:
                name, parent = case["case"], root / case["case"]
                receipt = json.loads((parent / "contract.json").read_text())
                first = json.loads((root / "first" / name / "contract.json").read_text())
                original = runner.PE32((parent / "original.exe").read_bytes())
                product = runner.PE32((parent / "product.exe").read_bytes())
                for key, value in (("cleanup_actions", 3), ("cleanup_calls", 1),
                                   ("entry_registers", 1), ("incoming_writes", 1)):
                    with self.subTest(case=name, key=key), self.assertRaises(ValueError):
                        profile.validate_installation(original, product, receipt | {key: value}, first, name)
            case = capture["cases"][0]
            name = case["case"]
            parent = root / name
            receipt = json.loads((parent / "contract.json").read_text())
            values = [17, 28, 39, 7, 18, 39, 1, 12] + [runner.BASES[0] + 0x3004] * 3
            output = "MULTICATCH " + " ".join(f"{v:08X}" for v in values) + "\nCLEANUP 00000000 00000035 00000000\n"
            for changed in (output, output.replace("00403004", "00402004"),
                             output.replace("00000035", "00000053")):
                result = {"exit_code": 0, "stdout": changed}
                with patch.object(catches, "run_image", return_value=result):
                    args = (parent / "product.exe", name, "product", receipt, [], {}, 1)
                    if changed == output:
                        profile.observe(*args)
                    else:
                        with self.assertRaises(ValueError):
                            profile.observe(*args)

    def test_requires_both_generation_identities_and_the_complete_matrix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 128)
            for mutation in range(13):
                changed = copy.deepcopy(capture)
                case = changed["cases"][0]
                if mutation == 0:
                    changed["passed"] = False
                elif mutation < 4:
                    changed[("proof_sha256", "receipt_sha256", "first_capture_sha256")[mutation - 1]] = "stale"
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
                    case["ir_sha256"] = "stale"
                elif mutation == 11:
                    case["contract_sha256"] = "stale"
                else:
                    changed["catch_search_runtime"]["sha256"] = "a" * 64
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)

    def test_first_generation_must_be_valid_even_after_rehashing_its_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            path = root / "first/entry-rewrite.json"
            first = json.loads(path.read_text())
            first["passed"] = False
            path.write_text(json.dumps(first))
            capture["first_capture_sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_frame_abi_and_both_trampolines_cannot_be_substituted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = capture["cases"][0]
            parent = root / case["case"]
            first = json.loads((root / "first" / case["case"] / "contract.json").read_text())
            original = runner.PE32((parent / "original.exe").read_bytes())
            data = (parent / "product.exe").read_bytes()
            receipt = json.loads((parent / "contract.json").read_text())
            for key, value in (("source_frame", "fixed-displaced"), ("entry_pop", 4),
                               ("entry_registers", 1), ("incoming_reads", 0),
                               ("source_begin", 0x2001), ("source_end", 0x207f)):
                with self.subTest(key=key), self.assertRaises(ValueError):
                    runner.validate_installation(original, runner.PE32(data),
                                                 receipt | {key: value}, first, case["case"])
            for offset in (0x200, 0x201, 0x600, 0x601):
                changed = bytearray(data)
                changed[offset] ^= 1
                rehashed = receipt | {"image_sha256": hashlib.sha256(changed).hexdigest()}
                with self.subTest(offset=offset), self.assertRaises(ValueError):
                    runner.validate_installation(original, runner.PE32(changed), rehashed,
                                                 first, case["case"])

    def test_rebased_route_must_equal_the_second_transaction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            case = capture["cases"][0]
            record = next(r for r in case["images"] if r["image"] == "cli-inplace-rebased.exe")
            path = root / case["case"] / record["image"]
            data = bytearray(path.read_bytes())
            data[0x880] ^= 1
            path.write_bytes(data)
            record["sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_observed_callers_must_move_to_the_second_generated_owner(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            name = capture["cases"][0]["case"]
            parent = root / name
            receipt = json.loads((parent / "contract.json").read_text())
            for route in ("original", "product"):
                expected = 0x2004 if route == "original" else 0x3004
                for caller in (0x1004, 0x2004, 0x3004):
                    values = list(entry.EXPECTED) + [runner.BASES[0] + caller] * 3
                    result = {"exit_code": 0, "stdout": "ENTRY " + " ".join(f"{v:08X}" for v in values) + "\n"}
                    with self.subTest(route=route, caller=caller), patch.object(entry, "run_image", return_value=result):
                        args = (parent / (route + ".exe"), name, route, receipt, [], {}, 1)
                        if caller == expected:
                            runner.observe(*args, source_section=".ndtext")
                        else:
                            with self.assertRaises(ValueError):
                                runner.observe(*args, source_section=".ndtext")


if __name__ == "__main__":
    unittest.main()
