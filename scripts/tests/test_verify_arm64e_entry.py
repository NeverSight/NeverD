"""Reject partial, skipped or mismatched reconstructed-entry qualification."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify_arm64e_entry as entry

CASES = json.loads(Path(__file__).with_suffix(".def").read_text())


class EntryQualificationTests(unittest.TestCase):
    def test_export_requires_exact_executed_case(self):
        entry.check_export(CASES["export_xml"])
        for failure in entry.native.POLICY["failure_tags"]:
            xml = ET.fromstring(CASES["export_xml"])
            ET.SubElement(next(xml.iter("testcase")), failure)
            with self.subTest(failure=failure), self.assertRaises(ValueError):
                entry.check_export(ET.tostring(xml))
        for field in ("name", "classname", "status"):
            xml = ET.fromstring(CASES["export_xml"])
            next(xml.iter("testcase")).attrib.pop(field)
            with self.subTest(field=field), self.assertRaises(ValueError):
                entry.check_export(ET.tostring(xml))

    def test_probe_requires_same_source_system_pac_and_unwind(self):
        entry.check_probe(CASES["probe"], CASES["provenance"])
        for change in CASES["invalid_probe_overrides"]:
            with self.subTest(change=change), self.assertRaises(ValueError):
                entry.check_probe({**CASES["probe"], **change}, CASES["provenance"])

    def test_both_frames_and_authenticated_call_must_exist(self):
        entry.check_disassembly(CASES["assembly"], 0)
        entry.check_disassembly(CASES["assembly"], 1)
        for token in CASES["missing_frame_tokens"]:
            with self.subTest(token=token), self.assertRaises(ValueError):
                entry.check_disassembly(CASES["assembly"].replace(token, ""), 0)
        with self.assertRaises(ValueError):
            entry.check_disassembly(CASES["assembly"].replace("blrab", "blr"), 1)

    def test_complete_matrix_cleanup_and_no_codegen_or_cpta_claim(self):
        contract = entry.native.fixture_contract()
        for compile_only, fail in ((False, False), (True, False), (False, True)):
            with self.subTest(compile_only=compile_only, fail=fail), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                output = root / CASES["output_name"]
                output.mkdir()
                binary, compiler = root / CASES["fake_binary"], root / CASES["fake_compiler"]
                for path in (binary, compiler, root / CASES["source_name"]):
                    path.write_text(CASES["binary_bytes"])
                probe = root / CASES["probe_name"]
                probe.write_text(json.dumps(CASES["probe"]))
                executed = []

                def command(products, directory, label, argv, timeout, env=None):
                    stdout, code = CASES["compiler_version"], 0
                    if argv[0] == str(binary):
                        (output / entry.POLICY["export_xml"]).write_text(CASES["export_xml"])
                        for name in entry.POLICY["export_sources"]:
                            (output / name).write_text(CASES["binary_bytes"])
                    elif entry.POLICY["output_option"] in argv:
                        Path(argv[-1]).write_text(CASES["binary_bytes"])
                    elif label.endswith(entry.native.POLICY["disassembly_suffix"]):
                        stdout = CASES["assembly"]
                    elif label.endswith(entry.POLICY["sections_suffix"]):
                        stdout = CASES["sections_dwarf"] if CASES["dwarf_name_fragment"] in label else CASES["sections_compact"]
                    elif argv[0] == str(output / entry.POLICY["loader_binary"]):
                        executed.append(label)
                        if fail:
                            code = CASES["failure_code"]
                    return subprocess.CompletedProcess(argv, code, stdout, "")

                policy = {**entry.POLICY, "source_files": [CASES["source_name"]]}
                with mock.patch.object(entry, "POLICY", policy), \
                        mock.patch.object(entry.native, "ROOT", root), \
                        mock.patch.object(entry.native, "fixture_contract", return_value=contract), \
                        mock.patch.object(entry.native, "provenance", return_value=CASES["provenance"]), \
                        mock.patch.object(entry.native, "check_binary", side_effect=lambda p: {"sha256": entry.native.digest(p), "cpu_subtype": entry.POLICY["classic_subtype"]}), \
                        mock.patch.object(entry.native, "command", side_effect=command):
                    if fail:
                        with self.assertRaises(subprocess.CalledProcessError):
                            entry.run(output, binary, compiler, probe)
                    else:
                        entry.run(output, binary, compiler, None if compile_only else probe,
                                  compile_only=compile_only)
                report = json.loads((output / entry.POLICY["report_name"]).read_text())
                self.assertEqual(report["native_reconstructed_entry_qualified"], not compile_only and not fail)
                self.assertFalse(report["neverd_codegen_qualified"])
                self.assertFalse(report["native_apple_cpta_qualified"])
                self.assertFalse(report["whole_image_publication_qualified"])
                self.assertEqual(len(executed), 0 if compile_only else 1 if fail else CASES["case_count"])
                for path in output.iterdir():
                    self.assertNotIn(path.suffix, (".out", ".o", ".dylib"))

    def test_requested_unwind_mode_matches_image_sections(self):
        entry.check_sections(CASES["sections_dwarf"], True)
        entry.check_sections(CASES["sections_compact"], False)
        for text, dwarf in ((CASES["sections_dwarf"], False),
                            (CASES["sections_compact"], True), ("", True)):
            with self.subTest(dwarf=dwarf), self.assertRaises(ValueError):
                entry.check_sections(text, dwarf)

    def test_abi_subtype_is_exact_without_rewriting_the_object(self):
        for subtype in (entry.native.POLICY["cpu_subtype"], entry.POLICY["classic_subtype"]):
            identity = {"cpu_subtype": subtype}
            with mock.patch.object(entry.native, "check_binary", return_value=identity):
                if subtype == entry.POLICY["classic_subtype"]:
                    self.assertEqual(entry.check_classic(None), identity)
                else:
                    with self.assertRaises(ValueError):
                        entry.check_classic(None)


if __name__ == "__main__":
    unittest.main()
