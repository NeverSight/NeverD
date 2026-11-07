import json
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from scripts.audit_ci_test_inventory import TestRecord, parse_inventory
from scripts.audit_ci_test_results import (
    OPTIONAL_KERNEL_INPUT_SKIPS,
    ResultError,
    audit_results,
    combine_prior_results,
    parse_junit,
    prior_results,
)
from scripts.tests.test_audit_ci_test_results import junit, policy_records, record


class PriorPassTests(unittest.TestCase):
    def test_only_consistent_passes_and_skips_are_reused_with_their_original_output(self):
        passed, skipped, failed = [record(name, "Owner") for name in ("a", "b", "c")]
        source = junit((passed, skipped, failed), {
            passed: ("run", "", "actual original execution"),
            skipped: ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "unavailable"),
            failed: ("fail", "failed", "actual failure"),
        })
        kept = prior_results((passed, skipped, failed), (passed, skipped, failed), (source,))
        outcomes = parse_junit(kept)
        self.assertEqual([item.test for item in outcomes], [passed, skipped])
        self.assertEqual(outcomes[0].output, "actual original execution")
        self.assertEqual(outcomes[1].outcome, "skipped")
        self.assertEqual(outcomes[1].output, "unavailable")
        # Another completed shard disagreeing about the pass forces a rerun.
        contrary = junit((passed,), {passed: ("fail", "failed", "second failure")})
        self.assertEqual(len(prior_results((passed, skipped, failed), (passed,), (source,))), 1)
        kept = prior_results((passed, skipped, failed), (passed,), (source, contrary))
        self.assertEqual(parse_junit(kept), ())
        # So does a shard disagreeing about whether the case could execute.
        ran = junit((skipped,))
        kept = prior_results((passed, skipped, failed), (skipped,), (source, ran))
        self.assertEqual(parse_junit(kept), ())

    def test_disabled_and_infrastructure_not_run_cases_are_never_reused(self):
        disabled, not_run = record("a", "Owner"), record("b", "Owner")
        source = junit((disabled, not_run), {
            disabled: ("disabled", "", ""),
            not_run: ("notrun", "Not Run: missing executable", ""),
        })
        kept = prior_results((disabled, not_run), (disabled, not_run), (source,))
        self.assertEqual(parse_junit(kept), ())

    def test_excluded_profile_cases_are_not_reused(self):
        selected, excluded = record("a", "Owner"), record("b", "OtherOwner")
        kept = prior_results((selected, excluded), (selected,), (junit((excluded, selected)),))
        self.assertEqual([item.test for item in parse_junit(kept)], [selected])

    def test_same_name_with_multiple_owners_moves_only_as_a_complete_group(self):
        focused, aggregate = record("a", "Focused"), record("a", "Aggregate")
        kept = prior_results((focused, aggregate), (focused, aggregate), (junit((focused,)),))
        self.assertEqual(parse_junit(kept), ())
        kept = prior_results((focused, aggregate), (focused, aggregate),
                            (junit((focused, aggregate)),))
        self.assertEqual({item.test for item in parse_junit(kept)}, {focused, aggregate})

    def test_changed_owners_and_stale_cases_fail_before_exclusion(self):
        known = record("a", "Owner")
        for unknown in (record("a", "ChangedOwner"), record("stale", "Owner")):
            with self.subTest(unknown=unknown), self.assertRaisesRegex(ResultError, "unknown inventory"):
                prior_results((known,), (known,), (junit((unknown,)),))

    def test_contradictory_counts_and_duplicate_cases_are_rejected(self):
        known = record("a", "Owner")
        for source in (junit((known, known)), junit((known,))):
            if len(source) == 1:
                source.set("tests", "2")
            with self.assertRaises(ResultError):
                prior_results((known,), (known,), (source,))

    def test_name_files_cannot_split_one_identity_into_multiple_tests(self):
        known = record("a\nb", "Owner")
        with self.assertRaisesRegex(ResultError, "name file"):
            prior_results((known,), (known,), (junit((known,)),))

    def test_fresh_and_prior_results_must_be_disjoint_and_prior_must_be_final(self):
        known = record("a", "Owner")
        passed = parse_junit(junit((known,)))
        failed = parse_junit(junit((known,), {known: ("fail", "failure", "")}))
        with self.assertRaisesRegex(ResultError, "duplicate identities"):
            combine_prior_results(passed, passed)
        with self.assertRaisesRegex(ResultError, "other than passed or skipped"):
            combine_prior_results((), failed)
        skipped = parse_junit(junit((known,), {
            known: ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "unavailable"),
        }))
        self.assertEqual(combine_prior_results((), skipped), skipped)

    def test_a_reused_skip_in_a_required_suite_still_fails_the_audit(self):
        selected = policy_records("windows-focused")
        required = next(test for test in selected if test.labels & {"NeverDSafetyTests"})
        source = junit(selected, {
            required: ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", "unavailable"),
        })
        retained = parse_junit(prior_results(selected, selected, (source,)))
        self.assertEqual(len(retained), len(selected))
        report = audit_results(selected, combine_prior_results((), retained),
                               "windows-focused", 0)
        self.assertFalse(report["ok"])
        self.assertEqual(report["counts"]["required_skips"], 1)

    def test_complete_inventory_and_nonzero_exit_remain_authoritative(self):
        selected = policy_records("windows-focused")
        previous = parse_junit(junit(selected[:3]))
        current = parse_junit(junit(selected[3:]))
        combined = combine_prior_results(current, previous)
        good = audit_results(selected, combined, "windows-focused", 0)
        self.assertTrue(good["ok"], good)
        self.assertEqual(good["counts"]["reported"], len(selected))
        self.assertFalse(audit_results(selected, combined[:-1], "windows-focused", 0)["ok"])
        self.assertFalse(audit_results(selected, combined, "windows-focused", 8)["ok"])

    def test_real_ctest_reuses_a_pass_and_a_skip_and_reruns_only_the_failure(self):
        with tempfile.TemporaryDirectory(prefix="neverd-ci-reuse-") as directory:
            root = Path(directory)
            child = root / "child.py"
            child.write_text(
                "import pathlib,sys\n"
                "p=pathlib.Path(sys.argv[1]);p.write_text(p.read_text()+'x' if p.exists() else 'x')\n"
                "print(sys.argv[2]);sys.exit(1 if sys.argv[2]=='FAIL' else 0)\n",
                encoding="utf-8",
            )
            cmake = ""
            for name, state in (("Passed", "PASS"), ("Skipped", "SKIP"), ("Failed", "FAIL")):
                cmake += (
                    f'add_test({name} "{Path(sys.executable).as_posix()}" '
                    f'"{child.as_posix()}" "{(root / name).as_posix()}" {state})\n'
                    f'set_tests_properties({name} PROPERTIES LABELS "Owner" '
                    'SKIP_REGULAR_EXPRESSION "SKIP")\n'
                )
            (root / "CTestTestfile.cmake").write_text(cmake, encoding="utf-8")
            discovery = subprocess.run(
                ["ctest", "--test-dir", str(root), "--show-only=json-v1"],
                capture_output=True, text=True, timeout=30, check=True,
            )
            records = parse_inventory(json.loads(discovery.stdout))
            first = root / "first.xml"
            result = subprocess.run(
                ["ctest", "--test-dir", str(root), "--output-junit", str(first)],
                capture_output=True, text=True, timeout=30,
            )
            self.assertNotEqual(result.returncode, 0)
            retained = prior_results(records, records, (ET.parse(first).getroot(),))
            previous = parse_junit(retained)
            self.assertEqual([item.test.name for item in previous], ["Passed", "Skipped"])
            names = root / "names.txt"
            names.write_text("Passed\nSkipped\n", encoding="utf-8")
            second = root / "second.xml"
            result = subprocess.run(
                ["ctest", "--test-dir", str(root), "--exclude-from-file", str(names),
                 "--output-junit", str(second)], capture_output=True, text=True, timeout=30,
            )
            self.assertNotEqual(result.returncode, 0)
            combined = combine_prior_results(parse_junit(ET.parse(second).getroot()), previous)
            self.assertEqual({item.test for item in combined}, set(records))
            self.assertEqual({item.test.name: item.outcome for item in combined}, {
                "Passed": "passed", "Skipped": "skipped", "Failed": "failed",
            })
            self.assertEqual((root / "Passed").read_text(), "x")
            self.assertEqual((root / "Skipped").read_text(), "x")
            self.assertEqual((root / "Failed").read_text(), "xx")


class OptionalKernelInputTests(unittest.TestCase):
    def check(self, test, reason, profile="linux-semantic", configured=frozenset()):
        selected = (*policy_records(profile), test)
        results = parse_junit(junit(selected, {
            test: ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", reason + "\n"),
        }))
        return audit_results(selected, results, profile, 0, configured)

    def test_exact_external_inputs_are_reported_unavailable_on_each_host(self):
        for profile in ("linux-semantic", "macos-patch", "windows-focused"):
            for test, (_, reason) in OPTIONAL_KERNEL_INPUT_SKIPS.items():
                with self.subTest(profile=profile, test=test):
                    report = self.check(test, reason, profile)
                    self.assertTrue(report["ok"], report)
                    self.assertEqual(report["counts"]["permitted_external_input_skips"], 1)
                    self.assertEqual(report["counts"]["passed"], len(policy_records(profile)))

    def test_configured_inputs_cannot_skip(self):
        for test, (inputs, reason) in OPTIONAL_KERNEL_INPUT_SKIPS.items():
            with self.subTest(test=test):
                report = self.check(test, reason, configured=inputs)
                self.assertFalse(report["ok"])
                self.assertEqual(report["counts"]["required_skips"], 1)

    def test_wrong_reason_owner_and_new_cases_remain_required(self):
        test, (_, reason) = next(iter(OPTIONAL_KERNEL_INPUT_SKIPS.items()))
        self.assertFalse(self.check(test, "other reason")["ok"])
        self.assertFalse(self.check(test, "prefix " + reason)["ok"])
        self.assertFalse(self.check(record(test.name, "NeverDSessionLLVMTests"), reason)["ok"])
        self.assertFalse(self.check(record(test.name + "Extra", "NeverDSessionCAPITests"), reason)["ok"])


if __name__ == "__main__":
    unittest.main()
