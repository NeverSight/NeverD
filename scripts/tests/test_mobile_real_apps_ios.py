"""CI orchestration tests: tool success is not source-recovery acceptance."""
from __future__ import annotations

import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from scripts import mobile_real_apps_common as common
from scripts.tests.test_mobile_real_apps_common import ProcessDouble
from scripts.tests.test_mobile_swift_toolchain import installed_identity_fixture


SPEC = importlib.util.spec_from_file_location(
    "mobile_real_apps_ios", Path(__file__).resolve().parents[1] / "mobile_real_apps_ios.py"
)
ios = importlib.util.module_from_spec(SPEC)
with patch.object(sys, "path", [str(Path(__file__).resolve().parents[1]), *sys.path]):
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
            "toolchain": "XcodeDefault",
        }
        (self.source / "Actual.xcodeproj").mkdir()
        (self.source / "Package.resolved").write_text('{"pins": [], "version": 3}')
        self.commands = []
        self.environments = {}
        self.stages = {}
        self.documents = {}
        self.failures = []
        self.tool_failure = None
        self.recovery_failure = None
        self.report_status = "success"
        self.xcode_version = "26.5"
        self.sdk_version = "26.5"
        self.sdk_path = "/Applications/Xcode.app/SDKs/iPhoneSimulator.sdk"
        self.swift_paths = {}
        self.build_log = "real build boundary mocked"
        self.settings_optimization = None
        self.extra_bundle_files = {}
        self.omit_framework = False
        self.after_recovery = None
        self.load_commands = "cmd LC_FUNCTION_STARTS\ncmd LC_BUILD_VERSION\nplatform 7\n"

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
        self.environments[name] = env
        if self.tool_failure and self.tool_failure in argv:
            return subprocess.CompletedProcess(argv, 1, "", "intentional tool failure")
        if argv[:3] == ["git", "rev-parse", "HEAD"]:
            return subprocess.CompletedProcess(argv, 0, "a" * 40 + "\n", "")
        if argv[0] == "xcodebuild" and "-version" in argv:
            return subprocess.CompletedProcess(argv, 0, f"Xcode {self.xcode_version}\nBuild version TEST\n", "")
        if "--show-sdk-path" in argv:
            return subprocess.CompletedProcess(argv, 0, self.sdk_path + "\n", "")
        if "--show-sdk-version" in argv:
            return subprocess.CompletedProcess(argv, 0, self.sdk_version + "\n", "")
        if "--show-sdk-build-version" in argv:
            return subprocess.CompletedProcess(argv, 0, "TESTSDK\n", "")
        if "--find" in argv:
            return subprocess.CompletedProcess(argv, 0, self.swift_paths.get(argv[-1], "/Apple/" + argv[-1]) + "\n", "")
        if "-showBuildSettings" in argv:
            return subprocess.CompletedProcess(argv, 0, json.dumps([{
                "target": "Actual", "buildSettings": {
                    "CLANG_ENABLE_OBJC_ARC": "YES",
                    "SWIFT_OPTIMIZATION_LEVEL": self.settings_optimization or ("-Osize" if self.variant["profile"] == "size" else "-O"),
                    "SWIFT_EXEC": self.swift_paths.get("swiftc", "/Apple/swiftc"),
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
            return subprocess.CompletedProcess(argv, 0, self.build_log, "")
        if "nm" in argv:
            return subprocess.CompletedProcess(argv, 0, "0000000100001000 (__TEXT,__text) external _$s4Demo3fooyyF\n", "")
        if "-function_starts" in argv:
            return subprocess.CompletedProcess(argv, 0, "-function_starts:\n  0x100001000 foo\n", "")
        if "-l" in argv and "otool" in argv:
            return subprocess.CompletedProcess(argv, 0, self.load_commands, "")
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

    def comparison_context(self, profile="release"):
        ctx = self.context()
        ctx.variant.update(id=f"actual-{profile}-arm64-sim-swift64", profile=profile,
                           toolchain=ios.BUNDLE_IDENTIFIER, comparison_of=f"actual-{profile}-arm64-sim")
        qualifier = sys.modules["qualify_mobile_swift_toolchain"]
        receipt = self.enterContext(installed_identity_fixture(
            ctx.work.parent, module=qualifier, case_id=ctx.variant["id"]))
        self.enterContext(patch.object(ios, "DEVELOPER_DIR", receipt["xcode"]["developer_dir"]))
        self.enterContext(patch.dict(os.environ, {"GITHUB_ACTIONS": "true", "GITHUB_RUN_ID": "123",
                                                "GITHUB_RUN_ATTEMPT": "2", "GITHUB_SHA": "d" * 40}))
        ctx.swift_paths = {row["name"]: row["reported_path"] for row in receipt["tools"]}
        ctx.sdk_path = next(row["path"] for row in receipt["sdks"] if row["name"] == ctx.variant["sdk"])
        opt = "-Osize" if profile == "size" else "-O"
        ctx.build_log = (f'    builtin-SwiftDriver -- {ctx.swift_paths["swiftc"]} -module-name Actual {opt} '
                         '@Actual.SwiftFileList -c -emit-module -target arm64-apple-ios18.0-simulator\n'
                         f'    {ctx.swift_paths["swift-frontend"]} -frontend -emit-module -module-name Actual {opt}\n')
        folder = ctx.work.parent / "installation/evidence"
        folder.mkdir(parents=True)
        receipt["commands"] = []
        for name in sorted(ios.REQUIRED_INSTALLATION_COMMANDS):
            row = {"name": name, "argv": ["mocked-installation-boundary", name], "exitcode": 0}
            for stream in ("stdout", "stderr"):
                value = (("c" * 40 + "\n") if name == "consumer-source-head" else f"retained {name} {stream}\n").encode()
                member = name + "." + stream
                (folder / member).write_bytes(value)
                row[stream] = member
                row[stream + "_sha256"] = hashlib.sha256(value).hexdigest()
            receipt["commands"].append(row)
        path = folder / "qualification.json"
        path.write_text(json.dumps(receipt))
        return ctx, path, receipt

    def test_snapshot_cases_revalidate_current_files_route_three_tools_and_keep_acceptance_incomplete(self):
        for profile in ("release", "size"):
            with self.subTest(profile=profile):
                ctx, path, receipt = self.comparison_context(profile)
                ios.run_ios(ctx, toolchain_receipt=path, consumer_commit="c" * 40)
                self.assertEqual(ctx.stages["provenance"]["status"], "success")
                self.assertEqual(ctx.stages["original_build"]["status"], "success")
                self.assertEqual(set(ctx.stages), set(ios.REQUIRED_STAGES))
                self.assertTrue(ctx.failures)
                for name in ("inventory", "recovery", "recompile", "behavior"):
                    self.assertEqual(ctx.stages[name]["status"], "incomplete")
                self.assertEqual(ctx.timeout, 4200)
                self.assertFalse(any(name in ios.REQUIRED_INSTALLATION_COMMANDS - {"xcode-version"}
                                     for name, _, _ in ctx.commands))
                self.assertEqual((ctx.work / "toolchain/qualification.json").read_bytes(), path.read_bytes())
                self.assertTrue((ctx.work / "toolchain/install.stdout").is_file())
                self.assertEqual(ctx.documents["ios-toolchain-build-observation.json"]["compile_command_count"], 1)
                self.assertFalse(ctx.documents["ios-toolchain-build-observation.json"]["application_behavior_verified"])
                for name, argv, timeout in ctx.commands:
                    self.assertLessEqual(timeout, 4200)
                    if argv[0] == "xcodebuild":
                        self.assertEqual(argv[argv.index("-toolchain") + 1], ios.BUNDLE_IDENTIFIER)
                        if name != "xcode-version":
                            self.assertIn("SWIFT_EXEC=" + ctx.swift_paths["swiftc"], argv)
                            self.assertIn("SWIFT_OPTIMIZATION_LEVEL=" + ("-Osize" if profile == "size" else "-O"), argv)
                    if argv[0] == "xcrun":
                        expected = ios.BUNDLE_IDENTIFIER if any(tool in argv for tool in ctx.swift_paths) else "XcodeDefault"
                        self.assertEqual(argv[argv.index("--toolchain") + 1], expected)
                    self.assertEqual(ctx.environments[name]["TOOLCHAINS"], ios.BUNDLE_IDENTIFIER)

    def test_failed_stale_or_missing_installation_stops_only_its_case_and_retains_six_stage_failure(self):
        for mutation in ("missing", "failed", "consumer", "workflow", "run", "attempt", "case", "malformed"):
            with self.subTest(mutation=mutation):
                ctx, path, receipt = self.comparison_context()
                if mutation == "failed":
                    receipt["status"] = "failed"
                    receipt["error"] = "installer rejected package"
                elif mutation == "workflow":
                    receipt["workflow_commit"] = "e" * 40
                elif mutation in ("consumer", "run", "attempt", "case"):
                    key = {"consumer": "consumer_commit", "run": "run_id", "attempt": "run_attempt", "case": "case_id"}[mutation]
                    receipt[key] = "different"
                path.write_text("{" if mutation == "malformed" else json.dumps(receipt))
                ios.run_ios(ctx, toolchain_receipt=None if mutation == "missing" else path, consumer_commit="c" * 40)
                self.assertEqual(ctx.commands, [])
                self.assertEqual(set(ctx.stages), set(ios.REQUIRED_STAGES))
                self.assertEqual(ctx.stages["provenance"]["status"], "failed")
                self.assertTrue(ctx.failures)
                if mutation != "missing":
                    self.assertEqual((ctx.work / "toolchain/qualification.json").read_bytes(), path.read_bytes())
                self.assertFalse((ctx.work / "original").exists())

    def test_installation_logs_are_required_and_byte_bound_to_the_current_receipt(self):
        for mutation in ("modified", "missing", "duplicate", "failed-step", "omitted-verification"):
            with self.subTest(mutation=mutation):
                ctx, path, receipt = self.comparison_context()
                row = receipt["commands"][0]
                if mutation == "modified":
                    (path.parent / row["stdout"]).write_text("modified installation evidence")
                elif mutation == "missing":
                    (path.parent / row["stderr"]).unlink()
                elif mutation == "duplicate":
                    receipt["commands"].append(dict(row))
                elif mutation == "failed-step":
                    row["exitcode"] = 1
                else:
                    receipt["commands"] = [row for row in receipt["commands"] if row["name"] != "swift-frontend-signature"]
                path.write_text(json.dumps(receipt))
                ios.run_ios(ctx, toolchain_receipt=path, consumer_commit="c" * 40)
                self.assertEqual(ctx.commands, [])
                self.assertEqual(ctx.stages["provenance"]["status"], "failed")
                self.assertTrue(ctx.failures)

    def test_actual_compiler_sdk_fallback_or_lower_optimization_cannot_claim_build_success(self):
        for mutation in ("xcrun", "sdk", "settings", "command-tool", "command-optimization", "module-only"):
            with self.subTest(mutation=mutation):
                ctx, path, receipt = self.comparison_context()
                if mutation == "xcrun":
                    ctx.swift_paths["swift-demangle"] = "/Apple/swift-demangle"
                elif mutation == "sdk":
                    ctx.sdk_path = "/Apple/unqualified.sdk"
                elif mutation == "settings":
                    ctx.settings_optimization = "-Onone"
                elif mutation == "command-tool":
                    ctx.build_log = ctx.build_log.replace(ctx.swift_paths["swiftc"], "/Apple/swiftc")
                elif mutation == "command-optimization":
                    ctx.build_log = ctx.build_log.replace(" -O ", " -Onone ")
                else:
                    ctx.build_log = f'{ctx.swift_paths["swift-frontend"]} -frontend -emit-module -module-name Actual -O\n'
                ios.run_ios(ctx, toolchain_receipt=path, consumer_commit="c" * 40)
                self.assertEqual(set(ctx.stages), set(ios.REQUIRED_STAGES))
                self.assertTrue(ctx.failures)
                self.assertNotEqual(ctx.stages["original_build"]["status"], "success")
                self.assertFalse(any("mobile" in argv for _, argv, _ in ctx.commands))

    def test_baseline_rejects_snapshot_receipt_and_missing_toolchain_without_running_commands(self):
        for mode in ("receipt", "missing-toolchain"):
            ctx = self.context()
            if mode == "missing-toolchain":
                ctx.variant.pop("toolchain")
            with patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}):
                ios.run_ios(ctx, toolchain_receipt=ctx.work / "snapshot.json" if mode == "receipt" else None)
            self.assertEqual(ctx.commands, [])
            self.assertEqual(ctx.stages["provenance"]["status"], "failed")
            self.assertTrue(ctx.failures)

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
                self.assertEqual(argv[argv.index("-toolchain") + 1], "XcodeDefault")
                self.assertEqual(ctx.environments[name]["TOOLCHAINS"], "XcodeDefault")
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

    def test_fixups_and_all_nonlazy_sections_are_collected_without_false_green(self):
        sections = "".join(
            f"Section\n  sectname {name}\n   segname {segment}\n"
            f"      addr {hex(address)}\n      size 0x8\n    offset 4096\n     flags 0x0\n"
            for segment, name, address in (("__DATA_CONST", "__objc_nlcatlist", 0x100001000),
                                           ("__DATA", "__objc_nlclslist", 0x100002000))
        )
        ctx = self.run_context(load_commands=sections)
        self.assertEqual(len([argv for _, argv, _ in ctx.commands if "-fixups" in argv]), 2)
        raw = [argv for _, argv, _ in ctx.commands if "-s" in argv and "otool" in argv]
        self.assertEqual(len(raw), 4)
        self.assertEqual({tuple(argv[argv.index("-s") + 1:argv.index("-s") + 3]) for argv in raw},
                         {("__DATA_CONST", "__objc_nlcatlist"), ("__DATA", "__objc_nlclslist")})
        self.assertTrue(all(timeout <= ios.ARTIFACT_INVENTORY_SECONDS for name, _, timeout in ctx.commands
                            if "objc-raw" in name))
        self.assertEqual(len([argv for _, argv, _ in ctx.commands if "mobile" in argv]), 2)
        self.assertEqual(ctx.stages["inventory"]["status"], "incomplete")
        self.assertTrue(ctx.failures)

    def test_known_objc_disk_inventory_does_not_certify_native_or_app_recovery(self):
        ctx = self.context()
        with patch.dict(os.environ, {"GITHUB_ACTIONS": "true"}), patch.object(ios, "objc_inventory", return_value={
            "scope": "on-disk-objc-method-records", "denominator_known": True, "status": "known",
            "method_count": 8, "methods": [], "issues": [], "evidence_commands": [],
        }):
            ios.run_ios(ctx)
        inventories = [value for name, value in ctx.documents.items() if "artifact-" in name and "inventory" in name]
        self.assertEqual(len(inventories), 2)
        for row in inventories:
            self.assertTrue(row["objc_denominator_known"])
            self.assertFalse(row["native_denominator_known"])
            self.assertFalse(row["swift_denominator_known"])
            self.assertEqual(row["status"], "incomplete")
        self.assertEqual(ctx.stages["recovery"]["status"], "incomplete")
        self.assertTrue(ctx.failures)

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
        self.assertEqual(rows[1]["timeout_policy"]["status"], "unallocated")
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
                    self.assertEqual(argv[argv.index("-toolchain") + 1], "XcodeDefault")
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


class IOSRecoveryBudgetTests(unittest.TestCase):
    """Exercise the real receipt writer with mocked process and clock boundaries."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.clock = [100.0]
        self.enterContext(patch.object(ios.time, "monotonic", side_effect=lambda: self.clock[0]))

    def context(self, remaining):
        ctx = common.CaseContext(
            app={"source_commit": "a" * 40},
            variant={"id": "owned-ios-release", "app": "owned", "platform": "ios",
                     "toolchain": "XcodeDefault"},
            source=self.source, work=self.root / "evidence", neverd=self.root / "neverd",
            timeout=4200, consumer_commit="b" * 40, manifest_sha256="c" * 64)
        ctx.deadline = self.clock[0] + remaining
        session = ios.Session(ctx)
        item = {"path": "Owned", "resolved": str(self.source / "Owned")}
        return ctx, session, item

    def invoke(self, ctx, session, item, process, *, handoff_delay=0, advance_wait=False):
        original_command = session.command
        original_wait = process.wait

        def delayed_command(*args, **kwargs):
            self.clock[0] += handoff_delay
            return original_command(*args, **kwargs)

        def bounded_wait(timeout=None):
            if advance_wait and not process.killed:
                self.clock[0] += timeout
            return original_wait(timeout=timeout)

        with patch.object(session, "command", side_effect=delayed_command), \
                patch.object(common.subprocess, "Popen", side_effect=process.start), \
                patch.object(common.os, "killpg", side_effect=process.killpg, create=True), \
                patch.object(process, "wait", side_effect=bounded_wait):
            return ios.artifact_recovery(session, item, 0, "arm64")

    def test_full_native_budget_has_separate_nominal_outer_allowance(self):
        ctx, session, item = self.context(4200)
        record = self.invoke(ctx, session, item, ProcessDouble(code=1))
        command = ctx.result["commands"][0]
        self.assertIn("--timeout=180", command["argv"])
        self.assertEqual(command["timeout_seconds"], 195)
        self.assertEqual(record["timeout_policy"], {
            "status": "planned", "native_timeout_seconds": 180,
            "planned_outer_timeout_seconds": 195,
            "nominal_cleanup_allowance_seconds": 15,
            "minimum_session_timeout_seconds": 181,
        })
        self.assertEqual(record["status"], "failed")

    def test_fractional_case_budget_bounds_both_timeouts(self):
        ctx, session, item = self.context(40.9)
        deadline = ctx.deadline
        record = self.invoke(ctx, session, item, ProcessDouble(code=1))
        command = ctx.result["commands"][0]
        self.assertIn("--timeout=25", command["argv"])
        self.assertEqual(command["timeout_seconds"], 40)
        self.assertEqual(record["timeout_policy"]["planned_outer_timeout_seconds"], 40)
        self.assertEqual(ctx.deadline, deadline)
        self.assertLessEqual(command["timeout_seconds"], deadline - 100.0)

    def test_insufficient_native_and_nominal_cleanup_budget_never_spawns(self):
        ctx, session, item = self.context(15.999)
        record = self.invoke(ctx, session, item, ProcessDouble())
        self.assertEqual(ctx.result["commands"], [])
        self.assertEqual(record["status"], "failed")
        self.assertEqual(record["timeout_policy"]["status"], "unallocated")
        self.assertIn("insufficient recovery and cleanup budget", record["reason"])

    def test_minimum_one_second_native_budget_is_not_rounded_up_past_case(self):
        ctx, session, item = self.context(16)
        record = self.invoke(ctx, session, item, ProcessDouble(code=1))
        command = ctx.result["commands"][0]
        self.assertIn("--timeout=1", command["argv"])
        self.assertEqual(command["timeout_seconds"], 16)
        self.assertEqual(record["timeout_policy"]["minimum_session_timeout_seconds"], 2)

    def test_handoff_budget_reduction_keeps_policy_nominal_and_receipt_actual(self):
        ctx, session, item = self.context(40.9)
        record = self.invoke(ctx, session, item, ProcessDouble(code=1), handoff_delay=14.5)
        command = ctx.result["commands"][0]
        self.assertIn("--timeout=25", command["argv"])
        self.assertAlmostEqual(command["timeout_seconds"], 26.4)
        self.assertEqual(record["timeout_policy"]["planned_outer_timeout_seconds"], 40)
        self.assertEqual(record["timeout_policy"]["status"], "planned")
        self.assertGreaterEqual(command["timeout_seconds"], 26)
        self.assertLess(command["timeout_seconds"], 40)

    def test_handoff_without_one_second_allowance_fails_before_common_launch(self):
        ctx, session, item = self.context(40.9)
        record = self.invoke(ctx, session, item, ProcessDouble(), handoff_delay=15.1)
        self.assertEqual(ctx.result["commands"], [])
        self.assertEqual(record["status"], "failed")
        self.assertEqual(record["timeout_policy"]["native_timeout_seconds"], 25)
        self.assertIn("insufficient recovery handoff budget", record["reason"])

    def test_common_final_case_clamp_is_authoritative_after_session_handoff(self):
        ctx, session, item = self.context(40.9)
        deadline = ctx.deadline
        original_command = ctx.command

        def delayed_common_command(*args, **kwargs):
            self.clock[0] += 16
            return original_command(*args, **kwargs)

        with patch.object(ctx, "command", side_effect=delayed_common_command):
            record = self.invoke(ctx, session, item, ProcessDouble(code=1))
        command = ctx.result["commands"][0]
        self.assertAlmostEqual(command["timeout_seconds"], 24.9)
        self.assertLess(command["timeout_seconds"], record["timeout_policy"]["minimum_session_timeout_seconds"])
        self.assertEqual(record["timeout_policy"]["planned_outer_timeout_seconds"], 40)
        self.assertEqual(record["timeout_policy"]["status"], "planned")
        self.assertEqual(record["status"], "failed")
        self.assertEqual(ctx.deadline, deadline)
        self.assertLessEqual(command["timeout_seconds"], deadline - self.clock[0])

    def test_native_budget_failure_retains_command_receipt_and_stderr(self):
        ctx, session, item = self.context(4200)
        process = ProcessDouble(code=1, stderr=b"mobile analysis exceeded its time budget\n")
        record = self.invoke(ctx, session, item, process)
        command = ctx.result["commands"][0]
        self.assertEqual(record["status"], "failed")
        self.assertEqual(record["returncode"], 1)
        self.assertEqual(command["status"], "failed")
        self.assertEqual(command["exitcode"], 1)
        self.assertEqual((ctx.work / command["stderr"]).read_bytes(), process.stderr_bytes)
        self.assertEqual(json.loads((ctx.work / "commands" / (command["id"] + ".json")).read_text()), command)

    def test_outer_hard_timeout_remains_failed_with_raw_evidence(self):
        ctx, session, item = self.context(20)
        process = ProcessDouble(running=True, stderr=b"native diagnostic before hard timeout\n")
        record = self.invoke(ctx, session, item, process, advance_wait=True)
        command = ctx.result["commands"][0]
        self.assertEqual(record["status"], "failed")
        self.assertIn("timeout", record["reason"])
        self.assertEqual(record["timeout_policy"]["native_timeout_seconds"], 5)
        self.assertEqual(command["timeout_seconds"], 20)
        self.assertEqual(command["status"], "timeout")
        self.assertEqual(command["exitcode"], -9)
        self.assertTrue(process.killed)
        self.assertEqual((ctx.work / command["stderr"]).read_bytes(), process.stderr_bytes)
        self.assertEqual(json.loads((ctx.work / "commands" / (command["id"] + ".json")).read_text()), command)


if __name__ == "__main__":
    unittest.main()
