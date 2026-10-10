import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_windows_registration_realigned_rewrite as runner
from scripts import replay_windows_registration_realigned_rewrite as replay
from scripts import windows_registration_libraries as libraries


class RealignedRewriteEvidenceTests(unittest.TestCase):
    def capture(self, root):
        data = bytearray(0x800)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3c, 0x80)
        data[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<HH", data, 0x84, 0x14c, 2)
        struct.pack_into("<H", data, 0x94, 224)
        optional = 0x98
        struct.pack_into("<H", data, optional, 0x10b)
        struct.pack_into("<I", data, optional + 28, runner.BASES[0])
        struct.pack_into("<II", data, optional + 96, 0x1100, 0x80)
        struct.pack_into("<II", data, optional + 96 + 5 * 8, 0x1300, 12)
        section = optional + 224
        data[section:section + 8] = b".text\0\0\0"
        struct.pack_into("<4I", data, section + 8, 0x400, 0x1000, 0x400, 0x200)
        struct.pack_into("<I", data, section + 36, 0x60000020)
        section += 40
        data[section:section + 8] = b".ndtext\0"
        struct.pack_into("<4I", data, section + 8, 0x200, 0x2000, 0x200, 0x600)
        struct.pack_into("<I", data, section + 36, 0x60000020)
        struct.pack_into("<5I", data, 0x300 + 20, 1, 1, 0x1140, 0x1144, 0x1148)
        struct.pack_into("<IIH", data, 0x340, 0x1000, 0x1150, 0)
        data[0x350:0x360] = b"callback_parent\0"
        struct.pack_into("<I", data, 0x250, 0x401000)
        struct.pack_into("<IIHH", data, 0x500, 0x1000, 12, 0x3050, 0)
        capture = {"schema": 1, "evidence": "realigned-source-reconstruction", "passed": True,
                   "source_sha256": runner.file_digest(runner.SOURCE),
                   "emitter_sha256": runner.file_digest(runner.EMITTER),
                   "direct_emitter_sha256": runner.file_digest(runner.DIRECT_EMITTER), "objects": {},
                   "runtime_libraries": {"schema": 1, "architecture": "x86", "toolset": "14.51",
                                         "files": [{"name": n, "sha256": "a" * 64}
                                                   for n in libraries.LIBRARIES]}, "cases": []}
        (root / "catch-projection.xml").write_text('<testsuites tests="1"/>')
        for kind in runner.FORMS:
            (root / (kind + ".obj")).write_bytes(kind.encode())
            (root / (kind + "-emit.xml")).write_text('<testsuites tests="1"/>')
            capture["objects"][kind] = runner.file_digest(root / (kind + ".obj"))
        for name in runner.CASES:
            case = root / name
            case.mkdir()
            original = bytearray(data)
            original[0x280] = len(name)
            product = bytearray(original)
            product[0x200] = 0xe9
            struct.pack_into("<i", product, 0x201, 0x2000 - 0x1005)
            receipt = {"schema": 1, "evidence": "checked-realigned-source-reconstruction",
                       "source_frame": "direct" if runner.proof_count(name) == 1 else "realigned",
                       "base": runner.BASES[0], "source_begin": 0x1000, "source_end": 0x1040,
                       "generated_begin": 0x2000, "generated_end": 0x2080,
                       "source_image_sha256": hashlib.sha256(original).hexdigest(),
                       "image_sha256": hashlib.sha256(product).hexdigest()}
            (case / "contract.json").write_text(json.dumps(receipt))
            (case / "source.ll").write_text("test IR")
            (case / "rewrite.xml").write_text(f'<testsuites tests="{runner.proof_count(name)}"/>')
            record = {"case": name, "images": [],
                      "contract_sha256": runner.file_digest(case / "contract.json"),
                      "ir_sha256": runner.file_digest(case / "source.ll")}
            for route in runner.ROUTES:
                source = runner.PE32(original if route == "original" else product)
                for suffix, base in (("", runner.BASES[0]), ("-rebased", runner.BASES[1])):
                    path = case / (route + suffix + ".exe")
                    path.write_bytes(source.data if not suffix else source.rebase(base))
                    record["images"].append({"image": path.name, "base": base, "route": route,
                                             "expected_exit": int(name.endswith("-control")),
                                             "sha256": runner.file_digest(path)})
            capture["cases"].append(record)
        return capture

    def test_requires_exact_sources_routes_controls_and_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            self.assertEqual(len(replay.validate_capture(root, capture)), 160)
            for mutation in range(12):
                changed = copy.deepcopy(capture)
                case = changed["cases"][0]
                if mutation == 0:
                    changed["passed"] = False
                elif mutation == 1:
                    changed["cases"].pop()
                elif mutation == 2:
                    changed["cases"][-1] = changed["cases"][0]
                elif mutation == 3:
                    case["images"].pop()
                elif mutation == 4:
                    case["images"][-1] = case["images"][0]
                elif mutation == 5:
                    case["images"][0]["image"] = "../original.exe"
                elif mutation == 6:
                    case["images"][0]["expected_exit"] = 1
                elif mutation == 7:
                    changed["emitter_sha256"] = "stale"
                elif mutation == 8:
                    changed["objects"]["value"] = "stale"
                elif mutation == 9:
                    case["contract_sha256"] = "stale"
                elif mutation == 10:
                    case["ir_sha256"] = "stale"
                elif mutation == 11:
                    changed["direct_emitter_sha256"] = "stale"
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            path = root / "value/cli-section-rebased.exe"
            data = bytearray(path.read_bytes())
            data[0x680] ^= 1
            path.write_bytes(data)
            record = next(r for r in capture["cases"][0]["images"] if r["image"] == path.name)
            record["sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_requires_executed_reconstruction(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            for xml in ('<testsuites tests="0"/>', '<testsuites tests="1"/>',
                        '<testsuites tests="2"/>', '<testsuites tests="4"/>',
                        '<testsuites tests="3" failures="1"/>',
                        '<testsuites tests="3"><testcase><skipped/></testcase></testsuites>'):
                (root / "value/rewrite.xml").write_text(xml)
                with self.subTest(xml=xml), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture)

    def test_requires_fixed_frame_and_shared_proof_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            for path in (root / "catch-projection.xml", root / "catch-all-fixed/rewrite.xml"):
                for xml in ('<testsuites tests="0"/>', '<testsuites tests="2"/>',
                            '<testsuites tests="1" failures="1"/>',
                            '<testsuites tests="1"><testcase><skipped/></testcase></testsuites>'):
                    path.write_text(xml)
                    with self.subTest(path=path, xml=xml), self.assertRaises(ValueError):
                        replay.validate_capture(root, capture)
                path.write_text('<testsuites tests="1"/>')

    def test_rejects_changed_source_frame_even_with_updated_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            record = next(c for c in capture["cases"] if c["case"] == "catch-all-fixed")
            path = root / record["case"] / "contract.json"
            contract = json.loads(path.read_text())
            contract["source_frame"] = "realigned"
            path.write_text(json.dumps(contract))
            record["contract_sha256"] = runner.file_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_runtime_requires_value_chain_iterations_and_exact_caller(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            for name, path, route, receipt in replay.validate_capture(root, capture):
                base = runner.PE32(path.read_bytes()).base
                owner = "source" if route == "original" else "generated"
                values = [7, 18 if name.startswith("reference") else 7, 1, 4,
                          base + receipt[owner + "_begin"] + 8]
                expected_exit = int(name.endswith("-control"))

                def observation(fields, exit_code=expected_exit):
                    return {"exit_code": exit_code,
                            "stdout": "CALLBACK " + " ".join(f"{v:08X}" for v in fields) + "\n"}

                with patch.object(runner, "run_image", return_value=observation(values)):
                    result = runner.observe(path, name, route, receipt, [], {}, 1)
                    self.assertEqual(result["caller_rva"], receipt[owner + "_begin"] + 8)
                rejected = [observation(values, 1 - expected_exit),
                            {"exit_code": expected_exit, "stdout": ""},
                            {"exit_code": expected_exit, "stdout": "prefix " + observation(values)["stdout"]}]
                for index in range(4):
                    changed = values.copy()
                    changed[index] += 1
                    rejected.append(observation(changed))
                for caller in (base + receipt[owner + "_begin"] - 1,
                               base + receipt[owner + "_end"], values[4] - base):
                    rejected.append(observation(values[:4] + [caller]))
                for result in rejected:
                    with self.subTest(case=name, image=path.name, runtime=result), \
                            patch.object(runner, "run_image", return_value=result), \
                            self.assertRaises(ValueError):
                        runner.observe(path, name, route, receipt, [], {}, 1)


if __name__ == "__main__":
    unittest.main()
