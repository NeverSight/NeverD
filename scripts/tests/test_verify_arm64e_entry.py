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
    def test_cli_preserves_cxx_driver_symlink_name(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            target = root / CASES["compiler_target"]
            target.write_text(CASES["binary_bytes"])
            compiler = root / CASES["compiler_symlink"]
            compiler.symlink_to(target)
            output = root / CASES["output_name"]
            argv = [entry.__file__, entry.POLICY["output_argument"], str(output),
                    entry.POLICY["binary_argument"], str(target),
                    entry.POLICY["compiler_argument"], str(compiler),
                    entry.POLICY["offline_argument"]]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(entry, "run", return_value={"status": entry.native.POLICY["passed"]}) as run, \
                    mock.patch("builtins.print"):
                self.assertEqual(entry.main(), 0)
            self.assertEqual(run.call_args.args[2], compiler)
            self.assertNotEqual(run.call_args.args[2], compiler.resolve())

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

    def test_complete_matrix_cleanup_and_qualified_producer_scope(self):
        contract = entry.native.fixture_contract()
        for compile_only, fail, backend, publish in (
                (False, False, False, False), (True, False, False, False),
                (False, True, False, False), (False, False, True, False),
                (False, False, True, True), (True, False, True, True),
                (False, True, True, True)):
            producer = entry.POLICY["backend_producer"] if backend else entry.POLICY["producers"][0]
            with self.subTest(compile_only=compile_only, fail=fail, backend=backend, publish=publish), tempfile.TemporaryDirectory() as temp:
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
                        if entry.POLICY["unreserved_filter"] in argv:
                            self.assertTrue(publish)
                            self.assertTrue(Path(env[entry.POLICY["publication_input_environment"]]).exists())
                            xml = CASES["export_xml"].replace(entry.POLICY["export_case"], entry.POLICY["unreserved_case"])
                            Path(argv[-1].removeprefix(entry.POLICY["export_xml_option"])).write_text(xml)
                        elif entry.POLICY["publication_filter"] in argv:
                            self.assertTrue(publish)
                            self.assertTrue(Path(env[entry.POLICY["publication_input_environment"]]).exists())
                            Path(env[entry.POLICY["publication_output_environment"]]).write_text(
                                CASES["binary_bytes"] * CASES["publication_image_repeats"])
                            Path(env[entry.POLICY["publication_receipt_environment"]]).write_text(json.dumps(CASES["receipt"]))
                            Path(argv[-1].removeprefix(entry.POLICY["export_xml_option"])).write_text(CASES["publication_xml"])
                        else:
                            (output / entry.POLICY["export_xml"]).write_text(CASES["export_xml"])
                            for name in entry.POLICY["export_sources"]:
                                (output / name).write_text(CASES["binary_bytes"])
                            self.assertEqual(entry.POLICY["backend_environment"] in env, backend)
                            if backend:
                                for key in ("backend_object", "backend_dwarf_object", "original_ir", "original_dwarf_ir"):
                                    (output / entry.POLICY[key]).write_text(CASES["binary_bytes"])
                    elif entry.POLICY["output_option"] in argv:
                        self.assertIn(entry.POLICY["sysroot_option"], argv)
                        self.assertEqual(argv[argv.index(entry.POLICY["sysroot_option"]) + 1], CASES["sdk_path"])
                        Path(argv[-1]).write_text(CASES["binary_bytes"])
                    elif label == entry.POLICY["sdk_label"]:
                        stdout = CASES["sdk_path"]
                    elif label.endswith(entry.native.POLICY["disassembly_suffix"]):
                        stdout = (CASES["inspection_assembly"] if entry.POLICY["inspection_suffix"] in label else CASES["assembly"])
                    elif label.endswith(entry.POLICY["sections_suffix"]):
                        stdout = CASES["sections_dwarf"] if CASES["dwarf_name_fragment"] in label else CASES["sections_compact"]
                    elif label.endswith(entry.POLICY["unwind_suffix"]):
                        stdout = CASES["unwind_dwarf"] if CASES["dwarf_name_fragment"] in label else CASES["unwind_compact"]
                    elif argv[0] == str(output / entry.POLICY["loader_binary"]):
                        if label.endswith(entry.POLICY["baseline_suffix"]):
                            code = entry.POLICY["baseline_returncodes"][0]
                            stdout = entry.POLICY["caller_entry_marker"]
                        else:
                            stdout = entry.POLICY["caller_entry_marker"] + entry.POLICY["caller_pass_marker"]
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
                            entry.run(output, binary, compiler, probe, producer=producer, publish=publish)
                    else:
                        entry.run(output, binary, compiler, None if compile_only else probe,
                                  compile_only=compile_only, producer=producer, publish=publish)
                report = json.loads((output / entry.POLICY["report_name"]).read_text())
                self.assertEqual(report["native_reconstructed_entry_qualified"], not compile_only and not fail)
                self.assertEqual(report["neverd_codegen_qualified"], backend and not compile_only and not fail)
                self.assertEqual(report["reconstructed_entry_publication_qualified"], publish and not compile_only and not fail)
                self.assertFalse(report["native_apple_cpta_qualified"])
                self.assertFalse(report["whole_image_publication_qualified"])
                self.assertEqual(len(executed), 0 if compile_only else 1 if fail else CASES["case_count"])
                for path in output.iterdir():
                    self.assertNotIn(path.suffix, (".out", ".o", ".dylib"))

    def test_receipt_cannot_omit_duplicate_or_alias_entries(self):
        entry.check_receipt(CASES["receipt"])
        variants = []
        missing = json.loads(json.dumps(CASES["receipt"]))
        missing["entries"].pop()
        variants.append(missing)
        duplicate = json.loads(json.dumps(CASES["receipt"]))
        duplicate["entries"][-1] = duplicate["entries"][0]
        variants.append(duplicate)
        alias = json.loads(json.dumps(CASES["receipt"]))
        alias["entries"][0]["original"] = alias["entries"][0]["installed"]
        variants.append(alias)
        for name in entry.exception_frames():
            missing_frame = json.loads(json.dumps(CASES["receipt"]))
            missing_frame["functions"].pop(name)
            variants.append(missing_frame)
        for receipt in variants:
            with self.subTest(receipt=receipt), self.assertRaises(ValueError):
                entry.check_receipt(receipt)

    def test_installed_frames_require_the_receipt_addresses(self):
        functions = entry.check_receipt(CASES["receipt"])
        entry.check_disassembly(CASES["installed_assembly"], 0, functions)
        entry.check_unwind(CASES["unwind_dwarf"], True, functions)
        moved = {name: address + 1 for name, address in functions.items()}
        with self.assertRaises(ValueError):
            entry.check_disassembly(CASES["installed_assembly"], 0, moved)
        with self.assertRaises(ValueError):
            entry.check_unwind(CASES["unwind_dwarf"], True, moved)

    def test_startup_traps_and_early_returns_do_not_qualify(self):
        started = entry.POLICY["caller_entry_marker"]
        passed = entry.POLICY["caller_pass_marker"]
        trap = entry.POLICY["baseline_returncodes"][0]
        entry.check_execution(subprocess.CompletedProcess([], trap, started), original=True)
        entry.check_execution(subprocess.CompletedProcess([], 0, started + passed))
        for code, output, original in ((trap, "", True), (trap, started + passed, True),
                                        (0, started, True), (0, "", False), (0, started, False)):
            with self.subTest(code=code, output=output, original=original), self.assertRaises(ValueError):
                entry.check_execution(subprocess.CompletedProcess([], code, output), original=original)

    def test_requested_unwind_mode_matches_image_sections(self):
        entry.check_sections(CASES["sections_dwarf"], True)
        entry.check_sections(CASES["sections_compact"], False)
        for text, dwarf in ((CASES["sections_no_index"], False),
                            (CASES["sections_no_index"], True),
                            (CASES["sections_compact"], True), ("", True)):
            with self.subTest(dwarf=dwarf), self.assertRaises(ValueError):
                entry.check_sections(text, dwarf)

    def test_unwind_encoding_covers_each_exception_frame(self):
        entry.check_unwind(CASES["unwind_dwarf"], True)
        entry.check_unwind(CASES["unwind_compact"], False)
        entry.check_unwind(CASES["unwind_regular_dwarf"], True)
        entry.check_unwind(CASES["unwind_regular_compact"], False)
        for text, dwarf in ((CASES["unwind_dwarf"], False),
                            (CASES["unwind_compact"], True), ("", True)):
            with self.subTest(dwarf=dwarf), self.assertRaises(ValueError):
                entry.check_unwind(text, dwarf)
        for name in entry.exception_frames() + entry.POLICY["native_exception_functions"]:
            for dwarf in (False, True):
                text = CASES["unwind_dwarf" if dwarf else "unwind_compact"]
                with self.subTest(name=name, dwarf=dwarf), self.assertRaises(ValueError):
                    entry.check_unwind(text.replace(name, ""), dwarf)
        for line in CASES["unwind_dwarf"].splitlines():
            if entry.re.search(entry.POLICY["unwind_pattern"], line):
                with self.subTest(line=line), self.assertRaises(ValueError):
                    entry.check_unwind(CASES["unwind_dwarf"].replace(line, ""), True)

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
