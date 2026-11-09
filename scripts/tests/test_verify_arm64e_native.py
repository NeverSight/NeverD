"""Reject false native qualification from skipped or mismatched evidence."""

import json
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify_arm64e_native as native

CASES = json.loads(Path(__file__).with_suffix(".def").read_text())


def runtime_xml():
    root = ET.Element(CASES["suite_tag"])
    for name in CASES["expected_tests"]:
        ET.SubElement(root, CASES["case_tag"], name=name,
                      status=CASES["run_status"])
    return root


class Arm64eNativeQualificationTests(unittest.TestCase):
    def test_failed_legacy_linker_lookup_stops_before_compiling(self):
        failure = subprocess.CompletedProcess([], CASES["legacy_lookup_failure"], "", "")
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(native, "command", return_value=failure) as command:
            with self.assertRaises(RuntimeError):
                native.build_legacy_fixtures(Path(temporary))
            command.assert_called_once()

    def test_legacy_bundle_requires_current_sources_and_exact_image_bytes(self):
        with tempfile.TemporaryDirectory() as temporary, \
                mock.patch.object(native, "provenance", return_value=CASES["provenance"]):
            root = Path(temporary)
            report = {**CASES["provenance"], "status": native.POLICY["passed"], "images": []}
            for case in native.legacy_cases(native.fixture_contract()):
                path = root / case["name"]
                path.write_bytes(struct.pack(CASES["legacy_header_format"], *CASES["legacy_header"]))
                report["images"].append({**case, **native.check_binary(path)})
            manifest = root / native.POLICY["report_name"]
            native.save(manifest, report)
            self.assertEqual(native.verify_legacy_fixtures(root), report)
            for mutation in CASES["probe_mutations"]:
                if not any(key in mutation for key in native.POLICY["provenance_keys"]):
                    continue
                native.save(manifest, dict(report, **mutation))
                with self.assertRaises(ValueError):
                    native.verify_legacy_fixtures(root)
            native.save(manifest, report)
            path.write_bytes(path.read_bytes() + CASES["legacy_trailing_bytes"].encode())
            with self.assertRaises(ValueError):
                native.verify_legacy_fixtures(root)
            path.unlink()
            with self.assertRaises(OSError):
                native.verify_legacy_fixtures(root)

    def test_pac_requires_entry_completion_and_successful_exit(self):
        contract = native.fixture_contract()
        for case in CASES["capability_cases"]:
            with self.subTest(case=case["name"]):
                lines = [contract[name] for name in case["markers"]]
                result = subprocess.CompletedProcess([], case["returncode"],
                                                     "\n".join(lines) + "\n", "")
                self.assertEqual(native.capability_status(result, contract), case["status"])

    def test_complete_runtime_matrix_passes(self):
        root = runtime_xml()
        self.assertEqual(set(native.check_runtime_report(ET.tostring(root))),
                         set(CASES["expected_tests"]))

    def test_failed_skipped_and_unrun_cases_do_not_qualify(self):
        for tag in CASES["failure_tags"]:
            with self.subTest(tag=tag):
                root = runtime_xml()
                ET.SubElement(root[0], tag)
                with self.assertRaises(ValueError):
                    native.check_runtime_report(ET.tostring(root))
        for status in CASES["unrun_statuses"]:
            with self.subTest(status=status):
                root = runtime_xml()
                root[0].set(CASES["status_key"], status)
                with self.assertRaises(ValueError):
                    native.check_runtime_report(ET.tostring(root))

    def test_missing_duplicate_and_foreign_cases_do_not_qualify(self):
        root = runtime_xml()
        root.remove(root[-1])
        with self.assertRaises(ValueError):
            native.check_runtime_report(ET.tostring(root))
        for replacement in [CASES["expected_tests"][1], CASES["foreign_test"]]:
            root = runtime_xml()
            root[0].set(CASES["name_key"], replacement)
            with self.assertRaises(ValueError):
                native.check_runtime_report(ET.tostring(root))

    def test_ctest_parameter_annotations_preserve_exact_case_identity(self):
        root = runtime_xml()
        for case in root:
            case.set(CASES["name_key"], case.attrib[CASES["name_key"]] + CASES["annotation"])
        self.assertEqual(set(native.check_runtime_report(ET.tostring(root))),
                         set(CASES["expected_tests"]))

    def test_unqualified_or_stale_probe_stops_before_test_execution(self):
        for mutation in CASES["probe_mutations"]:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                report = dict(CASES["passed_probe"], **mutation)
                probe = root / CASES["probe_file"]
                probe.write_text(json.dumps(report))
                with mock.patch.object(native, "provenance", return_value=CASES["provenance"]), \
                        mock.patch.object(native, "command") as command:
                    with self.assertRaises(ValueError):
                        native.run_installation(root, root, probe)
                    command.assert_not_called()
                actual = json.loads((root / native.POLICY["report_name"]).read_text())
                self.assertFalse(actual[CASES["qualification_key"]])


if __name__ == "__main__":
    unittest.main()
