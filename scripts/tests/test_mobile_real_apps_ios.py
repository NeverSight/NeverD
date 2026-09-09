"""CI orchestration tests: tool success is not source-recovery acceptance."""
from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "mobile_real_apps_ios", Path(__file__).resolve().parents[1] / "mobile_real_apps_ios.py"
)
ios = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ios)


class Context:
    """Replace only the external command boundary, retaining filesystem flow."""

    def __init__(self, root):
        self.source = root / "checkout"
        self.work = root / "evidence"
        self.source.mkdir()
        self.work.mkdir()
        self.neverd = root / "neverd"
        self.timeout = 4200
        self.app = {
            "repository": "https://github.com/example/ActualApp",
            "source_commit": "a" * 40,
            "license": "MIT",
            "ios": {
                "xcode": "26.5",
                "project": "Actual.xcodeproj", "scheme": "Actual",
                "product": "Actual.app", "package_resolved": "Package.resolved",
                "required_artifacts": ["Actual", "Frameworks/Shared.framework/Shared"],
            },
        }
        self.variant = {
            "id": "actual-release-arm64-sim", "app": "actual", "platform": "ios",
            "profile": "release", "architecture": "arm64", "sdk": "iphonesimulator",
        }
        (self.source / "Actual.xcodeproj").mkdir()
        (self.source / "Package.resolved").write_text('{"pins": [], "version": 3}')
        self.commands = []
        self.stages = {}
        self.documents = {}
        self.failures = []
        self.tool_failure = None
        self.recovery_failure = None
        self.report_status = "success"
        self.xcode_version = "26.5"
        self.sdk_version = "26.5"
        self.extra_bundle_files = {}
        self.omit_framework = False
        self.after_recovery = None

    def write_json(self, name, data):
        self.documents[name] = data
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding="utf-8")

    def stage(self, name, status, **details):
        if name in self.stages:
            raise AssertionError(f"stage emitted twice: {name}")
        self.stages[name] = {"status": status, **details}

    def fail(self, reason):
        self.failures.append(reason)

    def command(self, name, argv, cwd=None, env=None, timeout=None, allow_failure=False):
        argv = [str(x) for x in argv]
        self.commands.append((name, argv, timeout))
        if self.tool_failure and self.tool_failure in argv:
            return subprocess.CompletedProcess(argv, 1, "", "intentional tool failure")
        if argv[:3] == ["git", "rev-parse", "HEAD"]:
            return subprocess.CompletedProcess(argv, 0, "a" * 40 + "\n", "")
        if argv[:2] == ["xcodebuild", "-version"]:
            return subprocess.CompletedProcess(argv, 0, f"Xcode {self.xcode_version}\nBuild version TEST\n", "")
        if "--show-sdk-path" in argv:
            return subprocess.CompletedProcess(argv, 0, "/Applications/Xcode.app/SDKs/iPhoneSimulator.sdk\n", "")
        if "--show-sdk-version" in argv:
            return subprocess.CompletedProcess(argv, 0, self.sdk_version + "\n", "")
        if "--show-sdk-build-version" in argv:
            return subprocess.CompletedProcess(argv, 0, "TESTSDK\n", "")
        if "--find" in argv:
            return subprocess.CompletedProcess(argv, 0, "/Apple/" + argv[-1] + "\n", "")
        if "-showBuildSettings" in argv:
            return subprocess.CompletedProcess(argv, 0, json.dumps([{
                "target": "Actual", "buildSettings": {
                    "CLANG_ENABLE_OBJC_ARC": "YES", "SWIFT_OPTIMIZATION_LEVEL": "-O",
                    "SDKROOT": "/Apple/iPhoneSimulator.sdk", "ARCHS": self.variant["architecture"],
                },
            }]), "")
        if argv[0] == "xcodebuild" and "build" in argv:
            derived = Path(argv[argv.index("-derivedDataPath") + 1])
            bundle = derived / "Build/Products" / ("Release-" + self.variant["sdk"]) / "Actual.app"
            bundle.mkdir(parents=True)
            files = {"Actual": b"\xcf\xfa\xed\xfe" + bytes(60)}
            if not self.omit_framework:
                files["Frameworks/Shared.framework/Shared"] = b"\xcf\xfa\xed\xfe" + bytes(60)
            files.update(self.extra_bundle_files)
            for path, data in files.items():
                destination = bundle / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
            support = derived / "Build/Products" / ("Release-" + self.variant["sdk"])
            dwarf = support / "Actual.app.dSYM/Contents/Resources/DWARF/Actual"
            dwarf.parent.mkdir(parents=True)
            dwarf.write_bytes(b"independent build debug evidence")
            (support / "Actual-arm64-Release-LinkMap.txt").write_text("# Object files:\n# Symbols:\n")
            (support / "Actual.common-args.resp").write_text("-fobjc-arc -O\n")
            return subprocess.CompletedProcess(argv, 0, "real build boundary mocked", "")
        if "nm" in argv:
            return subprocess.CompletedProcess(argv, 0, "0000000100001000 (__TEXT,__text) external _$s4Demo3fooyyF\n", "")
        if "-function_starts" in argv:
            return subprocess.CompletedProcess(argv, 0, "-function_starts:\n  0x100001000 foo\n", "")
        if "-l" in argv and "otool" in argv:
            return subprocess.CompletedProcess(argv, 0, "cmd LC_FUNCTION_STARTS\ncmd LC_BUILD_VERSION\nplatform 7\n", "")
        if "--expand" in argv:
            symbols = argv[argv.index("--tree-only") + 1:]
            return subprocess.CompletedProcess(argv, 0, "".join(
                f"Demangling for {symbol}\nkind=Global\n  kind=Function\n    kind=Module, text=\"Demo\"\n"
                for symbol in symbols
            ), "")
        if argv[0] == str(self.neverd):
            binary = Path(argv[argv.index("mobile") + 1])
            if self.recovery_failure == binary.name:
                if self.after_recovery:
                    self.after_recovery()
                return subprocess.CompletedProcess(argv, 2, "", "unsupported actual artifact")
            output = Path(argv[argv.index("-o") + 1])
            output.mkdir(parents=True)
            (output / "report.json").write_text(json.dumps({
                "status": self.report_status, "platform": "ios",
                "architecture": self.variant["architecture"],
                "objc_method_recovery": {"method_count": 0, "recovered_method_count": 0},
                "swift_method_recovery": {"method_count": 0, "recovered_method_count": 0},
            }))
            if self.after_recovery:
                self.after_recovery()
            return subprocess.CompletedProcess(argv, 0, "", "")
        return subprocess.CompletedProcess(argv, 0, "", "")


class IOSRealAppOrchestrationTests(unittest.TestCase):
    def context(self, **changes):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        ctx = Context(Path(temporary.name))
        for key, value in changes.items():
            setattr(ctx, key, value)
        return ctx

    def run_context(self, **changes):
        ctx = self.context(**changes)
        with patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}):
            ios.run_ios(ctx)
        return ctx

    def test_successful_tools_cannot_claim_unimplemented_source_acceptance(self):
        ctx = self.run_context()
        self.assertEqual(set(ctx.stages), set(ios.REQUIRED_STAGES))
        self.assertEqual(ctx.stages["original_build"]["status"], "success")
        for stage in ("inventory", "recovery", "recompile", "behavior"):
            self.assertEqual(ctx.stages[stage]["status"], "incomplete", stage)
        self.assertTrue(ctx.failures)
        recovered = [argv[argv.index("mobile") + 1] for _, argv, _ in ctx.commands if "mobile" in argv]
        self.assertEqual({Path(path).name for path in recovered}, {"Actual", "Shared"})
        for stage in ctx.stages.values():
            if stage["status"] == "success":
                self.assertTrue(stage["evidence"])
                for member in stage["evidence"]:
                    self.assertFalse(Path(member).is_absolute())
                    self.assertTrue((ctx.work / member).is_file())

    def test_success_stage_cannot_reference_missing_or_escaping_evidence(self):
        ctx = self.context()
        session = ios.Session(ctx)
        for evidence in (None, [], ["missing.json"], ["../outside.json"]):
            with self.subTest(evidence=evidence), self.assertRaises(ios.EvidenceError):
                session.stage("provenance", "success", evidence=evidence)
        self.assertEqual(ctx.stages, {})

    def test_all_package_commands_avoid_global_cache_and_keep_locked_versions(self):
        ctx = self.run_context()
        expected = {"resolve-packages", "build-settings", "original-release-build"}
        commands = [(name, argv) for name, argv, _ in ctx.commands if name in expected]
        self.assertEqual({name for name, _ in commands}, expected)
        self.assertEqual(len(commands), 3)
        for name, argv in commands:
            with self.subTest(command=name):
                self.assertIn("-disablePackageRepositoryCache", argv)
                self.assertIn("-onlyUsePackageVersionsFromResolvedFile", argv)
                self.assertIn("SWIFT_OPTIMIZATION_LEVEL=-O", argv)
                self.assertNotIn("SWIFT_OPTIMIZATION_LEVEL=-Onone", argv)
                packages = Path(argv[argv.index("-clonedSourcePackagesDirPath") + 1])
                self.assertEqual(packages, ctx.work.parent / (ctx.work.name + "-ios-build") / "source-packages")
        self.assertEqual((ctx.source / "Package.resolved").read_bytes(),
                         (ctx.work / "Package.resolved").read_bytes())

    def test_embedded_framework_and_extension_are_attempted_after_main_failure(self):
        ctx = self.run_context(recovery_failure="Actual", extra_bundle_files={
            "PlugIns/Reader.appex/Reader": b"\xcf\xfa\xed\xfe" + bytes(60),
            "Resources/not-a-binary.json": b"{}",
        })
        calls = [argv for _, argv, _ in ctx.commands if "mobile" in argv]
        self.assertEqual([Path(argv[2]).name for argv in calls], ["Actual", "Shared", "Reader"])
        rows = ctx.stages["recovery"]["artifacts"]
        self.assertEqual([row["status"] for row in rows], ["failed", "incomplete", "incomplete"])
        self.assertEqual(ctx.stages["recovery"]["status"], "failed")
        self.assertEqual(len([name for name in ctx.documents if name.endswith("-recovery.json")]), 3)

    def test_failed_apple_tools_do_not_suppress_neverd_attempts(self):
        ctx = self.run_context(tool_failure="dyld_info")
        self.assertEqual(len([argv for _, argv, _ in ctx.commands if "mobile" in argv]), 2)
        self.assertEqual(ctx.stages["inventory"]["status"], "incomplete")
        first = ctx.stages["inventory"]["artifacts"][0]
        self.assertTrue(any("exited 1" in issue for issue in first["issues"]))
        self.assertFalse(first["native_denominator_known"])
        self.assertFalse(first["objc_denominator_known"])

    def test_missing_required_framework_is_visible_while_existing_main_is_analyzed(self):
        ctx = self.run_context(omit_framework=True)
        issues = ctx.stages["inventory"]["enumeration_issues"]
        self.assertTrue(any("Frameworks/Shared.framework/Shared" in issue for issue in issues))
        self.assertEqual(len([argv for _, argv, _ in ctx.commands if "mobile" in argv]), 1)
        self.assertTrue(ctx.failures)

    def test_partial_report_is_not_a_successful_recovery_stage(self):
        ctx = self.run_context(report_status="partial")
        self.assertEqual(ctx.stages["recovery"]["status"], "incomplete")
        self.assertEqual({row["report_status"] for row in ctx.stages["recovery"]["artifacts"]}, {"partial"})
        self.assertEqual(ctx.stages["behavior"]["status"], "incomplete")
        self.assertFalse(any("test" in argv for _, argv, _ in ctx.commands))

    def test_case_deadline_records_each_remaining_artifact_without_starting_more_processes(self):
        clock = [100.0]
        ctx = self.context(after_recovery=lambda: clock.__setitem__(0, 5000.0))
        with patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}), \
                patch.object(ios.time, "monotonic", side_effect=lambda: clock[0]):
            ios.run_ios(ctx)
        self.assertEqual(len([argv for _, argv, _ in ctx.commands if "mobile" in argv]), 1)
        rows = ctx.stages["recovery"]["artifacts"]
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[1]["status"], "failed")
        self.assertIn("budget exhausted", rows[1]["reason"])
        self.assertIn("ios-artifact-0001-recovery.json", ctx.documents)

    def test_release_and_size_preserve_requested_sdk_and_architecture(self):
        ctx = self.context()
        for profile in ("release", "size"):
            for architecture, sdk in (("arm64", "iphoneos"), ("arm64", "iphonesimulator"),
                                      ("x86_64", "iphonesimulator")):
                with self.subTest(profile=profile, architecture=architecture, sdk=sdk):
                    ctx.variant.update(profile=profile, architecture=architecture, sdk=sdk)
                    argv = ios.build_arguments(ctx, ctx.work / "derived", ctx.work / "packages")
                    self.assertEqual(argv[argv.index("-sdk") + 1], sdk)
                    self.assertIn("ARCHS=" + architecture, argv)
                    self.assertIn("SWIFT_OPTIMIZATION_LEVEL=" + ("-Osize" if profile == "size" else "-O"), argv)
                    self.assertIn("-onlyUsePackageVersionsFromResolvedFile", argv)
                    self.assertEqual(argv[argv.index("-clonedSourcePackagesDirPath") + 1], ctx.work / "packages")
                    self.assertNotIn("-clonedSourcePackagesDir", argv)
                    self.assertIn("-disablePackageRepositoryCache", argv)
                    self.assertIn("CODE_SIGNING_ALLOWED=NO", argv)
                    self.assertEqual("GCC_OPTIMIZATION_LEVEL=s" in argv, profile == "size")
        ctx.variant["profile"] = "unknown"
        with self.assertRaisesRegex(ios.EvidenceError, "explicit iOS profile"):
            ios.build_arguments(ctx, ctx.work / "derived", ctx.work / "packages")

    def test_manifest_xcode_and_actual_xcode_sdk_versions_must_match(self):
        for manifest, xcode, sdk in ((None, "26.5", "26.5"), ("26.4", "26.5", "26.5"),
                                     ("26.5", "26.4", "26.5"), ("26.5", "26.5", "26.4")):
            with self.subTest(manifest=manifest, xcode=xcode, sdk=sdk):
                ctx = self.context(xcode_version=xcode, sdk_version=sdk)
                ctx.app["ios"]["xcode"] = manifest
                with patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}):
                    ios.run_ios(ctx)
                self.assertEqual(ctx.stages["provenance"]["status"], "failed")
                self.assertTrue(ctx.failures)
                self.assertFalse(any(name in ("resolve-packages", "original-release-build")
                                     for name, _, _ in ctx.commands))

    def test_outputs_retain_app_and_debug_evidence_without_uploading_derived_or_packages(self):
        ctx = self.run_context()
        self.assertTrue((ctx.work / "original/Actual.app/Actual").is_file())
        self.assertTrue((ctx.work / "original/Actual.app/Frameworks/Shared.framework/Shared").is_file())
        preserved = ctx.documents["ios-build-artifacts.json"]["files"]
        self.assertTrue(any(".dSYM/" in row["path"] for row in preserved))
        self.assertTrue(any(row["path"].endswith("LinkMap.txt") for row in preserved))
        self.assertTrue(any(row["path"].endswith(".resp") for row in preserved))
        self.assertFalse((ctx.work / "derived-data").exists())
        self.assertFalse((ctx.work / "source-packages").exists())
        self.assertTrue((ctx.work.parent / (ctx.work.name + "-ios-build") / "derived-data").is_dir())

    def test_execution_outside_github_actions_starts_no_commands(self):
        ctx = self.context()
        with patch.dict(os.environ, {"GITHUB_ACTIONS": "false"}):
            ios.run_ios(ctx)
        self.assertEqual(ctx.commands, [])
        self.assertEqual(ctx.stages["provenance"]["status"], "failed")
        self.assertEqual(set(ctx.stages), set(ios.REQUIRED_STAGES))
        self.assertTrue(ctx.failures)

    def test_official_unknown_roles_and_incomplete_trees_are_not_declared_metadata(self):
        output = ("Demangling for $sKnown\nkind=Global\n  kind=Function\n    kind=AsyncAnnotation\n"
                  "Demangling for $sUnknown\nkind=Global\n  kind=FutureCompilerRole\n")
        roles = ios.demangle_roles(output, ["$sKnown", "$sUnknown"])
        self.assertEqual(roles["$sKnown"]["classification"], "callable")
        self.assertTrue(roles["$sKnown"]["async_tree_evidence"])
        self.assertEqual(roles["$sUnknown"]["classification"], "unknown")
        with self.assertRaisesRegex(ios.EvidenceError, "omitted"):
            ios.demangle_roles(output, ["$sKnown", "$sUnknown", "$sMissing"])
        with self.assertRaisesRegex(ios.EvidenceError, "duplicate"):
            ios.demangle_roles(output + output, ["$sKnown", "$sUnknown"])

    def test_native_aliases_are_preserved_and_undefined_swift_imports_are_not_definitions(self):
        rows = ios.swift_symbols(
            "0000000100001000 (__TEXT,__text) external _$sAliasOne\n"
            "0000000100001000 (__TEXT,__text) external _$sAliasTwo\n"
            "                 (undefined) external _$sImported\n"
        )
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["entry"], rows[1]["entry"])
        self.assertNotEqual(rows[0]["mangled_symbol"], rows[1]["mangled_symbol"])

    def test_actual_package_revision_mismatch_is_not_hidden_by_lockfile(self):
        ctx = self.context()
        packages = ctx.work / "packages"
        (packages / "checkouts/Example").mkdir(parents=True)
        (packages / "workspace-state.json").write_text(json.dumps({"object": {"dependencies": [{
            "packageRef": {"identity": "example"}, "subpath": "Example",
        }]}}))
        lock = {"pins": [{"identity": "example", "location": "https://github.com/example/library",
                          "state": {"revision": "b" * 40}}]}
        records, issues = ios.resolve_dependencies(ios.Session(ctx), packages, lock)
        self.assertFalse(records[0]["matched"])
        self.assertTrue(any("does not match" in issue for issue in issues))

    def test_symlink_escape_and_unknown_profile_do_not_use_fallback_inputs(self):
        ctx = self.context()
        (ctx.source / "outside").symlink_to(ctx.work, target_is_directory=True)
        with self.assertRaisesRegex(ios.EvidenceError, "outside"):
            ios.relative_path(ctx.source, "outside/file")
        with self.assertRaisesRegex(ios.EvidenceError, "escapes"):
            ios.relative_path(ctx.source, "../elsewhere")


if __name__ == "__main__":
    unittest.main()
