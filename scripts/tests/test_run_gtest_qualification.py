import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch

from scripts import run_gtest_qualification as qualification
from scripts import verification_report


LISTING = """Running main() from gtest_main.cc
Note: Google Test filter = Example.*
Example.
  First
  Second
"""
XML = """<testsuites tests="2" failures="0" disabled="0" errors="0">
  <testsuite tests="2" failures="0" disabled="0" errors="0">
    <testcase classname="Example" name="First" status="run" result="completed"/>
    <testcase classname="Example" name="Second" status="run" result="completed"/>
  </testsuite></testsuites>"""


class GTestQualificationTests(unittest.TestCase):
    def test_discovery_retains_parameterized_identities(self):
        names = qualification.identities(LISTING + """Typed/0.  # TypeParam = int
  Works
Targets/Matrix.
  Emits/X64  # GetParam() = 0
  Emits/AArch64  # GetParam() = 1
""", 5)
        self.assertEqual(names, ("Example.First", "Example.Second", "Typed/0.Works",
                                 "Targets/Matrix.Emits/X64",
                                 "Targets/Matrix.Emits/AArch64"))

    def test_added_tests_are_required_without_changing_the_floor(self):
        names = qualification.identities(LISTING + "  Third\n", 2)
        self.assertEqual(names[-1], "Example.Third")
        self.assertEqual(len(names), 3)

    def test_missing_duplicate_disabled_and_malformed_discoveries_fail(self):
        for listing in ("", "Example.\n  First\n", LISTING + "  First\n",
                        LISTING.replace("First", "DISABLED_First"),
                        LISTING.replace("Example.\n", "DISABLED_Example.\n"),
                        LISTING + "invalid output\n", "  Orphan\n", "Example.*\n"):
            with self.subTest(listing=listing), self.assertRaises(ValueError):
                qualification.identities(listing, 2)
        with self.assertRaises(ValueError):
            qualification.identities(LISTING, 0)

    def invoke(self, xml=XML, exit_code=0, discovery=LISTING, error=None,
               replace_binary=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, output = root / "tests.exe", root / "result.xml"
            binary.write_bytes(b"the qualified binary")
            receipt = output.with_suffix(".qualification.json")
            verification = output.with_suffix(".verification.json")
            verification.write_text('{"caller_loop":"completed"}')
            output.write_text(XML)
            receipt.write_text('{"old_success": true}')

            def execute(args, **kwargs):
                if "--gtest_list_tests" in args:
                    self.assertFalse(output.exists())
                    self.assertFalse(receipt.exists())
                    self.assertFalse(verification.exists())
                    self.assertEqual(args[1], "--gtest_filter=Example.*")
                    if error:
                        raise error
                    return subprocess.CompletedProcess(args, 0, stdout=discovery)
                self.assertEqual(args[1], "--gtest_filter=Example.*")
                if replace_binary:
                    binary.write_bytes(b"a different test binary")
                if xml is not None:
                    output.write_text(xml)
                return subprocess.CompletedProcess(args, exit_code)

            with patch.object(qualification.subprocess, "run", side_effect=execute):
                try:
                    report = qualification.run(binary, output, "Example.*", 2, 30)
                except Exception:
                    self.assertFalse(receipt.exists())
                    raise
                finally:
                    self.verification = json.loads(verification.read_text())
            self.assertEqual(json.loads(receipt.read_text()), report)
            self.assertEqual(report["outcome"]["passed"], 2)

    def test_complete_run_publishes_audited_identities(self):
        self.invoke()
        self.assertIsNone(self.verification["first_blocker"])
        self.assertIsNone(self.verification["record_comparison"])
        self.assertEqual(self.verification["caller_loop"], "unknown")

    def test_run_level_observations_and_specific_skip_survive_a_failed_audit(self):
        doc = ET.fromstring(XML)
        observations = {"record_comparison": {"total": 3, "compared": 3, "matches": 0},
                        "caller_loop": "not_run"}
        ET.SubElement(ET.SubElement(doc, "properties"), "property",
                      name="neverd_execution_evidence", value=json.dumps(observations))
        case = doc.find(".//testcase")
        ET.SubElement(case, "skipped", message="the reference was not captured")
        ET.SubElement(ET.SubElement(case, "properties"), "property",
                      name="neverd_missing_evidence", value="reference output for this input")
        with self.assertRaisesRegex(ValueError, "the reference was not captured"):
            self.invoke(xml=ET.tostring(doc, encoding="unicode"))
        blocker = self.verification["first_blocker"]
        self.assertEqual(blocker, {"path": ["case audit", "Example.First"], "state": "blocked",
                                   "diagnostic": "the reference was not captured",
                                   "missing_evidence": "reference output for this input"})
        for key, value in observations.items():
            self.assertEqual(self.verification[key], value)
        stages = {row["path"][0]: row["state"] for row in self.verification["stages"]}
        self.assertEqual(stages["case inventory"], "passed")
        self.assertEqual(stages["binary identity"], "passed")

    def test_inventory_mismatch_leaves_run_observations_unattributed(self):
        for replacement in ("Different", "First", None):
            with self.subTest(replacement=replacement):
                doc = ET.fromstring(XML)
                suite = doc.find("testsuite")
                second = suite.findall("testcase")[1]
                if replacement is None:
                    suite.remove(second)
                else:
                    second.set("name", replacement)
                ET.SubElement(ET.SubElement(doc, "properties"), "property",
                              name="neverd_execution_evidence",
                              value='{"record_comparison":{"compared":3,"matches":1},'
                                    '"caller_loop":"completed"}')
                with self.assertRaisesRegex(ValueError, "test identities differ"):
                    self.invoke(xml=ET.tostring(doc, encoding="unicode"))
                self.assertIsNone(self.verification["record_comparison"])
                self.assertEqual(self.verification["caller_loop"], "unknown")
                self.assertEqual(self.verification["first_blocker"]["path"], ["case inventory"])

    def test_replaced_binary_cannot_supply_observations_on_a_skipped_case(self):
        doc = ET.fromstring(XML)
        ET.SubElement(ET.SubElement(doc, "properties"), "property",
                      name="neverd_execution_evidence", value='{"caller_loop":"completed"}')
        ET.SubElement(doc.find(".//testcase"), "skipped", message="no reference")
        with self.assertRaisesRegex(ValueError, "binary changed"):
            self.invoke(xml=ET.tostring(doc, encoding="unicode"), replace_binary=True)
        self.assertEqual(self.verification["first_blocker"]["path"], ["binary identity"])
        self.assertIsNone(self.verification["record_comparison"])
        self.assertEqual(self.verification["caller_loop"], "unknown")

    def test_child_observations_cannot_claim_the_caller_of_the_entire_run(self):
        doc = ET.fromstring(XML)
        prop = ET.SubElement(doc.find(".//testcase"), "properties")
        ET.SubElement(prop, "property", name="neverd_execution_evidence",
                      value='{"caller_loop":"completed"}')
        self.invoke(xml=ET.tostring(doc, encoding="unicode"))
        self.assertEqual(self.verification["caller_loop"], "unknown")

    def test_invalid_run_observations_keep_their_own_failure_stage(self):
        doc = ET.fromstring(XML)
        ET.SubElement(ET.SubElement(doc, "properties"), "property",
                      name="neverd_execution_evidence", value='{"caller_loop":true}')
        with self.assertRaisesRegex(ValueError, "caller loop"):
            self.invoke(xml=ET.tostring(doc, encoding="unicode"))
        self.assertEqual(self.verification["first_blocker"]["path"], ["result metadata"])
        self.assertEqual(self.verification["caller_loop"], "unknown")

    def test_observed_mismatch_is_a_failure_with_its_own_detail(self):
        doc = ET.fromstring(XML)
        ET.SubElement(doc.find(".//testcase"), "failure").text = "expected 5, got 4"
        with self.assertRaisesRegex(ValueError, "expected 5, got 4"):
            self.invoke(xml=ET.tostring(doc, encoding="unicode"), exit_code=1)
        first = self.verification["first_blocker"]
        self.assertEqual(first["state"], "failed")
        self.assertNotIn("missing_evidence", first)

    def test_discovery_failure_clears_previous_success(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(error=subprocess.CalledProcessError(1, "tests.exe"))

    def test_failure_or_missing_xml_cannot_reuse_previous_success(self):
        with self.assertRaisesRegex(ValueError, "exit code"):
            self.invoke(exit_code=1)
        with self.assertRaises(FileNotFoundError):
            self.invoke(xml=None, exit_code=7)
        self.assertEqual(self.verification["first_blocker"]["path"], ["results"])
        self.assertIn("process exit 7", self.verification["first_blocker"]["diagnostic"])
        self.assertIn("fresh GoogleTest XML", self.verification["first_blocker"]["missing_evidence"])

    def test_discovered_case_cannot_be_missing_or_skipped(self):
        with self.assertRaisesRegex(ValueError, "identities differ"):
            self.invoke(discovery=LISTING + "  Third\n")
        with self.assertRaisesRegex(ValueError, "did not pass"):
            self.invoke(xml=XML.replace('name="First" status="run"',
                                        'name="First" status="notrun"'))

    def test_output_cannot_delete_the_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "tests.exe"
            binary.write_bytes(b"preserve")
            with self.assertRaisesRegex(ValueError, "overwrite"):
                qualification.run(binary, binary, "*", 1, 30)
            self.assertEqual(binary.read_bytes(), b"preserve")


class VerificationReportTests(unittest.TestCase):
    def setUp(self):
        self.data = {
            "schema": "neverd.verification.v1",
            "stages": [
                {"path": ["read"], "state": "passed"},
                {"path": ["compare", "reference"], "state": "blocked",
                 "diagnostic": "comparison has no reference",
                 "missing_evidence": "reference output for this input"},
                {"path": ["export"], "state": "not_run"},
            ],
            "record_comparison": {"total": 3, "compared": 3, "matches": 0},
            "caller_loop": "not_run",
        }

    def test_first_blocker_and_two_independent_scopes(self):
        result = verification_report.summarize(self.data)
        self.assertEqual(result["first_blocker"], self.data["stages"][1])
        text = verification_report.render(result)
        for expected in ("compare > reference", "reference output for this input",
                         "3 of 3 compared", "0 matches", "Caller loop: not run",
                         "export: not run"):
            self.assertIn(expected, text)
        self.data["caller_loop"] = "started"
        self.assertEqual(verification_report.summarize(self.data)["caller_loop"], "started")

    def test_missing_evidence_cannot_be_inferred_from_other_observations(self):
        del self.data["caller_loop"]
        self.assertEqual(verification_report.summarize(self.data)["caller_loop"], "unknown")
        for state in ("not_run", "started", "completed", "unknown"):
            data = dict(self.data, caller_loop=state, record_comparison=None)
            result = verification_report.summarize(data)
            self.assertIsNone(result["record_comparison"])
            self.assertEqual(result["caller_loop"], state)
        self.data["record_comparison"] = {"total": 5, "compared": 2, "matches": 1}
        self.assertIn("2 of 5 compared", verification_report.render(verification_report.summarize(self.data)))

    def test_order_and_unknown_stage_do_not_imply_a_result(self):
        first = {"path": ["z"], "state": "failed", "diagnostic": "wrong value"}
        second = {"path": ["a"], "state": "blocked"}
        self.data["stages"] = [first, second]
        self.data["first_blocker"] = second
        self.assertEqual(verification_report.summarize(self.data)["first_blocker"], first)
        self.data["stages"] = [{"path": ["prior run"], "state": "unknown"}]
        result = verification_report.summarize(self.data)
        self.assertIsNone(result["first_blocker"])
        self.assertNotIn("passed", verification_report.render(result))

    def test_reader_cli_recomputes_the_blocker_and_rejects_bad_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "input.json"
            path.write_text(json.dumps(dict(self.data, first_blocker=None)))
            command = [sys.executable, str(Path(verification_report.__file__)), str(path), "--json"]
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)["first_blocker"], self.data["stages"][1])
            path.write_text("{")
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(result.stdout, "")

    def test_malformed_or_inconsistent_evidence_is_rejected(self):
        for comparison in ({"compared": True, "matches": 0},
                           {"compared": -1, "matches": 0},
                           {"compared": 2, "matches": 3},
                           {"compared": 3, "matches": 0, "total": 2}, {}):
            with self.subTest(comparison=comparison), self.assertRaises(ValueError):
                verification_report.summarize(dict(self.data, record_comparison=comparison))
        for field, value in (("schema", 1), ("caller_loop", True), ("stages", {}),
                             ("stages", [{"path": [], "state": "blocked"}]),
                             ("stages", [{"path": ["check"], "state": "complete-ish"}])):
            with self.subTest(field=field), self.assertRaises(ValueError):
                verification_report.summarize(dict(self.data, **{field: value}))
        data = copy.deepcopy(self.data)
        data["stages"][1]["diagnostic"] = False
        with self.assertRaises(ValueError):
            verification_report.summarize(data)


if __name__ == "__main__":
    unittest.main()
