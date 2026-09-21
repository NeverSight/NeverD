"""Evidence acquisition guards; mocked compilers do not qualify a runtime ABI."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import collect_mobile_swift_string_abi as collector


SYMBOL = "$ss27_stringCompareWithSmolCheck__9expectingSbs11_StringGutsV_ADs01_G16ComparisonResultOtF"
TARGETS = {"iphoneos": "arm64-apple-ios18.0",
           "iphonesimulator": "arm64-apple-ios18.0-simulator"}


def ir(language, target="arm64-apple-ios18.0"):
    name = SYMBOL if language == "swift" else r"\01_" + SYMBOL
    attrs = "" if language == "swift" else " noundef"
    types = ("i64", "ptr", "i64", "ptr", "i8")
    result = f'target triple = "{target}"\n'
    result += f'declare swiftcc i1 @"{name}"(' + ", ".join(t + attrs for t in types) + ") #1\n"
    for mode in (0, 1) if language == "swift" else (None,):
        args = [t + attrs + " %" + str(i) for i, t in enumerate(types)]
        if mode is not None:
            args[-1] = "i8 " + str(mode)
        result += f'  %5 = tail call swiftcc i1 @"{name}"(' + ", ".join(args) + ")\n"
    if language == "c":
        result += "  %6 = zext i1 %5 to i8\n  ret i8 %6\n"
    return result


class SwiftStringIRTests(unittest.TestCase):
    def test_actual_calls_preserve_i1_and_all_five_arguments(self):
        for language in ("swift", "c"):
            with self.subTest(language=language):
                result = collector.validate_ir(ir(language), language, TARGETS["iphoneos"])
                self.assertEqual(result["return"], "i1")
                self.assertEqual(result["parameters"], ["i64", "ptr", "i64", "ptr", "i8"])
                self.assertEqual(result["call_count"], 2 if language == "swift" else 1)
        self.assertEqual(collector.validate_ir(ir("swift"), "swift")["modes"], ["0", "1"])

    def test_wrong_return_hidden_arguments_and_wrong_symbols_are_rejected(self):
        base = ir("swift")
        changes = [
            base.replace("swiftcc i1", "swiftcc i8"),
            base.replace("swiftcc", "ccc"),
            base.replace("i8)", "i8, ptr swiftself)"),
            base.replace("i64, ptr, i64, ptr, i8", "i64, ptr, i64, ptr, i64"),
            base.replace(SYMBOL, SYMBOL + "Other"),
            base + base.splitlines()[1] + "\n",
            base.replace("tail call", "invoke"),
            base.replace("i8 1", "i8 0"),
            base.replace("i8 1", "ptr %extra, i8 1"),
            base.replace("i64 %0", "i64 undef"),
            base.replace("i8 1", "i8 2"),
        ]
        for index, text in enumerate(changes):
            with self.subTest(index=index), self.assertRaises(ValueError):
                collector.validate_ir(text, "swift")

    def test_c_result_must_be_the_actual_normalized_call_result(self):
        base = ir("c")
        for text in (base.replace("zext i1 %5", "zext i1 %other"),
                     base.replace("ret i8 %6", "ret i8 %other"),
                     base.replace("zext i1", "sext i1"),
                     base.replace("to i8", "to i32")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                collector.validate_ir(text, "c")

    def test_target_triple_is_independent_evidence(self):
        for target in ("arm64-apple-ios18.0", "arm64-apple-ios18.0.0"):
            collector.validate_ir(ir("c", target), "c", TARGETS["iphoneos"])
        for target in ("arm64-apple-ios18.0-simulator", "arm64-apple-macosx15.0",
                       "x86_64-apple-ios18.0-simulator"):
            with self.subTest(target=target), self.assertRaises(ValueError):
                collector.validate_ir(ir("c", target), "c", TARGETS["iphoneos"])
        with self.assertRaises(ValueError):
            collector.validate_ir(ir("c") + 'target triple = "arm64-apple-ios18.0"\n',
                                  "c", TARGETS["iphoneos"])


class SwiftStringCollectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.developer = self.root / "Xcode_26.5.app/Contents/Developer"
        self.developer.mkdir(parents=True)
        self.tools = {}
        for name in ("swiftc", "clang"):
            path = self.developer / "Toolchains/XcodeDefault.xctoolchain/usr/bin" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(("mock identity: " + name).encode())
            self.tools[name] = path
        self.sdks = {}
        for sdk in TARGETS:
            root = self.developer / "Platforms" / (sdk + ".sdk")
            (root / "usr/lib/swift").mkdir(parents=True)
            (root / "SDKSettings.json").write_text(json.dumps({"Version": "26.5", "CanonicalName": sdk}))
            (root / "usr/lib/swift/libswiftCore.tbd").write_text("fixed mock linker identity " + sdk)
            self.sdks[sdk] = root
        self.output = self.root / "evidence"
        self.versions = dict.fromkeys(TARGETS, "26.5")
        self.xcode = "Xcode 26.5\nBuild version 17F45\n"
        self.calls = []
        self.now = 0
        self.fail_command = None
        self.timeout_command = None
        self.bad_ir = False

    def run_command(self, argv, *, cwd, env, stdout, stderr, timeout, check):
        args = [str(value) for value in argv]
        self.calls.append(args)
        self.assertEqual(Path(cwd), self.output)
        self.assertFalse(check)
        self.assertTrue(0 < timeout <= 120)
        for key in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH",
                    "SDKROOT", "SWIFT_EXEC", "SWIFT_DRIVER_SWIFT_FRONTEND_EXEC"):
            self.assertNotIn(key, env)
        name = Path(stdout.name).stem
        if args == ["xcodebuild", "-version"]:
            text = self.xcode
        elif args[0] == "xcrun":
            self.assertEqual(args[1:3], ["--toolchain", "XcodeDefault"])
            sdk = args[args.index("--sdk") + 1]
            if args[-1] == "--show-sdk-version":
                text = self.versions[sdk]
            elif args[-1] == "--show-sdk-path":
                text = str(self.sdks[sdk])
            else:
                self.assertEqual(args[-2], "--find")
                text = str(self.tools[args[-1]])
        elif args[-1] == "--version":
            self.assertIn(args[0], [str(path) for path in self.tools.values()])
            text = "fixed mock compiler version\n"
        else:
            language = "swift" if args[0] == str(self.tools["swiftc"]) else "c"
            self.assertEqual(args[0], str(self.tools["swiftc" if language == "swift" else "clang"]))
            target = args[args.index("-target") + 1]
            sdk = next(key for key, value in TARGETS.items() if value == target)
            self.assertEqual(args[args.index("-sdk" if language == "swift" else "-isysroot") + 1],
                             str(self.sdks[sdk]))
            self.assertEqual(args[-2], "-o")
            source = Path(args[-3]).read_text()
            if language == "swift":
                self.assertIn("lhs == rhs", source)
                self.assertIn("lhs < rhs", source)
                self.assertIn("-O", args)
            else:
                self.assertIn("extern _Bool neverd_compare", source)
                self.assertIn("__attribute__((swiftcall))", source)
                self.assertIn("-Werror", args)
                self.assertIn("-std=gnu11", args)
            text = ""
            payload = ir(language, target) if args[-1].endswith(".ll") else "mock assembly\n"
            if self.bad_ir:
                payload = payload.replace("swiftcc i1", "swiftcc i8")
            Path(args[-1]).write_text(payload)
        stdout.write(text.encode())
        if name in (self.timeout_command, self.fail_command):
            stderr.write(b"compiler stopped; preserve partial outputs\n")
            if name == self.timeout_command:
                raise subprocess.TimeoutExpired(args, timeout)
            return subprocess.CompletedProcess(args, 7)
        return subprocess.CompletedProcess(args, 0)

    def collect(self, **overrides):
        environment = {"GITHUB_ACTIONS": "true", "CONSUMER_COMMIT": "a" * 40,
                       "DEVELOPER_DIR": str(self.developer), "CPATH": "/ignored",
                       "SDKROOT": "/ignored", "SWIFT_EXEC": "/ignored"}
        environment.update(overrides)
        with mock.patch.dict(collector.os.environ, environment, clear=True), \
                mock.patch.object(collector.sys, "platform", "darwin"), \
                mock.patch.object(collector.time, "monotonic", side_effect=lambda: self.now), \
                mock.patch.object(collector.subprocess, "run", side_effect=self.run_command):
            success = collector.collect(self.output)
        report = json.loads((self.output / "manifest.json").read_text())
        self.assertEqual(success, report["status"] == "complete")
        return report

    def test_complete_both_profiles_retains_exact_inputs_outputs_and_identity(self):
        report = self.collect()
        self.assertEqual(report["status"], "complete")
        self.assertEqual(report["consumer_commit"], "a" * 40)
        self.assertEqual(report["symbol"], SYMBOL)
        self.assertEqual(len(report["profiles"]), 2)
        for profile in report["profiles"]:
            self.assertEqual(profile["status"], "complete")
            self.assertEqual(profile["target"], TARGETS[profile["sdk"]])
            self.assertEqual(len(profile["files"]), 8)
            self.assertEqual(len(profile["tools"]), 2)
            self.assertEqual(profile["swift_abi"]["return"], "i1")
            self.assertEqual(profile["c_abi"]["return"], "i1")
        for entry in report["retained_files"]:
            data = (self.output / entry["path"]).read_bytes()
            self.assertEqual(entry["size"], len(data))
            self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
        self.assertTrue(all(command["exitcode"] == 0 for command in report["commands"]))

    def test_wrong_sdk_version_preserves_completed_device_profile(self):
        self.versions["iphonesimulator"] = "26.4"
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["profiles"][0]["status"], "complete")
        self.assertEqual(report["profiles"][1]["status"], "incomplete")
        self.assertIn("unexpected SDK", report["error"])

    def test_wrong_xcode_and_commit_fail_before_compilation(self):
        self.xcode = "Xcode 26.4\nBuild version old\n"
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(self.calls), 1)
        self.output = self.root / "bad-commit"
        self.calls.clear()
        report = self.collect(CONSUMER_COMMIT="dev")
        self.assertEqual(report["status"], "failed")
        self.assertEqual(self.calls, [])

    def test_compile_failure_and_timeout_preserve_partial_ir_and_logs(self):
        for attribute in ("fail_command", "timeout_command"):
            with self.subTest(attribute=attribute):
                self.output = self.root / attribute
                setattr(self, attribute, "iphoneos-swift-ir")
                report = self.collect()
                self.assertEqual(report["status"], "failed")
                command = report["commands"][-1]
                self.assertIn("error", command)
                self.assertTrue((self.output / "iphoneos-swift.ll").is_file())
                self.assertIn("compiler stopped", (self.output / command["stderr"]["path"]).read_text())
                self.assertIn("iphoneos-swift.ll", [entry["path"] for entry in report["retained_files"]])
                setattr(self, attribute, None)

    def test_wrong_ir_preserves_the_rejected_compiler_output(self):
        self.bad_ir = True
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertIn("swiftcc i8", (self.output / "iphoneos-swift.ll").read_text())
        self.assertNotIn("swift_abi", report["profiles"][0])

    def test_missing_empty_oversized_and_external_sdk_inputs_fail(self):
        source = self.sdks["iphoneos"] / "usr/lib/swift/libswiftCore.tbd"
        original = source.read_bytes()
        for kind in ("missing", "empty", "oversized", "external"):
            with self.subTest(kind=kind):
                self.output = self.root / kind
                source.unlink(missing_ok=True)
                if kind == "empty":
                    source.write_bytes(b"")
                elif kind == "oversized":
                    with source.open("wb") as stream:
                        stream.truncate(collector.MAX_BYTES + 1)
                elif kind == "external":
                    external = self.root / "outside.tbd"
                    external.write_bytes(original)
                    source.symlink_to(external)
                report = self.collect()
                self.assertEqual(report["status"], "failed")
                self.assertTrue((self.output / "iphoneos-SDKSettings.json").is_file())
        source.unlink(missing_ok=True)
        source.write_bytes(original)

    def test_external_tool_is_rejected_before_it_executes(self):
        external = self.root / "outside-swiftc"
        external.write_bytes(b"mock external compiler")
        self.tools["swiftc"] = external
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertIn("outside the selected Xcode", report["error"])
        self.assertFalse(any(args[0] == str(external) for args in self.calls))

    def test_external_sdk_is_rejected_before_reading_its_inputs(self):
        external = self.root / "outside.sdk"
        external.mkdir()
        self.sdks["iphoneos"] = external
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertIn("SDK is outside", report["error"])
        self.assertFalse((self.output / "iphoneos-SDKSettings.json").exists())

    def test_deadline_failure_still_writes_a_manifest(self):
        original = self.run_command

        def expire(*args, **kwargs):
            result = original(*args, **kwargs)
            self.now = 481
            return result

        self.run_command = expire
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertIn("deadline", report["error"])
        self.assertEqual(len(self.calls), 1)

    def test_oversized_log_cannot_prevent_failure_manifest_retention(self):
        original = self.run_command

        def oversized(*args, **kwargs):
            result = original(*args, **kwargs)
            kwargs["stderr"].truncate(collector.MAX_BYTES + 1)
            return result

        self.run_command = oversized
        report = self.collect()
        self.assertEqual(report["status"], "failed")
        self.assertIn("retention_errors", report)
        self.assertIn("retention_errors", report["commands"][0])
        self.assertEqual(len(self.calls), 1)

    def test_collection_does_not_overwrite_existing_directory(self):
        self.output.mkdir()
        marker = self.output / "marker"
        marker.write_bytes(b"keep")
        with self.assertRaises(FileExistsError):
            self.collect()
        self.assertEqual(marker.read_bytes(), b"keep")

    def test_local_execution_and_non_actions_collection_are_rejected(self):
        with mock.patch.dict(collector.os.environ, {}, clear=True), self.assertRaises(RuntimeError):
            collector.collect(self.output)
        with mock.patch.dict(collector.os.environ, {"GITHUB_ACTIONS": "true"}, clear=True), \
                mock.patch.object(collector.sys, "platform", "linux"), self.assertRaises(RuntimeError):
            collector.collect(self.output)
        self.assertFalse(self.output.exists())


class SwiftStringWorkflowTests(unittest.TestCase):
    def test_workflow_is_manual_read_only_and_keeps_failure_evidence(self):
        root = Path(__file__).resolve().parents[2]
        text = (root / ".github/workflows/mobile-swift-string-abi.yml").read_text()
        self.assertIn("on:\n  workflow_dispatch:", text)
        for event in ("  push:", "  pull_request:", "  schedule:"):
            self.assertNotIn(event, text)
        self.assertIn("permissions:\n  contents: read", text)
        self.assertIn("persist-credentials: false", text)
        self.assertIn("/Applications/Xcode_26.5.app/Contents/Developer", text)
        self.assertIn("CONSUMER_COMMIT: ${{ github.sha }}", text)
        self.assertIn("if: always()", text)
        self.assertIn("if-no-files-found: error", text)
        self.assertIn("retention-days: 7", text)
        self.assertIn("python -m unittest scripts.tests.test_mobile_swift_string_abi", text)
        self.assertIn("python scripts/collect_mobile_swift_string_abi.py", text)
        self.assertEqual(len(re.findall(r'uses: [^\n@]+@[0-9a-f]{40}\b', text)), 3)


if __name__ == "__main__":
    unittest.main()
