from pathlib import Path
import copy
import hashlib
import struct
import tempfile
import unittest

from scripts import check_windows_registration_realigned as runner
from scripts import replay_windows_registration_realigned as replay
from scripts import windows_registration_libraries as libraries



class RealignedCallbackEvidenceTests(unittest.TestCase):
    def capture(self, root):
        data = bytearray(0x600)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3c, 0x80)
        data[0x80:0x84] = b"PE\0\0"
        struct.pack_into("<HH", data, 0x84, 0x14c, 1)
        struct.pack_into("<H", data, 0x94, 224)
        optional = 0x98
        struct.pack_into("<H", data, optional, 0x10b)
        struct.pack_into("<I", data, optional + 28, 0x400000)
        struct.pack_into("<II", data, optional + 96 + 5 * 8, 0x1200, 12)
        section = optional + 224
        data[section:section + 8] = b".text\0\0\0"
        struct.pack_into("<4I", data, section + 8, 0x400, 0x1000, 0x400, 0x200)
        struct.pack_into("<I", data, 0x210, 0x401000)
        struct.pack_into("<IIHH", data, 0x400, 0x1000, 12, 0x3010, 0)
        (root / "frame.obj").write_bytes(b"test object")
        (root / "emit.xml").write_text('<testsuites tests="3"/>')
        capture = {"schema": 1, "evidence": "generated-realigned-callback-analysis",
                   "source_sha256": hashlib.sha256(runner.SOURCE.read_bytes()).hexdigest(),
                   "object_sha256": hashlib.sha256(b"test object").hexdigest(),
                   "runtime_libraries": {"schema": 1, "architecture": "x86",
                                         "toolset": "14.44", "files": [
                                             {"name": name, "sha256": "a" * 64}
                                             for name in libraries.LIBRARIES]},
                   "images": []}
        for name, expected in replay.IMAGES.items():
            image = bytearray(data)
            image[0x218] = expected
            if "rebased" in name:
                image = runner.PE32(image).rebase(0x18000000)
            path = root / name
            path.write_bytes(image)
            record = {"image": name, "expected_exit": expected,
                      "sha256": runner.image_digest(path)}
            if not expected:
                record["analysis_tests"] = 3
                (root / (Path(name).stem + ".xml")).write_text('<testsuites tests="3"/>')
                record["decompilation"] = {}
                for language in ("c", "cpp"):
                    source = root / (Path(name).stem + ".decompiled." + language)
                    source.write_text(self.output(language))
                    record["decompilation"][language] = {
                        "source": source.name,
                        "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "syntax_checked": language == "c"}
            capture["images"].append(record)
        return capture

    @staticmethod
    def output(language):
        return ("/* highir.structured_regions=1, fallback_regions=0 */\n"
                "v1 = __neverd_x86_callback_esp(0x401000u, 0x401100u);\n"
                "callback_increment(((frame_base - 16) & mask) - 576);\n" +
                ("/* Native x86 callback @ 0x401100 */\ngoto L_x86_eh_after_0;\n"
                 if language == "c" else "catch (...) {}\n"))

    def test_public_output_preserves_the_callback_and_exact_call_arity(self):
        for language in ("c", "cpp"):
            text = self.output(language)
            runner.validate_decompilation(text, language)
            for before, after in (("regions=1", "regions=0"),
                                  ("__neverd_x86_callback_esp", "unknown"),
                                  (" - 576);", " - 576, saved_ebp);"),
                                  ("Native x86 callback @", "unknown"),
                                  ("catch (...)", "unknown")):
                if before not in text:
                    continue
                with self.subTest(language=language, before=before), self.assertRaises(ValueError):
                    runner.validate_decompilation(text.replace(before, after), language)

    def test_replay_requires_unchanged_public_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            for mutation in range(5):
                changed = copy.deepcopy(capture)
                outputs = changed["images"][0]["decompilation"]
                if mutation == 0:
                    outputs.pop("cpp")
                if mutation == 1:
                    outputs["c"]["syntax_checked"] = False
                if mutation == 2:
                    outputs["c"]["sha256"] = "stale"
                if mutation == 3:
                    outputs["c"]["source"] = "../probe.decompiled.c"
                if mutation == 4:
                    (root / outputs["cpp"]["source"]).write_text("changed")
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)

    def test_replay_authenticates_bases_files_and_the_control_matrix(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            replay.validate_capture(root, capture)
            for mutation in range(8):
                changed = copy.deepcopy(capture)
                if mutation == 0:
                    changed["images"].pop()
                if mutation == 1:
                    changed["images"][-1] = changed["images"][0]
                if mutation == 2:
                    changed["images"][0]["expected_exit"] = 1
                if mutation == 3:
                    changed["images"][0]["sha256"] = "stale"
                if mutation == 4:
                    changed["object_sha256"] = "stale"
                if mutation == 5:
                    changed["source_sha256"] = "stale"
                if mutation == 6:
                    changed["images"][0]["image"] = "../probe.exe"
                if mutation == 7:
                    changed.pop("runtime_libraries")
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    replay.validate_capture(root, changed)
            path = root / "probe-rebased.exe"
            data = bytearray(path.read_bytes())
            data[0x220] ^= 1
            path.write_bytes(data)
            capture["images"][1]["sha256"] = runner.image_digest(path)
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)

    def test_replay_cannot_admit_skipped_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            capture = self.capture(root)
            (root / "probe.xml").write_text(
                '<testsuites tests="3"><testcase><skipped/></testcase></testsuites>')
            with self.assertRaises(ValueError):
                replay.validate_capture(root, capture)
            (root / "probe.xml").write_text('<testsuites tests="3"/>')
            for count in (0, 1, 2, 4):
                (root / "emit.xml").write_text(f'<testsuites tests="{count}"/>')
                with self.subTest(count=count), self.assertRaises(ValueError):
                    replay.validate_capture(root, capture)

    def test_analysis_requires_executed_passing_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.xml"
            path.write_text('<testsuites tests="1" failures="0"><testcase/></testsuites>')
            self.assertEqual(runner.require_test_result(path), 1)
            for xml in ('<testsuites tests="0"/>', '<testsuites tests="-1"/>',
                        '<testsuites tests="1" failures="1"/>',
                        '<testsuites tests="1" disabled="1"/>',
                        '<testsuites tests="1"><testcase><skipped/></testcase></testsuites>'):
                path.write_text(xml)
                with self.assertRaises(ValueError):
                    runner.require_test_result(path)


if __name__ == "__main__":
    unittest.main()
