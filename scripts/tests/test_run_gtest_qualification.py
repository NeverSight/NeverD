import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from scripts import run_gtest_qualification as qualification


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

    def invoke(self, xml=XML, exit_code=0, discovery=LISTING, error=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary, output = root / "tests.exe", root / "result.xml"
            binary.write_bytes(b"the qualified binary")
            receipt = output.with_suffix(".qualification.json")
            output.write_text(XML)
            receipt.write_text('{"old_success": true}')

            def execute(args, **kwargs):
                if "--gtest_list_tests" in args:
                    self.assertFalse(output.exists())
                    self.assertFalse(receipt.exists())
                    self.assertEqual(args[1], "--gtest_filter=Example.*")
                    if error:
                        raise error
                    return subprocess.CompletedProcess(args, 0, stdout=discovery)
                self.assertEqual(args[1], "--gtest_filter=Example.*")
                if xml is not None:
                    output.write_text(xml)
                return subprocess.CompletedProcess(args, exit_code)

            with patch.object(qualification.subprocess, "run", side_effect=execute):
                try:
                    report = qualification.run(binary, output, "Example.*", 2, 30)
                except Exception:
                    self.assertFalse(receipt.exists())
                    raise
            self.assertEqual(json.loads(receipt.read_text()), report)
            self.assertEqual(report["outcome"]["passed"], 2)

    def test_complete_run_publishes_audited_identities(self):
        self.invoke()

    def test_discovery_failure_clears_previous_success(self):
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(error=subprocess.CalledProcessError(1, "tests.exe"))

    def test_failure_or_missing_xml_cannot_reuse_previous_success(self):
        with self.assertRaisesRegex(ValueError, "exit code"):
            self.invoke(exit_code=1)
        with self.assertRaises(FileNotFoundError):
            self.invoke(xml=None)

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


if __name__ == "__main__":
    unittest.main()
