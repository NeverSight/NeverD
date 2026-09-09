"""Configuration mutations for the complete Actions matrix; no tools run."""
from __future__ import annotations

from collections import Counter
from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import hashlib
import io
import json
from pathlib import Path
import re
import tempfile
import unittest

from scripts import mobile_real_apps_matrix as matrix


MANIFEST = Path(__file__).resolve().parents[1] / "mobile_real_apps.json"
WORKFLOW = MANIFEST.parent.parent / ".github/workflows/mobile-real-apps.yml"


class MobileRealAppsMatrixTests(unittest.TestCase):
    def manifest(self):
        return json.loads(MANIFEST.read_text(encoding="utf-8"))

    def rows(self, generated):
        return [*generated["android_matrix"]["include"], *generated["ios_matrix"]["include"]]

    def test_full_scope_preserves_eighteen_baselines_and_adds_four_swift_comparisons(self):
        manifest = self.manifest()
        generated = matrix.generate(manifest)
        expected_android = {f"{app}-{profile}" for app in ("markor", "calculator")
                            for profile in ("official-release", "gradle-release", "gradle-debug")}
        expected_ios = {f"{app}-{profile}-{target}" for app in ("icecubes", "wikipedia")
                        for profile in ("release", "size")
                        for target in ("arm64-simulator", "arm64-device", "x86_64-simulator")}
        expected_ios |= {f"icecubes-{profile}-arm64-{target}-swift64"
                         for profile in ("release", "size") for target in ("simulator", "device")}
        android = generated["android_matrix"]["include"]
        ios = generated["ios_matrix"]["include"]
        self.assertEqual(Counter(row["case_id"] for row in android), Counter(expected_android))
        self.assertEqual(Counter(row["case_id"] for row in ios), Counter(expected_ios))
        self.assertEqual(generated["required_case_count"], 22)
        self.assertEqual(generated["baseline_case_count"], 6)
        self.assertEqual(set(generated["required_case_ids"]), {case["id"] for case in manifest["required_cases"]})
        self.assertEqual({row["java_version"] for row in android}, {"21"})
        expected_packages = {"build-tools;35.0.0", "build-tools;36.0.0", "platforms;android-35", "platforms;android-36"}
        self.assertTrue(all(set(row["sdk_packages"]) == expected_packages for row in android))

    def test_every_row_preserves_the_exact_manifest_case_and_source_identity(self):
        manifest = self.manifest()
        cases = {case["id"]: case for case in manifest["required_cases"]}
        for row in self.rows(matrix.generate(manifest)):
            with self.subTest(case=row["case_id"]):
                case = cases[row["case_id"]]
                app = manifest["apps"][case["app"]]
                self.assertEqual(row["variant"], case)
                self.assertEqual(row["source_commit"], app["source_commit"])
                self.assertEqual(row["repository"], app["repository"])
                for field in ("app", "platform", "profile", "architecture", "sdk"):
                    self.assertEqual(row[field], case[field])

    def test_baseline_changes_never_filter_the_actual_execution_matrix(self):
        manifest = self.manifest()
        original = matrix.generate(manifest)
        for baseline in ([], [manifest["required_cases"][0]["id"]], list(reversed(manifest["baseline_cases"]))):
            with self.subTest(baseline=baseline):
                changed = deepcopy(manifest)
                changed["baseline_cases"] = baseline
                actual = matrix.generate(changed)
                self.assertEqual(actual["android_matrix"], original["android_matrix"])
                self.assertEqual(actual["ios_matrix"], original["ios_matrix"])
                self.assertEqual(actual["required_case_count"], 22)

    def test_incomplete_implementation_or_qualification_does_not_hide_cases(self):
        manifest = self.manifest()
        for case in manifest["required_cases"]:
            case["implementation_status"] = "incomplete"
        manifest["apps"]["icecubes"]["status"] = "unsupported"
        generated = matrix.generate(manifest)
        self.assertEqual(len(self.rows(generated)), 22)
        self.assertTrue(all(row["variant"]["implementation_status"] == "incomplete"
                            for row in self.rows(generated)))

    def test_sdk_packages_include_source_build_and_inventory_requirements(self):
        manifest = self.manifest()
        manifest["apps"]["markor"]["android"]["source_build"]["build_tools"] = "34.0.0"
        for row in matrix.generate(manifest)["android_matrix"]["include"]:
            self.assertEqual(set(row["sdk_packages"]), {
                "build-tools;34.0.0", "build-tools;35.0.0", "build-tools;36.0.0",
                "platforms;android-35", "platforms;android-36"})

    def test_manifest_java_source_commit_and_xcode_changes_propagate_without_workflow_edits(self):
        manifest = self.manifest()
        manifest["apps"]["markor"]["source_commit"] = "a" * 40
        config = manifest["apps"]["markor"]["android"]
        config["java"] = "25"
        config["source_build"]["jdk_major"] = 25
        manifest["apps"]["wikipedia"]["ios"]["xcode"] = "26.4"
        for row in self.rows(matrix.generate(manifest)):
            if row["app"] == "markor":
                self.assertEqual(row["source_commit"], "a" * 40)
                self.assertEqual(row["java_version"], "25")
            elif row["app"] == "wikipedia":
                self.assertEqual(row["developer_dir"], "/Applications/Xcode_26.4.app/Contents/Developer")

    def test_comparisons_keep_baseline_source_optimization_and_target_with_separate_budget(self):
        manifest = self.manifest()
        rows = {row["case_id"]: row for row in self.rows(matrix.generate(manifest))}
        comparisons = [case for case in manifest["required_cases"] if "comparison_of" in case]
        self.assertEqual(len(comparisons), 4)
        self.assertEqual(sum("comparison_of" not in case for case in manifest["required_cases"]), 18)
        for case in comparisons:
            row, baseline = rows[case["id"]], rows[case["comparison_of"]]
            with self.subTest(case=case["id"]):
                self.assertEqual(row["toolchain"], matrix.BUNDLE_IDENTIFIER)
                self.assertEqual(baseline["toolchain"], "XcodeDefault")
                self.assertTrue(row["install_toolchain"])
                self.assertFalse(baseline["install_toolchain"])
                self.assertEqual((row["job_timeout_minutes"], baseline["job_timeout_minutes"]), (135, 80))
                for field in ("app", "source_commit", "repository", "profile", "architecture", "sdk"):
                    self.assertEqual(row[field], baseline[field])

    def test_missing_floating_or_mismatched_toolchain_comparisons_are_not_scheduled(self):
        for mutation in ("toolchain-missing", "toolchain-floating", "baseline-missing", "self",
                         "baseline-toolchain", "app", "profile", "sdk", "architecture", "xcode"):
            with self.subTest(mutation=mutation):
                manifest = self.manifest()
                case = next(case for case in manifest["required_cases"] if "comparison_of" in case)
                baseline = next(row for row in manifest["required_cases"] if row["id"] == case["comparison_of"])
                if mutation == "toolchain-missing":
                    case.pop("toolchain")
                elif mutation == "toolchain-floating":
                    case["toolchain"] = "swift-latest"
                elif mutation == "baseline-missing":
                    case["comparison_of"] = "missing-baseline"
                elif mutation == "self":
                    case["comparison_of"] = case["id"]
                elif mutation == "baseline-toolchain":
                    baseline["toolchain"] = matrix.BUNDLE_IDENTIFIER
                elif mutation == "app":
                    case["app"] = "wikipedia"
                elif mutation == "profile":
                    case["profile"] = "size" if case["profile"] == "release" else "release"
                elif mutation == "sdk":
                    case["sdk"] = "iphoneos" if case["sdk"] == "iphonesimulator" else "iphonesimulator"
                elif mutation == "architecture":
                    case["architecture"] = "x86_64"
                else:
                    manifest["apps"]["icecubes"]["ios"]["xcode"] = "26.4"
                with self.assertRaises(matrix.MatrixError):
                    matrix.generate(manifest)

    def test_default_baseline_cannot_be_relabelled_as_a_comparison(self):
        manifest = self.manifest()
        baseline = next(case for case in manifest["required_cases"] if case.get("toolchain") == "XcodeDefault")
        baseline["comparison_of"] = "icecubes-release-arm64-simulator"
        with self.assertRaisesRegex(matrix.MatrixError, "baseline"):
            matrix.generate(manifest)

    def test_gradle_tasks_are_derived_from_the_same_manifest_as_the_case_profile(self):
        manifest = self.manifest()
        rows = matrix.generate(manifest)["android_matrix"]["include"]
        for row in rows:
            config = manifest["apps"][row["app"]]["android"]
            if row["profile"] == "official-release":
                self.assertNotIn("gradle_task", row)
            else:
                key = "release_task" if row["profile"] == "gradle-release" else "debug_task"
                self.assertEqual(row["gradle_task"], config[key])

    def test_duplicate_unknown_and_empty_required_cases_fail_instead_of_disappearing(self):
        for mutation in ("empty", "duplicate", "unknown-app", "unknown-platform", "unknown-profile", "missing-sdk"):
            with self.subTest(mutation=mutation):
                manifest = self.manifest()
                if mutation == "empty":
                    manifest["required_cases"] = []
                elif mutation == "duplicate":
                    manifest["required_cases"].append(deepcopy(manifest["required_cases"][0]))
                else:
                    case = manifest["required_cases"][0]
                    if mutation == "missing-sdk":
                        case.pop("sdk")
                    else:
                        field = {"unknown-app": "app", "unknown-platform": "platform", "unknown-profile": "profile"}[mutation]
                        case[field] = "not-declared"
                with self.assertRaises(matrix.MatrixError):
                    matrix.generate(manifest)

    def test_baseline_must_not_invent_cases_or_duplicate_ids(self):
        for baseline in (["not-declared"], ["markor-official-release", "markor-official-release"]):
            with self.subTest(baseline=baseline):
                manifest = self.manifest()
                manifest["baseline_cases"] = baseline
                with self.assertRaisesRegex(matrix.MatrixError, "baseline_cases"):
                    matrix.generate(manifest)

    def test_empty_platform_or_unused_app_cannot_silently_drop_an_obligation(self):
        manifest = self.manifest()
        manifest["required_cases"] = [case for case in manifest["required_cases"] if case["platform"] == "android"]
        manifest["baseline_cases"] = ["markor-official-release", "calculator-official-release"]
        with self.assertRaisesRegex(matrix.MatrixError, "app has no required case"):
            matrix.generate(manifest)
        del manifest["apps"]["icecubes"], manifest["apps"]["wikipedia"]
        with self.assertRaisesRegex(matrix.MatrixError, "each platform requires"):
            matrix.generate(manifest)

    def test_floating_or_injected_source_repository_case_and_tool_versions_fail(self):
        for mutation in ("commit", "repository", "case-id", "java", "inventory-tools", "source-tools", "xcode"):
            with self.subTest(mutation=mutation):
                manifest = self.manifest()
                app = manifest["apps"]["markor"]
                if mutation == "commit":
                    app["source_commit"] = "main"
                elif mutation == "repository":
                    app["repository"] = "owner/repo\nother=value"
                elif mutation == "case-id":
                    manifest["required_cases"][0]["id"] = "case\nother=value"
                elif mutation == "java":
                    app["android"]["java"] = "21; echo injected"
                elif mutation == "inventory-tools":
                    app["android"]["build_tools"] = "latest"
                elif mutation == "source-tools":
                    app["android"]["source_build"]["build_tools"] = "latest"
                else:
                    manifest["apps"]["icecubes"]["ios"]["xcode"] = "26.5/../../other"
                with self.assertRaises(matrix.MatrixError):
                    matrix.generate(manifest)

    def test_source_jdk_sdk_and_gradle_profile_disagreements_are_errors(self):
        for mutation in ("jdk", "sdk", "task", "profile", "source-build"):
            with self.subTest(mutation=mutation):
                manifest = self.manifest()
                config = manifest["apps"]["markor"]["android"]
                if mutation == "jdk":
                    config["source_build"]["jdk_major"] = 17
                elif mutation == "sdk":
                    manifest["required_cases"][0]["sdk"] = "36"
                elif mutation == "task":
                    config["release_task"] = ":app:assembleUnrelated"
                elif mutation == "profile":
                    config["source_build"]["profiles"].pop("gradle-debug")
                else:
                    config.pop("source_build")
                with self.assertRaises(matrix.MatrixError):
                    matrix.generate(manifest)

    def test_device_x86_64_is_rejected_but_simulator_x86_64_is_scheduled(self):
        manifest = self.manifest()
        generated = matrix.generate(manifest)
        self.assertEqual(sum(row["architecture"] == "x86_64" for row in generated["ios_matrix"]["include"]), 4)
        case = next(case for case in manifest["required_cases"] if case["platform"] == "ios" and case["sdk"] == "iphoneos")
        case["architecture"] = "x86_64"
        with self.assertRaisesRegex(matrix.MatrixError, "SDK/architecture"):
            matrix.generate(manifest)

    def test_github_matrix_limit_fails_instead_of_truncating(self):
        manifest = self.manifest()
        for index in range(257):
            case = deepcopy(manifest["required_cases"][0])
            case["id"] = f"additional-case-{index}"
            manifest["required_cases"].append(case)
        with self.assertRaisesRegex(matrix.MatrixError, "do not silently truncate"):
            matrix.generate(manifest)

    def test_cli_outputs_the_exact_two_matrices_and_input_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / "manifest.json"
            manifest.write_bytes(MANIFEST.read_bytes())
            output = Path(temporary) / "github-output"
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                result = matrix.main(["--manifest", str(manifest), "--github-output", str(output)])
            self.assertEqual(result, 0)
            values = {key: json.loads(value) for key, value in (line.split("=", 1) for line in output.read_text().splitlines())}
            self.assertEqual(set(values), {"android_matrix", "ios_matrix"})
            report = json.loads(stdout.getvalue())
            self.assertEqual(report["manifest_sha256"], hashlib.sha256(manifest.read_bytes()).hexdigest())
            for key in values:
                self.assertEqual(values[key], report[key])

    def test_invalid_json_does_not_publish_partial_or_stale_matrix_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / "manifest.json"
            manifest.write_text('{"schema_version":1,"schema_version":1}')
            output = Path(temporary) / "github-output"
            with redirect_stderr(io.StringIO()):
                result = matrix.main(["--manifest", str(manifest), "--github-output", str(output)])
            self.assertEqual(result, 1)
            self.assertFalse(output.exists())

    def test_workflow_consumes_full_outputs_and_does_not_require_sibling_build_success(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        for platform in ("android", "ios"):
            job = re.search(rf"(?ms)^  {platform}:\n(.*?)(?=^  \S|\Z)", workflow)
            self.assertIsNotNone(job, f"missing {platform} job")
            body = job.group(1)
            self.assertIn(f"matrix: ${{{{ fromJSON(needs.guards.outputs.{platform}_matrix) }}}}", body)
            self.assertIn("needs: [trigger, guards, build]", body)
            self.assertIn("needs.trigger.result == 'success'", body)
            self.assertIn(f"needs.guards.outputs.{platform}_matrix != ''", body)
            self.assertNotIn("needs.build.result == 'success'", body)
            self.assertIn("ref: ${{ matrix.source_commit }}", body)
            self.assertIn("name: real-app-evidence-${{ matrix.case_id }}", body)
            self.assertNotIn("-official-release\n", body)
        self.assertIn("python scripts/check_docs_i18n.py", workflow)

    def test_comparison_installation_is_fresh_separate_and_does_not_consume_the_app_budget(self):
        workflow = WORKFLOW.read_text(encoding="utf-8")
        ios = re.search(r"(?ms)^  ios:\n(.*?)(?=^  \S|\Z)", workflow).group(1)
        self.assertIn("timeout-minutes: ${{ matrix.job_timeout_minutes }}", ios)
        self.assertIn("if: ${{ matrix.install_toolchain }}", ios)
        self.assertIn("timeout-minutes: 50", ios)
        self.assertIn('--consumer-commit "$CONSUMER_COMMIT" --case-id "$CASE_ID"', ios)
        self.assertIn('"$RUNNER_TEMP/real-app-toolchains/$CASE_ID/evidence/qualification.json"', ios)
        self.assertIn('"${receipt_args[@]}"', ios)
        self.assertIn('--work-dir "$RUNNER_TEMP/real-app-evidence/$CASE_ID" --timeout 4200', ios)
        self.assertIn("name: real-app-toolchain-${{ matrix.case_id }}", ios)
        self.assertIn("always() && matrix.install_toolchain", ios)
        self.assertIn("scripts.tests.test_mobile_swift_toolchain", workflow)
        self.assertNotIn("continue-on-error", ios)


if __name__ == "__main__":
    unittest.main()
