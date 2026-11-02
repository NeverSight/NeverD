"""Native SP evidence binds each complete inventory to its own export."""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import verify_arm64e_sp_memory as verifier


class SPFaultEvidenceTests(unittest.TestCase):
    def report(self, name):
        expected = verifier.CONFIG["test_reports"][name]
        suites = {}
        for identity in expected["cases"]:
            suite, case = identity.rsplit(".", 1)
            suites.setdefault(suite, []).append(
                {"name": case, "status": "RUN", "result": "COMPLETED"})
        suites = [{"name": name, "testsuite": cases}
                  for name, cases in suites.items()]
        suites[0]["testsuite"][0][expected["artifact_property"]] = self.id()
        return {"tests": len(expected["cases"]), "failures": 0, "errors": 0,
                "testsuites": suites}

    def test_every_profile_requires_its_complete_report(self):
        for name in verifier.CONFIG["test_reports"]:
            with self.subTest(profile=name):
                report = self.report(name)
                self.assertEqual(verifier.validate_report(report, name), self.id())
                case = report["testsuites"][-1]["testsuite"][-1]
                for field, value in (("result", "SKIPPED"),
                                     ("status", "NOTRUN"),
                                     ("name", "missing"),
                                     ("failures", [{"failure": "failed"}])):
                    bad = copy.deepcopy(report)
                    bad["testsuites"][-1]["testsuite"][-1][field] = value
                    with self.assertRaises(ValueError):
                        verifier.validate_report(bad, name)
                self.assertEqual(case["result"], "COMPLETED")

    def test_other_families_cannot_lend_their_artifact_export(self):
        for name, expected in verifier.CONFIG["test_reports"].items():
            report = self.report(name)
            case = report["testsuites"][0]["testsuite"][0]
            del case[expected["artifact_property"]]
            with self.assertRaises(ValueError):
                verifier.validate_report(report, name)
            case[expected["artifact_property"]] = self.id()
            report["testsuites"][-1]["testsuite"][-1][expected["artifact_property"]] = self.id()
            with self.assertRaises(ValueError):
                verifier.validate_report(report, name)

    def test_extended_profiles_keep_specific_counts_and_false_abi_claims(self):
        counts = {"sp-writeback": 130, "sp-vector-writeback": 100,
                  "sp-pair": 215, "sp-neon": 48, "sp-neon-tail": 30}
        for name, count in counts.items():
            profile = verifier.CONFIG["profiles"][name]
            self.assertEqual(profile["fault_observations"]["raw_sp_faults"], count)
            self.assertFalse(profile["fault_observations"]["stack_abi_qualified"])
            self.assertIn("-DSP_TEST_CASE_COUNT=" + str(count), profile["flags"])
            self.assertIn("-Werror", profile["flags"])
            self.assertEqual(profile["flags"][-1], "-Wno-unused-parameter")
            self.assertEqual(profile["test_reports"], {"stack": name})
        for name in ("immediate", "register"):
            self.assertIn("control", verifier.CONFIG["profiles"][name]["test_reports"])


if __name__ == "__main__":
    unittest.main()
