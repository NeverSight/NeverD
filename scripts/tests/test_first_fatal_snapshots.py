"""CI regressions for the original 741 KMP failure receipt shapes."""

import io
import subprocess
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

from scripts import collect_first_fatal_snapshots as collector


OWNER = "NeverDSemanticTests"


class OriginalCTestCommandTests(unittest.TestCase):
    @staticmethod
    def windows_command(function="x86sww_deepnest"):
        # Main 34631601930/a1, Windows full artifact 10278829154. These are
        # the retained inventory command arguments, not a locally run command.
        return [
            "C:/Program Files/CMake/bin/cmake.exe",
            "-D", "TEST_EXECUTABLE=D:/a/NeverD/NeverD/build-ci/bin/NeverDSwitchXformTests.exe",
            "-D", "TEST_EXECUTOR=",
            "-D", "TEST_FILTER=SwXform4/X86SwXformRT.Verify/" + function,
            "-D", "TEST_XML_OUTPUT=",
            "-D", "TEST_EXTRA_ARGS=",
            "-P", "C:/Program Files/CMake/share/cmake-4.4/Modules/GoogleTest/LaunchTest.cmake",
        ]

    def test_original_linux_direct_commands_keep_their_owned_filters(self):
        for suite, function in (("X64", "x64o105_kmp"), ("X86", "x86o105_kmp")):
            gtest = f"OptStress105/{suite}OptStress105RT.Verify/{function}"
            command = ["/home/runner/work/NeverD/NeverD/build-ci/bin/NeverDSemanticTests",
                       "--gtest_filter=" + gtest, "--gtest_also_run_disabled_tests"]
            with self.subTest(function=function):
                self.assertEqual(collector.command_filter({"command": command}, OWNER), gtest)

    def test_original_windows_wrappers_keep_both_owned_filters(self):
        for function in ("x86sww_deepnest", "x86sww_chained"):
            with self.subTest(function=function):
                self.assertEqual(
                    collector.command_filter({"command": self.windows_command(function)},
                                             "NeverDSwitchXformTests"),
                    "SwXform4/X86SwXformRT.Verify/" + function)

    def test_matching_filter_cannot_substitute_another_test_executable(self):
        direct = {"command": ["/build/NeverDOtherTests", "--gtest_filter=Example.Case"]}
        self.assertIsNone(collector.command_filter(direct, OWNER))
        wrapper = self.windows_command()
        wrapper[2] = "TEST_EXECUTABLE=D:/build/NeverDOtherTests.exe"
        self.assertIsNone(collector.command_filter({"command": wrapper}, "NeverDSwitchXformTests"))

    def test_duplicate_or_conflicting_filters_are_rejected(self):
        for second in ("Example.Case", "Example.Other"):
            direct = ["/build/NeverDSemanticTests", "--gtest_filter=Example.Case",
                      "--gtest_filter=" + second]
            with self.subTest(second=second):
                self.assertIsNone(collector.command_filter({"command": direct}, OWNER))
        wrapper = self.windows_command()
        wrapper[10] = wrapper[6]  # Duplicate TEST_FILTER, not TEST_EXTRA_ARGS.
        self.assertIsNone(collector.command_filter({"command": wrapper}, "NeverDSwitchXformTests"))
        wrapper = self.windows_command()
        wrapper[10] = "TEST_EXTRA_ARGS=--gtest_filter=Example.Other"
        self.assertIsNone(collector.command_filter({"command": wrapper}, "NeverDSwitchXformTests"))

    def test_unobserved_wrapper_launch_contracts_are_rejected(self):
        for index, value in ((0, "other.exe"), (1, "-U"),
                             (4, "TEST_EXECUTOR=other.exe"),
                             (12, "C:/Other/LaunchTest.cmake")):
            wrapper = self.windows_command()
            wrapper[index] = value
            with self.subTest(index=index):
                self.assertIsNone(collector.command_filter({"command": wrapper},
                                                          "NeverDSwitchXformTests"))
        self.assertIsNone(collector.command_filter({"command": []}, OWNER))


def junit(name, status="fail", child="failure", message="Failed"):
    root = ET.Element("testsuite", {
        "tests": "1", "failures": str(int(child == "failure")),
        "disabled": str(int(status == "disabled")), "skipped": str(int(child == "skipped")),
    })
    case = ET.SubElement(root, "testcase", {"name": name, "status": status})
    properties = ET.SubElement(case, "properties")
    ET.SubElement(properties, "property", {"name": "cmake_labels", "value": OWNER})
    if child:
        ET.SubElement(case, child, {"message": message})
    return root


class OriginalCTestFailureReceiptTests(unittest.TestCase):
    def test_741_parameterized_failures_retain_their_actual_failed_state(self):
        # Original Main 34631601930/a1, Linux, CTest 10983/10986. The FAILED
        # parameter suffixes/durations below are copied from its JUnit stdout.
        # New observation runs use CTest verbose output, which adds the id.
        for target, number, duration in (("x64", 10983, 108), ("x86", 10986, 75)):
            with self.subTest(target=target):
                suite = "X64" if target == "x64" else "X86"
                function = target + "o105_kmp"
                gtest = f"OptStress105/{suite}OptStress105RT.Verify/{function}"
                param = "OptStress105/" + function
                name = gtest + "  # GetParam() = " + param
                raw = (f"{number}: [ RUN      ] {gtest}\n"
                       f"{number}: [  FAILED  ] {gtest}, where GetParam() = {param} ({duration} ms)\n"
                       f"{number}: [  PASSED  ] 0 tests.\n"
                       f"{number}: [  FAILED  ] 1 test, listed below:\n"
                       f"{number}: [  FAILED  ] {gtest}, where GetParam() = {param}\n")
                result = collector.audit_execution(junit(name), raw, {"name": name}, gtest, OWNER, 8)
                self.assertEqual(result["semantic_outcome"], "failed")
                self.assertEqual(result["errors"], [])
                self.assertEqual(len(result["raw_completions"]), 1)
                self.assertEqual(result["raw_completions"][0][3], param)

    def test_normal_pass_uses_run_status_and_ok_completion(self):
        gtest = "Example.ActualPass"
        raw = f"1: [ RUN      ] {gtest}\n1: [       OK ] {gtest} (1 ms)\n"
        result = collector.audit_execution(junit(gtest, "run", None), raw,
                                           {"name": gtest}, gtest, OWNER, 0)
        self.assertEqual(result["semantic_outcome"], "passed")
        self.assertEqual(result["errors"], [])

    def test_explicit_notrun_skip_stays_skipped(self):
        gtest = "Example.Skipped"
        raw = f"1: [ RUN      ] {gtest}\n1: [  SKIPPED ] {gtest} (0 ms)\n"
        result = collector.audit_execution(
            junit(gtest, "notrun", "skipped", "SKIP_REGULAR_EXPRESSION_MATCHED"),
            raw, {"name": gtest}, gtest, OWNER, 0)
        self.assertEqual(result["semantic_outcome"], "skipped")
        self.assertIn("selected registration was skipped", result["errors"])

    def test_contradictory_status_and_failure_child_are_rejected(self):
        with self.assertRaises(ValueError):
            collector.audit_execution(junit("Example.Bad", "run"), "",
                                       {"name": "Example.Bad"}, "Example.Bad", OWNER, 8)

    def test_failed_xml_cannot_be_accepted_with_an_ok_ending(self):
        gtest = "Example.Failed"
        raw = f"1: [ RUN      ] {gtest}\n1: [       OK ] {gtest} (1 ms)\n"
        result = collector.audit_execution(junit(gtest), raw, {"name": gtest}, gtest, OWNER, 8)
        self.assertEqual(result["semantic_outcome"], "failed")
        self.assertIn("raw GTest completion identity/state differs from XML", result["errors"])

    def test_failed_xml_cannot_be_accepted_with_zero_ctest_exit(self):
        gtest = "Example.Failed"
        raw = f"1: [ RUN      ] {gtest}\n1: [  FAILED  ] {gtest} (1 ms)\n"
        result = collector.audit_execution(junit(gtest), raw, {"name": gtest}, gtest, OWNER, 0)
        self.assertIn("failed XML and CTest exit status differ", result["errors"])

    def test_other_test_completion_is_rejected(self):
        gtest = "Example.Failed"
        raw = f"1: [ RUN      ] {gtest}\n1: [  FAILED  ] Example.Other (1 ms)\n"
        result = collector.audit_execution(junit(gtest), raw, {"name": gtest}, gtest, OWNER, 8)
        self.assertIn("raw GTest completion identity/state differs from XML", result["errors"])


class DiagnosticSupervisorTests(unittest.TestCase):
    def test_outer_allowance_preserves_the_existing_native_timeout(self):
        for native in (120, 600):
            test = {"properties": [{"name": "TIMEOUT", "value": native}]}
            self.assertEqual(collector.execution_timeout(test), native + 60)
        with self.assertRaises(ValueError):
            collector.execution_timeout({"properties": []})

    def test_timeout_retains_actual_return_code_and_records_cleanup(self):
        process = mock.Mock(returncode=-9)
        process.wait.side_effect = [subprocess.TimeoutExpired(["ctest"], 60), -9]
        with mock.patch.object(collector.subprocess, "Popen", return_value=process), \
                mock.patch.object(collector, "terminate_group", return_value=[]) as terminate:
            result = collector.run_bounded(["ctest"], io.BytesIO(), io.BytesIO(), timeout=60)
        self.assertTrue(result["timed_out"])
        self.assertEqual(result["returncode"], -9)
        self.assertEqual(result["cleanup_errors"], [])
        terminate.assert_called_once_with(process)
        process.wait.assert_has_calls([mock.call(timeout=60), mock.call(timeout=10)])

    def test_unfinished_cleanup_is_explicitly_incomplete(self):
        process = mock.Mock(returncode=None)
        process.wait.side_effect = [subprocess.TimeoutExpired(["ctest"], 60),
                                    subprocess.TimeoutExpired(["ctest"], 10)]
        with mock.patch.object(collector.subprocess, "Popen", return_value=process), \
                mock.patch.object(collector, "terminate_group", return_value=[]):
            result = collector.run_bounded(["ctest"], io.BytesIO(), io.BytesIO(), timeout=60)
        self.assertTrue(result["timed_out"])
        self.assertIsNone(result["returncode"])
        self.assertTrue(result["cleanup_errors"])


if __name__ == "__main__":
    unittest.main()
