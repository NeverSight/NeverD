"""Mutations of the release evidence protocol; no application/tool execution."""
from __future__ import annotations

from contextlib import redirect_stdout
from copy import deepcopy
import hashlib
import io
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from scripts import check_mobile_release_gate as gate


CONSUMER = "a" * 40
SOURCE = "b" * 40
ORIGINAL = b"independent original implementation artifact"


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


class Fixture:
    """A complete one-case protocol fixture, not evidence about a real app."""
    def __init__(self, root, *, count=1, present=None, qualification=None, dependency_locks=None):
        self.root = root
        self.results = root / "results"
        self.results.mkdir()
        self.manifest_path = root / "manifest.json"
        self.needs_path = root / "needs.json"
        self.cases = [{"id": f"case-{index:02}", "app": "owned-fixture", "platform": "ios",
                       "profile": "release", "architecture": "arm64", "sdk": "iphonesimulator"}
                      for index in range(count)]
        self.manifest = {
            "schema_version": 1,
            "apps": {"owned-fixture": {"source_commit": SOURCE, "repository": "example/owned-fixture",
                                       "ios": {"behavior_obligations": ["sum"]},
                                       "acceptance_dependencies": dependency_locks or []}},
            "required_cases": self.cases,
            "baseline_cases": [row["id"] for row in self.cases[:6]],
            "required_jobs": ["guards", "build", "android", "ios"],
            # Production keeps its two incomplete requirements. Only this
            # independent protocol fixture has no additional qualification scope.
            "qualification_requirements": qualification or {},
        }
        write_json(self.manifest_path, self.manifest)
        self.manifest_sha = hashlib.sha256(self.manifest_path.read_bytes()).hexdigest()
        write_json(self.needs_path, {name: {"result": "success"} for name in self.manifest["required_jobs"]})
        for case in self.cases[:count if present is None else present]:
            self.case(case)

    def identity(self, case, kind):
        return {"schema_version": 1, "kind": kind, "consumer_commit": CONSUMER,
                "source_commit": SOURCE, "manifest_sha256": self.manifest_sha, "case_id": case["id"]}

    def case(self, case):
        root = self.results / case["id"]
        root.mkdir()
        original_work = f"/ci/work/{case['id']}"
        source = "recovered/sources/recovered.c"
        source_path = root / source
        source_path.parent.mkdir(parents=True)
        source_path.write_text("int fixture_answer(void) { return 42; }\n")
        (root / "original").write_bytes(ORIGINAL)
        (root / "rebuilt").write_bytes(b"independently rebuilt implementation artifact")
        (root / "logs").mkdir()
        assertions = {"schema_version": 1, "assertions": [
            {"id": "sum:normal", "obligation": "sum", "status": "pass", "value": 42}]}
        write_json(root / "expected.json", assertions)
        for label in ("original", "rebuilt"):
            write_json(root / f"logs/{label}.stdout", assertions)
            (root / f"logs/{label}.stderr").write_text("")
        (root / "logs/compile.stdout").write_text("")
        (root / "logs/compile.stderr").write_text("")
        write_json(root / "input-inventory.json", {"schema_version": 1, "identity": "fixture_answer", "unknown_count": 0})
        write_json(root / "recovered/report.json", {
            "status": "success", "platform": "ios", "architecture": "arm64",
            "outputs": {"native_source": "sources/recovered.c"}})
        write_json(root / "dependencies.json", {
            "schema_version": 1, "original_implementation_inputs": [], "original_artifacts": ["original"],
            "inputs": [{"path": source, "origin": "recovered-source"}]})
        write_json(root / "compile-proof.json", {
            **self.identity(case, "independent-recompile"),
            "input_sources": [source], "source_reports": ["recovered/report.json"],
            "outputs": ["rebuilt"], "command_ids": ["compile"],
            "original_implementation_inputs": [], "dependency_manifest": "dependencies.json"})
        write_json(root / "behavior-proof.json", {
            **self.identity(case, "behavior-comparison"),
            "original_artifact": "original", "rebuilt_artifact": "rebuilt",
            "original_command_ids": ["original"], "rebuilt_command_ids": ["rebuilt"],
            "expected_assertions": "expected.json", "original_results": "logs/original.stdout",
            "rebuilt_results": "logs/rebuilt.stdout",
            "runtime": {"kind": "ios-simulator", "sdk": "iphonesimulator", "architecture": "arm64"}})
        stages = {name: {"status": "success", "details": {"evidence": ["input-inventory.json"]}} for name in gate.STAGES}
        stages["original_build"]["details"]["evidence"] = ["original"]
        stages["recovery"]["details"]["evidence"] = ["recovered/report.json"]
        for name, proof in (("recompile", "compile-proof.json"), ("behavior", "behavior-proof.json")):
            stages[name]["details"] = {"evidence": [proof], "proof": proof}
        commands = [
            {"id": "compile", "argv": ["/usr/bin/clang", f"{original_work}/{source}", "-o", f"{original_work}/rebuilt"]},
            {"id": "original", "argv": ["/usr/bin/xcrun", "simctl", "spawn", "booted", f"{original_work}/original"]},
            {"id": "rebuilt", "argv": ["/usr/bin/xcrun", "simctl", "spawn", "booted", f"{original_work}/rebuilt"]},
        ]
        for command in commands:
            command.update({"status": "success", "exitcode": 0, "cwd": original_work,
                            "stdout": f"logs/{command['id']}.stdout", "stderr": f"logs/{command['id']}.stderr"})
        for name, requirement in self.manifest["qualification_requirements"].items():
            for ref in requirement.get("evidence", []):
                if ref["case_id"] == case["id"]:
                    write_json(root / ref["path"], {**self.identity(case, "qualification"), "status": "success",
                               "requirement": name, "case_ids": [row["id"] for row in self.cases]})
        record = {"schema_version": 1, "case_id": case["id"], "app": case["app"], "variant": deepcopy(case),
                  "consumer_commit": CONSUMER, "source_commit": SOURCE, "manifest_sha256": self.manifest_sha,
                  "status": "success", "stages": stages, "failures": [], "commands": commands,
                  "work_directory": original_work, "artifacts": []}
        write_json(root / "result.json", record)
        self.refresh(case["id"])

    def path(self, name="result.json", case="case-00"):
        return self.results / case / name

    def document(self, name="result.json", case="case-00"):
        return json.loads(self.path(name, case).read_text())

    def mutate(self, name, change, *, case="case-00", refresh=True):
        value = self.document(name, case)
        change(value)
        write_json(self.path(name, case), value)
        if refresh and name != "result.json":
            self.refresh(case)

    def refresh(self, case="case-00"):
        record = self.document(case=case)
        record["artifacts"] = [{"path": path.relative_to(self.results / case).as_posix(),
                                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "size": path.stat().st_size}
                               for path in sorted((self.results / case).rglob("*"))
                               if path.is_file() and path != self.path(case=case)]
        write_json(self.path(case=case), record)

    def audit(self):
        return gate.audit(self.manifest_path, self.results, CONSUMER, self.needs_path)


class MobileReleaseGateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def assert_rejected(self, fixture, reason=None):
        report = fixture.audit()
        self.assertFalse(report["qualified"], report)
        self.assertEqual(report["status"], "failed")
        if reason:
            self.assertIn(reason, json.dumps(report))
        return report

    def test_complete_independent_fixture_passes_after_artifact_relocation(self):
        fixture = Fixture(self.root)
        report = fixture.audit()
        self.assertTrue(report["qualified"], report)
        self.assertEqual(report["scope"], "full-release")
        self.assertEqual(report["qualified_cases"], ["case-00"])

    def test_six_green_baselines_do_not_replace_eighteen_required_cases(self):
        fixture = Fixture(self.root, count=18, present=6)
        report = self.assert_rejected(fixture, "full release matrix is missing cases")
        self.assertEqual(len(report["baseline_qualified_cases"]), 6)
        self.assertEqual(len(report["required_cases"]), 18)
        self.assertEqual(len(report["missing_cases"]), 12)

    def test_empty_result_directory_is_not_a_vacuous_pass(self):
        fixture = Fixture(self.root, present=0)
        self.assert_rejected(fixture, "missing cases")

    def test_duplicate_result_does_not_get_first_wins(self):
        fixture = Fixture(self.root)
        shutil.copytree(fixture.results / "case-00", fixture.results / "duplicate")
        self.assert_rejected(fixture, "duplicate case result")

    def test_extra_result_is_not_ignored(self):
        fixture = Fixture(self.root)
        write_json(fixture.results / "extra/result.json", {"case_id": "not-declared"})
        self.assert_rejected(fixture, "unexpected or missing case_id")

    def test_consumer_source_manifest_and_exact_variant_cannot_drift(self):
        for field, value in (("consumer_commit", "c" * 40), ("source_commit", "d" * 40),
                             ("manifest_sha256", "e" * 64), ("app", "another-app"),
                             ("variant", {"id": "case-00"})):
            with self.subTest(field=field), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                fixture.mutate("result.json", lambda row: row.update({field: value}))
                self.assert_rejected(fixture, f"case {field} differs")

    def test_required_github_jobs_cannot_be_omitted_skipped_or_cancelled(self):
        for mutation in ("omit", "empty", "failure", "skipped", "cancelled", "missing-result"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                needs = json.loads(fixture.needs_path.read_text())
                if mutation == "omit":
                    needs.pop("ios")
                elif mutation == "empty":
                    needs.clear()
                else:
                    needs["ios"] = {} if mutation == "missing-result" else {"result": mutation}
                write_json(fixture.needs_path, needs)
                self.assert_rejected(fixture, "GitHub")

    def test_incomplete_qualifications_keep_an_otherwise_green_case_red(self):
        for name in ("independent_holdouts", "toolchain_variation"):
            with self.subTest(requirement=name), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary), qualification={name: {"status": "incomplete"}})
                self.assert_rejected(fixture, f"qualification {name} remains incomplete")

    def test_qualification_success_alone_or_an_unrelated_log_cannot_pass(self):
        for requirement in ({"status": "success"}, {"status": "success", "evidence": [
                {"case_id": "case-00", "path": "qualification.json"}]}):
            with self.subTest(requirement=requirement), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary), qualification={"independent_holdouts": requirement})
                if requirement.get("evidence"):
                    write_json(fixture.path("qualification.json"), {"message": "all tests passed"})
                    fixture.refresh()
                self.assert_rejected(fixture)

    def test_development_case_cannot_self_certify_as_an_independent_holdout(self):
        fixture = Fixture(self.root, qualification={"independent_holdouts": {"status": "success", "evidence": [
            {"case_id": "case-00", "path": "qualification.json"}]}})
        self.assert_rejected(fixture, "independent holdout matrix")

    def test_one_toolchain_cannot_self_certify_as_toolchain_variation(self):
        fixture = Fixture(self.root, qualification={"toolchain_variation": {"status": "success", "evidence": [
            {"case_id": "case-00", "path": "qualification.json"}]}})
        self.assert_rejected(fixture, "pinned variant identities")

    def test_status_only_success_cannot_replace_unimplemented_rebuild_or_behavior(self):
        for stage in ("recompile", "behavior"):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                fixture.mutate("result.json", lambda row: row["stages"][stage]["details"].pop("proof"))
                self.assert_rejected(fixture, f"{stage} proof is absent")

    def test_each_required_stage_and_nested_partial_state_is_enforced(self):
        for stage in gate.STAGES:
            for mutation in ("missing", "incomplete", "nested-partial"):
                with self.subTest(stage=stage, mutation=mutation), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                    fixture = Fixture(Path(temporary))
                    def change(row):
                        if mutation == "missing":
                            row["stages"].pop(stage)
                        elif mutation == "incomplete":
                            row["stages"][stage]["status"] = "incomplete"
                        else:
                            row["stages"][stage]["details"]["inventory"] = {"status": "partial"}
                    fixture.mutate("result.json", change)
                    self.assert_rejected(fixture)

    def test_commands_must_exist_execute_successfully_and_reference_hashed_logs(self):
        for mutation in ("empty", "duplicate", "timeout", "nonzero", "missing-log"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                def change(row):
                    if mutation == "empty":
                        row["commands"] = []
                    elif mutation == "duplicate":
                        row["commands"].append(deepcopy(row["commands"][0]))
                    elif mutation == "timeout":
                        row["commands"][0]["status"] = "timeout"
                    elif mutation == "nonzero":
                        row["commands"][0]["exitcode"] = 1
                    else:
                        row["commands"][0]["stdout"] = "missing.log"
                fixture.mutate("result.json", change)
                self.assert_rejected(fixture)

    def test_artifact_escape_duplicates_size_and_content_mismatches_fail(self):
        for mutation in ("../outside", "/absolute", "C:/absolute", "nested\\file", "duplicate", "size", "sha", "unlisted"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                def change(row):
                    if mutation == "duplicate":
                        row["artifacts"].append(deepcopy(row["artifacts"][0]))
                    elif mutation == "size":
                        row["artifacts"][0]["size"] += 1
                    elif mutation == "sha":
                        row["artifacts"][0]["sha256"] = "f" * 64
                    elif mutation == "unlisted":
                        fixture.path("unlisted.bin").write_bytes(b"unlisted implementation")
                    else:
                        row["artifacts"][0]["path"] = mutation
                fixture.mutate("result.json", change)
                self.assert_rejected(fixture)

    def test_changed_artifact_bytes_fail_even_when_reported_size_is_unchanged(self):
        fixture = Fixture(self.root)
        path = fixture.path("rebuilt")
        path.write_bytes(b"x" * path.stat().st_size)
        self.assert_rejected(fixture, "artifact SHA/size mismatch")

    def test_unreadable_directory_cannot_disappear_from_an_otherwise_matching_inventory(self):
        fixture = Fixture(self.root)
        walk = gate.os.walk

        def unreadable(root, *args, **kwargs):
            yield from walk(root, *args, **kwargs)
            # Without onerror this omitted subtree would be silently ignored,
            # while all returned files still exactly match the claimed list.
            if kwargs.get("onerror"):
                kwargs["onerror"](PermissionError("unreadable evidence subtree"))

        with patch.object(gate.os, "walk", side_effect=unreadable):
            self.assert_rejected(fixture, "cannot inventory case artifact directory")

    def test_stage_evidence_reference_cannot_escape_or_name_an_unlisted_file(self):
        for path in ("../outside", "logs/not-recorded.txt"):
            with self.subTest(path=path), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                fixture.mutate("result.json", lambda row: row["stages"]["inventory"]["details"].update(evidence=[path]))
                self.assert_rejected(fixture)

    def test_a_log_cannot_be_presented_as_a_recovered_source_file(self):
        fixture = Fixture(self.root)
        fixture.path("recovered/sources/compiler.log").write_text("successful source recovery\n")
        fixture.mutate("recovered/report.json", lambda row: row["outputs"].update(native_source="sources/compiler.log"))
        fixture.mutate("compile-proof.json", lambda row: row.update(input_sources=["recovered/sources/compiler.log"]))
        self.assert_rejected(fixture, "non-source file")

    def test_original_implementation_cannot_hide_behind_an_empty_self_report(self):
        fixture = Fixture(self.root, dependency_locks=[{
            "origin": "platform-sdk", "sha256": hashlib.sha256(ORIGINAL).hexdigest()}])
        fixture.path("supposed-sdk.o").write_bytes(ORIGINAL)
        fixture.mutate("dependencies.json", lambda row: row["inputs"].append({"path": "supposed-sdk.o", "origin": "platform-sdk"}))
        fixture.mutate("result.json", lambda row: row["commands"][0]["argv"].append("/ci/work/case-00/supposed-sdk.o"))
        self.assert_rejected(fixture, "reuse original implementation bytes")

    def test_undeclared_original_link_input_is_found_in_the_actual_command(self):
        fixture = Fixture(self.root)
        fixture.mutate("result.json", lambda row: row["commands"][0]["argv"].append("/ci/work/case-00/original"))
        self.assert_rejected(fixture, "undeclared artifact input/output")

    def test_arbitrary_external_dependency_cannot_be_allowed_by_its_claimed_origin(self):
        fixture = Fixture(self.root)
        fixture.path("support.a").write_bytes(b"unlocked dependency implementation")
        fixture.mutate("dependencies.json", lambda row: row["inputs"].append({"path": "support.a", "origin": "platform-sdk"}))
        self.assert_rejected(fixture, "independent lock")

    def test_copy_command_or_unconsumed_source_is_not_independent_compilation(self):
        for mutation in ("copy", "missing-source"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                def change(row):
                    if mutation == "copy":
                        row["commands"][0]["argv"][0] = "/bin/cp"
                    else:
                        row["commands"][0]["argv"].pop(1)
                fixture.mutate("result.json", change)
                self.assert_rejected(fixture)

    def test_behavior_must_execute_the_actual_independent_compiler_output(self):
        fixture = Fixture(self.root)
        fixture.mutate("result.json", lambda row: row["commands"][2]["argv"].__setitem__(-1, "/ci/work/case-00/original"))
        self.assert_rejected(fixture, "did not execute/install the compiler output")

    def test_behavior_cannot_reference_an_unbound_binary_or_alias_original(self):
        fixture = Fixture(self.root)
        fixture.mutate("behavior-proof.json", lambda row: row.update(rebuilt_artifact="original"))
        self.assert_rejected(fixture, "not bound to original and independently compiled outputs")

    def test_expected_file_copy_is_not_an_execution_result(self):
        fixture = Fixture(self.root)
        shutil.copyfile(fixture.path("expected.json"), fixture.path("copied-results.json"))
        fixture.mutate("behavior-proof.json", lambda row: row.update(rebuilt_results="copied-results.json"))
        self.assert_rejected(fixture, "not the recorded stdout")

    def test_assertion_omissions_role_changes_failures_and_value_drift_fail(self):
        for mutation in ("empty", "id", "obligation", "failed", "value"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                def change(row):
                    if mutation == "empty":
                        row["assertions"] = []
                    else:
                        field, value = {"id": ("id", "other"), "obligation": ("obligation", "other"),
                                        "failed": ("status", "failed"), "value": ("value", 43)}[mutation]
                        row["assertions"][0][field] = value
                fixture.mutate("logs/rebuilt.stdout", change)
                self.assert_rejected(fixture)

    def test_all_three_matching_results_still_must_cover_manifest_business_obligations(self):
        fixture = Fixture(self.root)
        for path in ("expected.json", "logs/original.stdout", "logs/rebuilt.stdout"):
            fixture.mutate(path, lambda row: row["assertions"][0].update(obligation="trivial-launch"))
        self.assert_rejected(fixture, "omits or substitutes manifest obligations")

    def test_wrong_runtime_sdk_or_architecture_is_not_the_required_behavior(self):
        for field, value in (("kind", "macos"), ("sdk", "iphoneos"), ("architecture", "x86_64")):
            with self.subTest(field=field), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                fixture.mutate("behavior-proof.json", lambda row: row["runtime"].update({field: value}))
                self.assert_rejected(fixture)

    def test_stale_proof_commit_or_manifest_is_not_reusable(self):
        for path in ("compile-proof.json", "behavior-proof.json"):
            with self.subTest(path=path), tempfile.TemporaryDirectory(dir=self.root) as temporary:
                fixture = Fixture(Path(temporary))
                fixture.mutate(path, lambda row: row.update(manifest_sha256="f" * 64))
                self.assert_rejected(fixture, "differs from the qualified case")

    def test_duplicate_json_keys_are_rejected(self):
        fixture = Fixture(self.root)
        fixture.needs_path.write_text('{"build":{"result":"failure"},"build":{"result":"success"}}')
        with self.assertRaisesRegex(gate.GateError, "duplicate JSON key"):
            fixture.audit()

    def test_cli_writes_a_failure_report_and_returns_nonzero_for_incomplete_evidence(self):
        fixture = Fixture(self.root, present=0)
        output = self.root / "gate/report.json"
        with redirect_stdout(io.StringIO()):
            code = gate.main(["--manifest", str(fixture.manifest_path), "--results", str(fixture.results),
                              "--consumer-commit", CONSUMER, "--needs-json", str(fixture.needs_path),
                              "--output", str(output)])
        self.assertEqual(code, 1)
        self.assertFalse(json.loads(output.read_text())["qualified"])


if __name__ == "__main__":
    unittest.main()
