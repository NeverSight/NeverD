import copy
import json
import unittest
import xml.etree.ElementTree as ET

from scripts import run_lift_qualification as qualification


class LiftQualificationTests(unittest.TestCase):
    def setUp(self):
        self.names = ("Example.First", "Example.Second")
        self.xml = ET.fromstring('''<testsuites tests="2" failures="0" disabled="0" errors="0">
          <testsuite tests="2" failures="0" disabled="0" errors="0">
            <testcase classname="Example" name="First" status="run" result="completed"/>
            <testcase classname="Example" name="Second" status="run" result="completed"/>
          </testsuite></testsuites>''')

    def test_requires_all_previous_identities(self):
        document = json.loads(qualification.MANIFEST.read_text())
        self.assertEqual(len(qualification.identities(document)), 108)
        for mutate in (
            lambda d: d["tests"].pop(),
            lambda d: d["tests"].__setitem__(0, d["tests"][1]),
            lambda d: d["tests"][0].__setitem__("baseline", "passed"),
            lambda d: d["tests"][0].__setitem__("name", "Example.*"),
        ):
            bad = copy.deepcopy(document)
            mutate(bad)
            with self.assertRaises(ValueError):
                qualification.identities(bad)

    def test_renames_retain_the_historical_case_and_outcome(self):
        document = json.loads(qualification.MANIFEST.read_text())
        old = "X86_32_X87FPU.NativeAndLiftedCallLoopsReturnTheIndependentSum"
        current = "X87CallReplay.NativeAndLiftedCallLoopsReturnTheIndependentSum"
        self.assertIn(old, [case["name"] for case in document["tests"]])
        self.assertIn(current, qualification.identities(document))
        self.assertNotIn(old, qualification.identities(document))
        case = next(case for case in document["tests"] if case["name"] == old)
        self.assertEqual(case["baseline"], "skipped")

    def test_current_identities_cannot_collide_or_select_multiple_tests(self):
        document = json.loads(qualification.MANIFEST.read_text())
        for invalid in (document["tests"][1]["name"], "Example.*", "", None, []):
            bad = copy.deepcopy(document)
            bad["tests"][0]["current_name"] = invalid
            with self.assertRaises(ValueError):
                qualification.identities(bad)

    def test_accepts_complete_individual_passes(self):
        self.assertEqual(qualification.audit(self.xml, self.names, 0)["passed"], 2)

    def test_skip_is_not_a_pass(self):
        case = self.xml.find(".//testcase")
        case.set("result", "skipped")
        ET.SubElement(case, "skipped", message="toolchain missing")
        with self.assertRaisesRegex(ValueError, "did not pass"):
            qualification.audit(self.xml, self.names, 0)

    def test_failure_cannot_hide_behind_successful_totals(self):
        ET.SubElement(self.xml.find(".//testcase"), "failure")
        with self.assertRaisesRegex(ValueError, "did not pass"):
            qualification.audit(self.xml, self.names, 0)

    def test_disabled_and_unfinished_cases_are_not_passes(self):
        for attribute, value in (("status", "notrun"), ("result", "suppressed"),
                                 ("result", "")):
            xml = copy.deepcopy(self.xml)
            xml.find(".//testcase").set(attribute, value)
            with self.assertRaisesRegex(ValueError, "did not pass"):
                qualification.audit(xml, self.names, 0)

    def test_missing_extra_and_duplicate_identities_fail(self):
        for name in ("First", "Unexpected"):
            xml = copy.deepcopy(self.xml)
            xml.findall(".//testcase")[1].set("name", name)
            with self.assertRaisesRegex(ValueError, "identities differ"):
                qualification.audit(xml, self.names, 0)
        self.xml.find("testsuite").remove(self.xml.findall(".//testcase")[1])
        with self.assertRaisesRegex(ValueError, "missing"):
            qualification.audit(self.xml, self.names, 0)

    def test_crashed_process_cannot_reuse_successful_xml(self):
        with self.assertRaisesRegex(ValueError, "exit code"):
            qualification.audit(self.xml, self.names, -11)

    def test_totals_are_reconciled_at_both_levels(self):
        for element in (self.xml, self.xml.find("testsuite")):
            for attribute, value in (("tests", "1"), ("errors", "1"),
                                     ("failures", "1"), ("disabled", "1")):
                old = element.get(attribute)
                element.set(attribute, value)
                with self.assertRaises(ValueError):
                    qualification.audit(self.xml, self.names, 0)
                element.set(attribute, old)


if __name__ == "__main__":
    unittest.main()
