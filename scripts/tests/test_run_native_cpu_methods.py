import contextlib
import copy
import io
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from scripts import run_native_cpu_methods as methods


class NativeMethodEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.document = {"kind": "ctestInfo", "version": {"major": 1}, "tests": [
            {"name": "Pretty/Native.Execute/value" + str(i),
             "command": [str(self.root / "Owner"), "--gtest_filter=Native.Execute/" + str(i),
                         "--gtest_also_run_disabled_tests"],
             "properties": [
                 {"name": "LABELS", "value": ["Owner"]},
                 {"name": "ENVIRONMENT", "value": ["NEVERD_SIGNATURE_CACHE=off"]},
                 {"name": "SKIP_REGULAR_EXPRESSION", "value": [r"\[  SKIPPED \]"]},
                 {"name": "TIMEOUT", "value": 20.0},
                 {"name": "WORKING_DIRECTORY", "value": str(self.root)}]}
            for i in range(2)]}
        self.xml = self.root / "result.xml"
        self.expected = next(iter(methods.method_inventory(self.document).values()))

    def write_xml(self, names=("Execute/0", "Execute/1"), skipped=False):
        root = ET.Element("testsuites", tests=str(len(names)), failures="0", disabled="0", errors="0")
        suite = ET.SubElement(root, "testsuite")
        for index, name in enumerate(names):
            case = ET.SubElement(suite, "testcase", classname="Native", name=name,
                                 status="run", result="completed")
            if skipped and index == 1:
                case.set("result", "skipped")
                ET.SubElement(case, "skipped", message="foreign host ISA")
        ET.ElementTree(root).write(self.xml)

    def test_original_names_map_to_exact_ctest_identities(self):
        self.write_xml(skipped=True)
        cases = methods.read_results(self.xml, self.expected)
        self.assertEqual([c.test.name for c in cases], [t["name"] for t in self.document["tests"]])
        self.assertEqual([c.outcome for c in cases], ["passed", "skipped"])
        self.assertEqual(cases[1].message, "foreign host ISA")

    def test_missing_duplicate_and_unexpected_results_cannot_pass(self):
        for names in (("Execute/0",), ("Execute/0", "Execute/0"), ("Execute/0", "Other")):
            with self.subTest(names=names):
                self.write_xml(names)
                with self.assertRaises(ValueError):
                    methods.read_results(self.xml, self.expected)

    def test_incomplete_contradictory_and_false_totals_cannot_pass(self):
        for mutation in ("notrun", "wrong-total", "skip-and-pass", "failure-and-skip"):
            with self.subTest(mutation=mutation):
                self.write_xml()
                root = ET.parse(self.xml).getroot()
                case = next(root.iter("testcase"))
                if mutation == "notrun":
                    case.set("status", "notrun")
                elif mutation == "wrong-total":
                    root.set("tests", "0")
                else:
                    ET.SubElement(case, "skipped")
                    if mutation == "failure-and-skip":
                        ET.SubElement(case, "failure")
                ET.ElementTree(root).write(self.xml)
                with self.assertRaises(ValueError):
                    methods.read_results(self.xml, self.expected)

    def test_unknown_ctest_contracts_and_wildcards_fail_before_execution(self):
        mutations = [
            lambda d: d["tests"][0]["properties"].append({"name": "FIXTURES_REQUIRED", "value": ["fixture"]}),
            lambda d: d["tests"][0]["command"].append("--other"),
            lambda d: d["tests"][0]["command"].__setitem__(1, "--gtest_filter=Native.*"),
            lambda d: d["tests"].append(copy.deepcopy(d["tests"][0])),
            lambda d: d["tests"][0]["properties"].append({"name": "TIMEOUT", "value": 30}),
        ]
        for mutation in mutations:
            document = copy.deepcopy(self.document)
            mutation(document)
            with self.assertRaises(ValueError):
                methods.method_inventory(document)

    def test_distinct_environments_do_not_share_a_process(self):
        self.document["tests"][1]["properties"][1]["value"] = ["NEVERD_SIGNATURE_CACHE=on"]
        self.assertEqual(len(methods.method_inventory(self.document)), 2)

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_timeout_retires_a_real_child_and_preserves_status(self):
        evidence = self.root / "timeout"
        result = methods.execute([sys.executable, "-c", "import time; time.sleep(60)"],
                                 str(self.root), os.environ, 0.1, evidence)
        self.assertTrue(result["timed_out"])
        self.assertTrue(result["child_retired"])
        self.assertEqual(result["status"], -signal.SIGKILL)
        self.assertEqual(json.loads((evidence / "status.json").read_text()), result)

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_unretired_child_is_bounded_and_cannot_look_successful(self):
        child = mock.Mock(pid=12345)
        child.wait.side_effect = subprocess.TimeoutExpired("test", 1)
        with mock.patch.object(methods.subprocess, "Popen", return_value=child), \
                mock.patch.object(methods.os, "killpg") as kill:
            result = methods.execute(["test"], str(self.root), {}, 1, self.root / "stuck")
        kill.assert_called_once_with(12345, signal.SIGKILL)
        self.assertFalse(result["child_retired"])
        self.assertIsNone(result["status"])
        self.assertEqual(child.wait.call_count, 2)

    def test_stale_output_directory_cannot_supply_new_evidence(self):
        (self.root / "stale").mkdir()
        with self.assertRaises(FileExistsError):
            methods.execute(["test"], str(self.root), {}, 1, self.root / "stale")

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_method_run_retains_environment_exact_filters_and_exit_failure(self):
        self.write_xml(skipped=True)
        def execute(command, directory, environment, timeout, evidence):
            self.assertEqual(command[1], "--gtest_filter=Native.Execute/0:Native.Execute/1")
            self.assertEqual(directory, str(self.root))
            self.assertEqual(environment["NEVERD_REQUIRE_HVF"], "1")
            self.assertEqual(environment["NEVERD_SIGNATURE_CACHE"], "off")
            evidence.mkdir(parents=True)
            (evidence / "results.xml").write_bytes(self.xml.read_bytes())
            return {"status": 7, "timed_out": False, "child_retired": True}
        with mock.patch.object(methods, "execute", side_effect=execute), \
                contextlib.redirect_stdout(io.StringIO()):
            cases, status = methods.run_methods(self.document, self.root / "evidence",
                                                {"NEVERD_REQUIRE_HVF": "1"})
        self.assertEqual(status, 1)
        self.assertEqual(len(cases), 2)
        self.assertTrue((self.root / "evidence/method-results.json").exists())

    @unittest.skipUnless(os.name == "posix", "requires POSIX process groups")
    def test_environment_cannot_disable_native_enforcement(self):
        for test in self.document["tests"]:
            test["properties"][1]["value"] = ["NEVERD_REQUIRE_HVF=0"]
        with self.assertRaisesRegex(ValueError, "disables required native coverage"):
            methods.run_methods(self.document, self.root / "evidence", {"NEVERD_REQUIRE_HVF": "1"})


if __name__ == "__main__":
    unittest.main()
