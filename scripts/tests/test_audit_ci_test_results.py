import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

from scripts.audit_ci_test_inventory import TestRecord, parse_inventory
from scripts.audit_ci_test_results import (
    NON_APPLE_CORPUS_SKIPS,
    ResultError,
    audit_results,
    check_ctest_version,
    format_summary,
    parse_junit,
    read_exit_status,
    required_labels,
)


def record(name, *labels):
    return TestRecord(name, frozenset(labels))


def policy_records(profile="linux-semantic"):
    return tuple(
        record("case/" + label, label) for label in sorted(required_labels(profile))
    )


def junit(records, changes=None):
    changes = changes or {}
    root = ET.Element(
        "testsuite", tests=str(len(records)), failures="0", skipped="0", disabled="0"
    )
    counts = {"failures": 0, "skipped": 0, "disabled": 0}
    for test in records:
        state, message, output = changes.get(test, ("run", "", ""))
        case = ET.SubElement(
            root,
            "testcase",
            name=test.name,
            classname=test.name,
            status=state,
            time="0",
        )
        if state == "fail":
            ET.SubElement(case, "failure", message=message)
            counts["failures"] += 1
        elif state == "notrun":
            ET.SubElement(case, "skipped", message=message)
            counts["skipped"] += 1
        elif state == "disabled":
            counts["disabled"] += 1
        properties = ET.SubElement(case, "properties")
        if test.labels:
            ET.SubElement(
                properties,
                "property",
                name="cmake_labels",
                value=";".join(sorted(test.labels)),
            )
        ET.SubElement(case, "system-out").text = output
    root.attrib.update({name: str(value) for name, value in counts.items()})
    return root


def workflow_bash():
    if sys.platform != "win32":
        bash = shutil.which("bash")
        if bash:
            return bash
        raise unittest.SkipTest("the CI bash shell is unavailable")

    # PATH can resolve bash to the WSL launcher, not the native CI shell.
    git = shutil.which("git")
    if git:
        root = Path(git).resolve().parent.parent
        if root.name.lower() in ("mingw32", "mingw64", "usr"):
            root = root.parent
        for relative in ("bin/bash.exe", "usr/bin/bash.exe"):
            bash = root / relative
            if bash.is_file():
                return str(bash)
    raise RuntimeError("Git Bash is required to exercise the Windows CI script")


class WorkflowShellTests(unittest.TestCase):
    def test_windows_uses_git_bash_even_when_wsl_is_first_on_path(self):
        with tempfile.TemporaryDirectory(prefix="neverd git shell ") as directory:
            root = Path(directory)
            bash = root / "bin" / "bash.exe"
            bash.parent.mkdir()
            bash.touch()
            for layout in ("bin", "cmd", "mingw32/bin", "mingw64/bin", "usr/bin"):
                with self.subTest(layout=layout), mock.patch.object(
                    sys, "platform", "win32"
                ), mock.patch.object(
                    shutil,
                    "which",
                    side_effect={
                        "git": str(root / layout / "git.exe"),
                        "bash": "C:/Windows/System32/bash.exe",
                    }.get,
                ) as which:
                    self.assertEqual(workflow_bash(), str(bash.resolve()))
                    which.assert_called_once_with("git")

    def test_windows_accepts_git_usr_bin_layout(self):
        with tempfile.TemporaryDirectory(prefix="neverd git shell ") as directory:
            root = Path(directory)
            bash = root / "usr" / "bin" / "bash.exe"
            bash.parent.mkdir(parents=True)
            bash.touch()
            with mock.patch.object(sys, "platform", "win32"), mock.patch.object(
                shutil, "which", return_value=str(root / "cmd" / "git.exe")
            ):
                self.assertEqual(workflow_bash(), str(bash.resolve()))

    def test_windows_missing_git_bash_fails_instead_of_using_wsl_or_skipping(self):
        with tempfile.TemporaryDirectory() as directory:
            for git in (None, str(Path(directory) / "cmd" / "git.exe")):
                with self.subTest(git=git), mock.patch.object(
                    sys, "platform", "win32"
                ), mock.patch.object(shutil, "which", return_value=git):
                    with self.assertRaisesRegex(RuntimeError, "Git Bash"):
                        workflow_bash()

    def test_unix_uses_path_bash(self):
        with mock.patch.object(sys, "platform", "linux"), mock.patch.object(
            shutil, "which", return_value="/bin/bash"
        ) as which:
            self.assertEqual(workflow_bash(), "/bin/bash")
            which.assert_called_once_with("bash")

    def test_unix_without_bash_retains_optional_tool_skip(self):
        with mock.patch.object(sys, "platform", "darwin"), mock.patch.object(
            shutil, "which", return_value=None
        ):
            with self.assertRaises(unittest.SkipTest):
                workflow_bash()


class JUnitParsingTests(unittest.TestCase):
    def test_pass_fail_skip_disabled_and_infrastructure_not_run_stay_distinct(self):
        records = tuple(
            record(name, "suite")
            for name in ("pass", "fail", "regex", "code", "disabled", "missing")
        )
        changes = {
            records[1]: ("fail", "Timeout", "timed out"),
            records[2]: (
                "notrun",
                "SKIP_REGULAR_EXPRESSION_MATCHED",
                "external tool unavailable",
            ),
            records[3]: ("notrun", "SKIP_RETURN_CODE=77", "unavailable"),
            records[4]: ("disabled", "", ""),
            records[5]: ("notrun", "Unable to find executable", "missing"),
        }
        outcomes = parse_junit(junit(records, changes))
        self.assertEqual(
            [item.outcome for item in outcomes],
            ["passed", "failed", "skipped", "skipped", "disabled", "not_run"],
        )
        self.assertEqual(outcomes[2].output, "external tool unavailable")

    def test_same_name_with_different_labels_is_preserved(self):
        records = (record("same", "aggregate"), record("same", "focused"))
        self.assertEqual(
            tuple(item.test for item in parse_junit(junit(records))), records
        )

    def test_duplicate_identity_is_rejected(self):
        test = record("same", "suite")
        with self.assertRaisesRegex(ResultError, "duplicate testcase identities"):
            parse_junit(junit((test, test)))

    def test_unknown_missing_and_contradictory_statuses_are_rejected(self):
        for status, element in (
            (None, None),
            ("success", None),
            ("run", "skipped"),
            ("notrun", None),
            ("disabled", "skipped"),
            ("run", "error"),
        ):
            with self.subTest(status=status, element=element):
                root = junit((record("a", "suite"),))
                case = root[0]
                if status is None:
                    del case.attrib["status"]
                else:
                    case.set("status", status)
                if element:
                    ET.SubElement(case, element, message="bad")
                with self.assertRaises(ResultError):
                    parse_junit(root)

    def test_missing_negative_and_forged_totals_are_rejected(self):
        for key in ("tests", "failures", "disabled", "skipped"):
            for value in (None, "-1", "false", "999"):
                with self.subTest(key=key, value=value):
                    root = junit((record("a", "suite"),))
                    if value is None:
                        del root.attrib[key]
                    else:
                        root.set(key, value)
                    with self.assertRaises(ResultError):
                        parse_junit(root)

    def test_wrong_root_hidden_cases_and_error_count_are_rejected(self):
        for root in (ET.Element("testsuites"), ET.Element("testsuite", errors="1")):
            with self.subTest(root=root.attrib):
                with self.assertRaises(ResultError):
                    parse_junit(root)
        root = junit((record("a", "suite"),))
        ET.SubElement(root, "testsuite")
        with self.assertRaises(ResultError):
            parse_junit(root)

    def test_empty_duplicate_or_ambiguous_labels_are_rejected(self):
        for labels in ("", "a;;b", "a;a", r"a\;b", "a[b;c]"):
            root = junit((record("a", "suite"),))
            root[0].find("./properties/property").set("value", labels)
            with self.subTest(labels=labels), self.assertRaises(ResultError):
                parse_junit(root)

    def test_non_ctest_skip_reason_is_not_treated_as_optional_skip(self):
        test = record("a", "optional")
        for reason in (
            "Fixture dependency failed",
            "SKIP_RETURN_CODE=999",
            "",
            "arbitrary reason",
        ):
            with self.subTest(reason=reason):
                outcomes = parse_junit(junit((test,), {test: ("notrun", reason, "")}))
                self.assertEqual(outcomes[0].outcome, "not_run")


class OutcomePolicyTests(unittest.TestCase):
    def audit(self, records, changes=None, profile="linux-semantic", status=0):
        return audit_results(
            records, parse_junit(junit(records, changes)), profile, status
        )

    def test_all_required_registrations_must_pass_on_every_profile(self):
        for profile in ("linux-semantic", "macos-patch", "windows-focused"):
            records = policy_records(profile)
            with self.subTest(profile=profile):
                report = self.audit(records, profile=profile)
                self.assertTrue(report["ok"], report["errors"])
                self.assertEqual(report["counts"]["passed"], len(records))

    def test_heavy_owners_and_integrity_suites_are_explicit_policy(self):
        self.assertIn("NeverDSemanticTests", required_labels("linux-semantic"))
        self.assertIn("NeverDPatchFullTests", required_labels("macos-patch"))
        for profile in ("linux-semantic", "macos-patch", "windows-focused"):
            for label in (
                "NeverDSupportThreadTests",
                "NeverDSessionCAPITests",
                "NeverDSessionLLVMTests",
                "NeverDSemanticFixtureTests",
                "NeverDPipelineOutcomeTests",
            ):
                with self.subTest(profile=profile, label=label):
                    self.assertIn(label, required_labels(profile))

    def test_a_skip_in_every_mandatory_label_is_missing_evidence(self):
        for profile in ("linux-semantic", "macos-patch", "windows-focused"):
            records = policy_records(profile)
            for test in records:
                with self.subTest(profile=profile, label=test.labels):
                    report = self.audit(
                        records,
                        {
                            test: (
                                "notrun",
                                "SKIP_REGULAR_EXPRESSION_MATCHED",
                                "clang unavailable",
                            )
                        },
                        profile,
                    )
                    self.assertFalse(report["ok"])
                    self.assertEqual(report["counts"]["required_skips"], 1)
                    self.assertEqual(report["counts"]["skipped"], 1)
                    self.assertEqual(report["counts"]["failed"], 0)

    def test_optional_skip_is_reported_but_never_counted_as_passed(self):
        optional = record("optional_solc", "NeverDEVMEmitterTests")
        records = policy_records() + (optional,)
        report = self.audit(
            records, {optional: ("notrun", "SKIP_RETURN_CODE=77", "solc unavailable")}
        )
        self.assertTrue(report["ok"], report["errors"])
        self.assertEqual(report["counts"]["skipped"], 1)
        self.assertEqual(report["counts"]["passed"], len(records) - 1)
        self.assertEqual(report["tests"][-1]["output"], "solc unavailable")
        self.assertIn(
            "child-runner coverage is not inferred", format_summary(report, "host")
        )

    def test_disabled_and_infrastructure_failure_are_never_optional(self):
        optional = record("optional", "optional")
        records = policy_records() + (optional,)
        for state, reason in (
            ("disabled", ""),
            ("notrun", "Unable to find executable"),
            ("fail", "Failed"),
        ):
            with self.subTest(state=state):
                report = self.audit(records, {optional: (state, reason, "")})
                self.assertFalse(report["ok"])

    def test_only_exact_non_apple_corpus_skip_identities_and_reasons_are_permitted(
        self,
    ):
        for test, reason in NON_APPLE_CORPUS_SKIPS.items():
            for profile in ("linux-semantic", "windows-focused", "macos-patch"):
                records = policy_records(profile) + (test,)
                with self.subTest(test=test, profile=profile):
                    report = self.audit(
                        records,
                        {test: ("notrun", "SKIP_REGULAR_EXPRESSION_MATCHED", reason)},
                        profile,
                    )
                    self.assertEqual(report["ok"], profile != "macos-patch")
            for changed_test, changed_reason in (
                (test, "unexpected unavailable dependency"),
                (record(test.name + "New", *test.labels), reason),
            ):
                report = self.audit(
                    policy_records() + (changed_test,),
                    {
                        changed_test: (
                            "notrun",
                            "SKIP_REGULAR_EXPRESSION_MATCHED",
                            changed_reason,
                        )
                    },
                )
                self.assertFalse(report["ok"])

    def test_deleting_an_entire_required_suite_cannot_pass_vacuously(self):
        records = policy_records()
        for index in range(len(records)):
            selected = records[:index] + records[index + 1 :]
            report = self.audit(selected)
            self.assertFalse(report["ok"])
            self.assertIn("required execution labels are absent", report["errors"][0])

    def test_nonzero_ctest_exit_rejects_an_apparently_all_passed_report(self):
        report = self.audit(policy_records(), status=8)
        self.assertFalse(report["ok"])
        self.assertIn("CTest exited with status 8", report["errors"])

    def test_missing_unexpected_and_changed_label_results_are_diagnosed(self):
        records = policy_records()
        extra = record(records[0].name, "wrong_owner")
        outcomes = parse_junit(junit(records[1:] + (extra,)))
        report = audit_results(records, outcomes, "linux-semantic", 0)
        self.assertFalse(report["ok"])
        self.assertEqual(report["counts"]["missing"], 1)
        self.assertEqual(report["counts"]["unexpected"], 1)
        self.assertEqual(report["tests"][0]["outcome"], "missing")

    def test_same_named_focused_and_heavy_outcomes_reconcile_independently(self):
        records = policy_records() + (
            record("same", "NeverDSemanticTests"),
            record("same", "Focused"),
        )
        report = self.audit(records)
        self.assertTrue(report["ok"], report["errors"])
        self.assertEqual(report["counts"]["passed"], len(records))

    def test_exit_status_file_and_tool_version_are_closed_contracts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "status"
            for value in ("", "false", "-1", "256", "0\n8", "0 8"):
                path.write_text(value)
                with self.subTest(value=value), self.assertRaises(ResultError):
                    read_exit_status(path)
            path.write_text("8\n")
            self.assertEqual(read_exit_status(path), 8)
            path.unlink()
            with self.assertRaises(OSError):
                read_exit_status(path)
        for version in ("ctest version 3.28.0", "ctest version 4.0.0"):
            check_ctest_version(version)
        for version in ("ctest version 3.27.9", "cmake version 3.31.4", ""):
            with self.assertRaises(ResultError):
                check_ctest_version(version)


@unittest.skipUnless(shutil.which("ctest"), "CTest is unavailable")
class RealCTestOutcomeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        version = subprocess.check_output(["ctest", "--version"], text=True)
        try:
            check_ctest_version(version)
        except ResultError as error:
            raise unittest.SkipTest(str(error))

    def run_fixture(self, tests, extra_args=()):
        temporary = tempfile.TemporaryDirectory(prefix="neverd-ctest-outcome-test-")
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        lines = []
        for name, code, properties in tests:
            command = (
                [sys.executable, "-c", code]
                if code is not None
                else [str(root / "missing-executable")]
            )
            lines.append(
                "add_test("
                + " ".join(f"[=[{argument}]=]" for argument in (name, *command))
                + ")"
            )
            lines.append(
                f"set_tests_properties([=[{name}]=] PROPERTIES LABELS fixture {properties})"
            )
        (root / "CTestTestfile.cmake").write_text("\n".join(lines), encoding="utf-8")
        discovery = subprocess.run(
            ["ctest", "--test-dir", str(root), "--show-only=json-v1"],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
        selected = parse_inventory(json.loads(discovery.stdout))
        report_path = root / "results.xml"
        run = subprocess.run(
            [
                "ctest",
                "--test-dir",
                str(root),
                "--parallel",
                "1",
                "--output-junit",
                str(report_path),
                *extra_args,
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        outcomes = parse_junit(ET.parse(report_path).getroot())
        return selected, outcomes, run.returncode

    def test_actual_ctest_pass_skip_disabled_failure_timeout_and_missing_executable(
        self,
    ):
        selected, outcomes, status = self.run_fixture(
            [
                ("pass", "print('ok')", ""),
                (
                    "regex",
                    "print('[  SKIPPED ] missing external tool')",
                    "SKIP_REGULAR_EXPRESSION SKIPPED",
                ),
                ("code", "import sys;sys.exit(77)", "SKIP_RETURN_CODE 77"),
                ("disabled", "print('must not run')", "DISABLED TRUE"),
                ("fail", "import sys;sys.exit(1)", ""),
                ("timeout", "import time;time.sleep(2)", "TIMEOUT 0.1"),
                ("missing", None, ""),
            ]
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(set(selected), {outcome.test for outcome in outcomes})
        self.assertEqual(
            {outcome.test.name: outcome.outcome for outcome in outcomes},
            {
                "pass": "passed",
                "regex": "skipped",
                "code": "skipped",
                "disabled": "disabled",
                "fail": "failed",
                "timeout": "failed",
                "missing": "not_run",
            },
        )

    def test_actual_all_skipped_report_is_not_execution_success(self):
        selected, outcomes, status = self.run_fixture(
            [
                (
                    "only",
                    "print('[  SKIPPED ] unavailable')",
                    "SKIP_REGULAR_EXPRESSION SKIPPED",
                )
            ]
        )
        self.assertEqual(status, 0)
        self.assertEqual(outcomes[0].outcome, "skipped")
        report = audit_results(selected, outcomes, "linux-semantic", status)
        self.assertFalse(report["ok"])
        self.assertEqual(report["counts"]["passed"], 0)
        self.assertEqual(report["counts"]["skipped"], 1)

    def test_actual_stop_on_failure_omits_unstarted_cases(self):
        selected, outcomes, status = self.run_fixture(
            [
                ("fails_first", "import sys;sys.exit(1)", ""),
                ("never_started", "print('later')", ""),
            ],
            ("--stop-on-failure",),
        )
        self.assertNotEqual(status, 0)
        self.assertEqual(len(outcomes), 1)
        report = audit_results(selected, outcomes, "linux-semantic", status)
        self.assertEqual(report["counts"]["missing"], 1)
        self.assertFalse(report["ok"])

    def test_workflow_run_script_preserves_failure_status_and_removes_stale_evidence(
        self,
    ):
        bash = workflow_bash()
        repository = Path(__file__).resolve().parents[2]
        workflow = (repository / ".github/workflows/ci.yml").read_text()
        step = workflow.split("      - name: Run selected test profile\n", 1)[1].split(
            "\n      - name:", 1
        )[0]
        body = step.split("        run: |\n", 1)[1].split("        env:\n", 1)[0]
        body = "\n".join(line[10:] for line in body.splitlines())
        with tempfile.TemporaryDirectory(prefix="neverd-ctest-workflow-") as directory:
            root = Path(directory)
            build = root / "build-ci"
            build.mkdir()
            (build / "CTestTestfile.cmake").write_text(
                f'add_test(failure [=[{sys.executable}]=] -c "import sys;sys.exit(1)")\n'
                f'add_test(unstarted [=[{sys.executable}]=] -c "print(42)")\n'
            )
            for name in (
                "ctest-results.xml",
                "ctest-exit-status.txt",
                "ctest-outcomes.json",
            ):
                (build / name).write_text("stale success")
            run = subprocess.run(
                [bash, "-e", "-o", "pipefail", "-c", body],
                cwd=root,
                env=dict(os.environ, EXCLUDE_LABELS="^excluded$", TEST_PARALLEL="1"),
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(run.returncode, 8, run.stdout + run.stderr)
            self.assertEqual(read_exit_status(build / "ctest-exit-status.txt"), 8)
            outcomes = parse_junit(ET.parse(build / "ctest-results.xml").getroot())
            self.assertEqual(len(outcomes), 1)
            self.assertEqual(outcomes[0].outcome, "failed")
            self.assertFalse((build / "ctest-outcomes.json").exists())
            self.assertTrue((root / "ctest.log").is_file())


class OutcomeCommandTests(unittest.TestCase):
    def test_complete_file_inventory_and_failed_evidence_write_truthful_artifacts(self):
        # Exercise the real CLI and its production discovery floors, rather
        # than weakening the audit with a test-only minimum override.
        repository = Path(__file__).resolve().parents[2]
        semantic = tuple(
            record(f"semantic/{index}", "NeverDSemanticTests")
            for index in range(20_000)
        )
        patch = tuple(
            record(f"patch/{index}", "NeverDPatchFullTests") for index in range(22_000)
        )
        selected = semantic + policy_records()
        complete = selected + patch
        discovery = {
            "kind": "ctestInfo",
            "version": {"major": 1, "minor": 0},
            "tests": [
                {
                    "name": test.name,
                    "properties": [{"name": "LABELS", "value": sorted(test.labels)}],
                }
                for test in complete
            ],
        }
        with tempfile.TemporaryDirectory(prefix="neverd-ctest-command-") as directory:
            root = Path(directory)
            inventory_path, junit_path = root / "inventory.json", root / "results.xml"
            status_path, report_path = root / "status.txt", root / "outcomes.json"
            summary_path = root / "summary.md"
            inventory_path.write_text(json.dumps(discovery), encoding="utf-8")
            xml = ET.tostring(junit(selected), encoding="unicode")
            command = [
                sys.executable,
                str(repository / "scripts/audit_ci_test_results.py"),
                "audit",
                "--inventory",
                str(inventory_path),
                "--junit",
                str(junit_path),
                "--ctest-status",
                str(status_path),
                "--output",
                str(report_path),
                "--profile",
                "linux-semantic",
                "--exclude-label-regex",
                "^NeverDPatchFullTests$",
                "--matrix-name",
                "test host",
            ]
            for scenario in (
                "complete",
                "missing-status",
                "missing-junit",
                "malformed-junit",
                "nonzero-status",
            ):
                status_path.write_text("8\n" if scenario == "nonzero-status" else "0\n")
                junit_path.write_text(xml, encoding="utf-8")
                summary_path.write_text("")
                if scenario == "missing-status":
                    status_path.unlink()
                elif scenario == "missing-junit":
                    junit_path.unlink()
                elif scenario == "malformed-junit":
                    junit_path.write_text("<testsuite")
                with self.subTest(scenario=scenario):
                    run = subprocess.run(
                        command,
                        env=dict(os.environ, GITHUB_STEP_SUMMARY=str(summary_path)),
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    self.assertEqual(
                        run.returncode,
                        0 if scenario == "complete" else 1,
                        run.stdout + run.stderr,
                    )
                    report = json.loads(report_path.read_text())
                    self.assertEqual(report["ok"], scenario == "complete")
                    summary = summary_path.read_text()
                    self.assertIn("CTest registration", summary)
                    if scenario == "complete":
                        self.assertEqual(report["counts"]["passed"], len(selected))
                        self.assertEqual(report["counts"]["skipped"], 0)
                    else:
                        self.assertTrue(report["errors"])
                        self.assertIn("Audit failed", summary)


if __name__ == "__main__":
    unittest.main()
