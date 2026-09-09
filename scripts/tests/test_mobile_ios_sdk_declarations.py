"""Guards for real SDK namespace evidence; these do not qualify recovery."""
import copy
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest import mock

from scripts import collect_mobile_ios_sdk_declarations as collector
from scripts.collect_mobile_ios_sdk_declarations import declarations, macro_identifiers


class SDKDeclarationEvidenceTests(unittest.TestCase):
    def test_macros_preserve_object_and_function_names_without_evaluating_values(self):
        output = ("#define __APPLE__ 1\n#define __OBJC__ 1\n"
                  "#define SDK_EMPTY\n#define SDK_VALUE (1 + 2)\n"
                  "#define SDK_FUNCTION(x, y) ((x) + \\\n(y))\n")
        self.assertEqual(macro_identifiers(output),
                         ["SDK_EMPTY", "SDK_FUNCTION", "SDK_VALUE", "__APPLE__", "__OBJC__"])

    def test_missing_target_duplicate_unknown_or_truncated_macro_evidence_fails(self):
        target = "#define __APPLE__ 1\n#define __OBJC__ 1\n"
        for text in ("", "#define __APPLE__ 1\n", target + "#define __OBJC__ 1\n",
                     target + "unrecognized text\n", target + "#define 123BAD 1\n",
                     target + "#define SDK_VALUE \\", target + "\\",
                     target + "#define SDK\\u1234 1\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                macro_identifiers(text)

    def test_interface_names_are_not_all_subclass_evidence(self):
        ast = {"kind": "TranslationUnitDecl", "inner": [
            {"kind": "ObjCInterfaceDecl", "name": "RequiredSystemClass"},
            {"kind": "ObjCInterfaceDecl", "name": "ForwardOnly"},
            {"kind": "TypedefDecl", "name": "SDKValue"},
            {"kind": "FunctionDecl", "name": "SDKFunction"},
            {"kind": "EnumDecl", "name": "SeparateTag", "inner": [
                {"kind": "EnumConstantDecl", "name": "SDKEnumerator"}]},
            {"kind": "ObjCProtocolDecl", "name": "SeparateProtocol"}]}
        result = declarations(ast, "RequiredSystemClass")
        self.assertEqual(result["interface_declarations"], ["ForwardOnly", "RequiredSystemClass"])
        self.assertEqual(result["ordinary_identifiers"],
                         ["ForwardOnly", "RequiredSystemClass", "SDKEnumerator", "SDKFunction", "SDKValue"])
        self.assertNotIn("subclass_declaration_probe", result)
        with self.assertRaises(ValueError):
            declarations(ast, "MissingClass")


class SDKCollectionIntegrationTests(unittest.TestCase):
    # These expectations are independent of the collector's prefix generator.
    BASE = ("#include <stdint.h>\n#include <stdbool.h>\n"
            "#import <Foundation/Foundation.h>\n")
    TARGETS = {"iphoneos": "arm64-apple-ios18.0",
               "iphonesimulator": "arm64-apple-ios18.0-simulator"}
    PROFILES = {
        "foundation": ((), ()),
        "messages": (("Messages/Messages.h",), ("MSStickerBrowserViewController",)),
        "user-notifications": (("UserNotifications/UserNotifications.h",),
                               ("UNNotificationServiceExtension",)),
        "messages-user-notifications": (
            ("Messages/Messages.h", "UserNotifications/UserNotifications.h"),
            ("MSStickerBrowserViewController", "UNNotificationServiceExtension")),
    }
    LEGACY = {"Messages/Messages.h": "MSStickerBrowserViewController",
              "UserNotifications/UserNotifications.h": "UNNotificationServiceExtension"}

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.clang = self.root / "toolchain" / "clang"
        self.clang.parent.mkdir()
        self.clang.write_bytes(b"mock compiler identity, never executed\n")
        self.sdk_roots = {}
        for sdk in self.TARGETS:
            root = self.root / (sdk + "26.5.sdk")
            root.mkdir()
            (root / "SDKSettings.json").write_text(json.dumps({"sdk": sdk, "version": "26.5"}))
            self.sdk_roots[sdk] = root
        self.now = 0.0
        self.calls = []
        self.versions = dict.fromkeys(self.TARGETS, "26.5")
        self.failure = lambda call: None
        self.after_command = lambda call: None
        self.missing_ast_parent = None
        self.output = self.root / "evidence"

    @staticmethod
    def sha(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def prefix(self, profile_id):
        headers, _ = self.PROFILES[profile_id]
        return self.BASE + "".join(f"#import <{header}>\n" for header in headers)

    def source_identity(self, source):
        prefix, delimiter, declaration = source.partition("@interface ")
        parent = None
        if delimiter:
            match = re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]* : ([A-Za-z_][A-Za-z0-9_]*)\n@end\n",
                                 declaration)
            self.assertIsNotNone(match, "unexpected probe source declaration")
            parent = match.group(1)
        for profile_id in self.PROFILES:
            if prefix == self.prefix(profile_id):
                return profile_id, prefix, parent
        for header in self.LEGACY:
            if prefix == f"#import <{header}>\n":
                return "legacy-" + header.split("/")[0], prefix, parent
        self.fail("mock received an unrecognized or reordered source prefix: " + repr(prefix))

    @staticmethod
    def marker(profile_id):
        if profile_id == "messages-user-notifications":
            return "SDKCombinedOnly"
        return "SDK_" + profile_id.replace("-", "_") + "_Only"

    def run_command(self, argv, *, cwd, stdout, stderr, env, timeout, check):
        args = [str(value) for value in argv]
        self.assertEqual(Path(cwd), self.output)
        self.assertFalse(check)
        self.assertGreater(timeout, 0)
        self.assertLessEqual(timeout, 120)
        for variable in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
            self.assertNotIn(variable, env)
        call = {"argv": args, "timeout": timeout, "stem": Path(stdout.name).stem}
        if args[0] == "xcrun":
            self.assertEqual(args[1:3], ["--toolchain", "XcodeDefault"])
            sdk = args[args.index("--sdk") + 1]
            self.assertIn(sdk, self.TARGETS)
            call.update(kind="identity", sdk=sdk)
            if args[-1] == "--show-sdk-version":
                text = self.versions[sdk] + "\n"
            elif args[-1] == "--show-sdk-path":
                text = str(self.sdk_roots[sdk]) + "\n"
            else:
                self.assertEqual(args[-2:], ["--find", "clang"])
                text = str(self.clang) + "\n"
        elif args[-1] == "--version":
            self.assertEqual(args[0], str(self.clang))
            call["kind"] = "identity"
            text = "Apple clang version 21.0.0 (mock identity)\n"
        else:
            self.assertEqual(args[0], str(self.clang))
            self.assertEqual(args[args.index("-x") + 1], "objective-c")
            for flag in ("-std=gnu11", "-fobjc-arc", "-fno-modules"):
                self.assertIn(flag, args)
            target = args[args.index("-target") + 1]
            sdk = next((sdk for sdk, expected in self.TARGETS.items() if target == expected), None)
            self.assertIsNotNone(sdk, "unexpected target triple")
            self.assertEqual(args[args.index("-isysroot") + 1], str(self.sdk_roots[sdk]))
            source = Path(args[-1]).read_text()
            profile_id, prefix, parent = self.source_identity(source)
            call.update(sdk=sdk, profile_id=profile_id, prefix=prefix,
                        source=args[-1], superclass=parent)
            if "-ast-dump=json" in args:
                self.assertIn("-fsyntax-only", args)
                self.assertIsNone(parent)
                call["kind"] = "ast"
                classes = ["NSObject", "SDKForwardOnly"]
                classes += [name for header, name in self.LEGACY.items() if header in prefix]
                if self.missing_ast_parent and self.missing_ast_parent[0] == profile_id:
                    classes.remove(self.missing_ast_parent[1])
                rows = [{"kind": "ObjCInterfaceDecl", "name": name} for name in classes]
                rows += [{"kind": "FunctionDecl", "name": self.marker(profile_id)},
                         {"kind": "TypedefDecl", "name": "SDKValue"}]
                text = json.dumps({"kind": "TranslationUnitDecl", "inner": rows})
            elif "-dM" in args:
                self.assertIn("-E", args)
                self.assertNotIn("-fsyntax-only", args)
                self.assertIsNone(parent)
                call["kind"] = "macros"
                text = ("#define __APPLE__ 1\n#define __OBJC__ 1\n"
                        f"#define TARGET_OS_SIMULATOR {int(sdk == 'iphonesimulator')}\n"
                        f"#define {self.marker(profile_id)} 1\n"
                        "#define SDK_FUNCTION(x) (x)\n")
            else:
                self.assertIn("-fsyntax-only", args)
                self.assertIsNotNone(parent)
                call["kind"] = "probe"
                text = ""
        self.calls.append(call)
        failure = self.failure(call)
        if failure == "timeout":
            stdout.write(b"partial stdout before timeout\n")
            stderr.write(b"partial stderr before timeout\n")
            raise subprocess.TimeoutExpired(args, timeout)
        stdout.write(text.encode())
        if failure:
            stderr.write(b"mock rejected superclass declaration\n")
        self.after_command(call)
        return subprocess.CompletedProcess(args, failure or 0)

    def collect(self):
        environment = {"GITHUB_ACTIONS": "true", "CONSUMER_COMMIT": "a" * 40,
                       "CPATH": "/ignored/include", "C_INCLUDE_PATH": "/ignored/include",
                       "CPLUS_INCLUDE_PATH": "/ignored/include", "OBJC_INCLUDE_PATH": "/ignored/include"}
        with mock.patch.dict(collector.os.environ, environment, clear=True), \
                mock.patch.object(collector.sys, "platform", "darwin"), \
                mock.patch.object(collector.time, "monotonic", side_effect=lambda: self.now), \
                mock.patch.object(collector.subprocess, "run", side_effect=self.run_command):
            collector.collect(self.output, "26.5")
        return self.report()

    def report(self):
        return json.loads((self.output / "sdk-declarations.json").read_text())

    def test_collect_preserves_legacy_records_and_independent_complete_source_profiles(self):
        report = self.collect()
        self.assertEqual(report["status"], "success")
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(report["scope"], "sdk-declarations-only")
        self.assertEqual(report["consumer_commit"], "a" * 40)
        self.assertEqual(report["imports"], list(self.LEGACY))
        expected_ids = {sdk + "/" + profile for sdk in self.TARGETS for profile in self.PROFILES}
        self.assertEqual(set(report["expected_source_profile_ids"]), expected_ids)
        self.assertEqual(len(report["expected_source_profile_ids"]), 8)
        self.assertEqual({(row["sdk"], row["header"]) for row in report["sdks"]},
                         {(sdk, header) for sdk in self.TARGETS for header in self.LEGACY})
        self.assertEqual(len(report["sdks"]), 4)
        evidence_paths = set()
        for row in report["sdks"]:
            stem = row["sdk"] + "-" + row["header"].split("/")[0]
            self.assertEqual(row["inventory"], stem + "-declarations.json")
            path = self.output / row["inventory"]
            self.assertEqual(row["sha256"], self.sha(path))
            inventory = json.loads(path.read_text())
            self.assertEqual(inventory["header"], row["header"])
            self.assertEqual(inventory["target"], self.TARGETS[row["sdk"]])
            self.assertEqual(inventory["macro_evidence"]["path"], stem + "-macros.stdout")
            for suffix in ("ast", "macros", "subclass"):
                self.assertTrue(any(call["stem"] == stem + "-" + suffix for call in self.calls))
            self.assertEqual((self.output / (stem + ".m")).read_text(), f"#import <{row['header']}>\n")
            probe = inventory["subclass_declaration_probe"]
            self.assertEqual(probe["superclass"], self.LEGACY[row["header"]])
            self.assertEqual(probe["source"], stem + "-subclass.m")
            self.assertEqual(probe["status"], "success")
            self.assertIs(probe["instance_layout_verified"], False)
            evidence_paths.add(path.name)
        self.assertEqual(len(report["source_profiles"]), 8)
        self.assertEqual({row["sdk"] + "/" + row["profile_id"] for row in report["source_profiles"]}, expected_ids)
        for row in report["source_profiles"]:
            profile = row["profile_id"]
            stem = row["sdk"] + "-source-" + profile
            extras, parents = self.PROFILES[profile]
            self.assertEqual(row["kind"], "objc-source-prefix")
            self.assertEqual(row["status"], "success")
            self.assertEqual(row["version"], "26.5")
            self.assertEqual(row["target"], self.TARGETS[row["sdk"]])
            self.assertEqual(row["headers"], ["stdint.h", "stdbool.h", "Foundation/Foundation.h", *extras])
            self.assertEqual(row["required_superclasses"], list(parents))
            self.assertEqual(row["prefix"]["path"], stem + ".m")
            self.assertEqual((self.output / row["prefix"]["path"]).read_text(), self.prefix(profile))
            self.assertEqual(row["prefix"]["sha256"], self.sha(self.output / row["prefix"]["path"]))
            self.assertEqual(row["inventory"]["path"], stem + "-declarations.json")
            for field in ("prefix", "inventory", "ast_evidence", "macro_evidence"):
                evidence = row[field]
                self.assertNotIn(evidence["path"], evidence_paths)
                evidence_paths.add(evidence["path"])
                self.assertEqual(evidence["sha256"], self.sha(self.output / evidence["path"]))
            self.assertEqual(row["ast_evidence"]["path"], stem + "-ast.stdout")
            self.assertEqual(row["macro_evidence"]["path"], stem + "-macros.stdout")
            self.assertEqual(row["macro_evidence"]["scope"], "preprocessor-identifiers-only")
            self.assertEqual(row["clang_sha256"], self.sha(self.clang))
            self.assertEqual(row["sdk_settings_sha256"], self.sha(self.sdk_roots[row["sdk"]] / "SDKSettings.json"))
            inventory = json.loads((self.output / row["inventory"]["path"]).read_text())
            self.assertIn(self.marker(profile), inventory["ordinary_identifiers"])
            self.assertIn(self.marker(profile), inventory["macro_identifiers"])
            if profile == "messages-user-notifications":
                for isolated in ("messages", "user-notifications"):
                    self.assertNotIn(self.marker(isolated), inventory["ordinary_identifiers"])
                    self.assertNotIn(self.marker(isolated), inventory["macro_identifiers"])
            self.assertEqual({probe["superclass"] for probe in row["subclass_probes"]}, set(parents))
            self.assertEqual(len(row["subclass_probes"]), len(parents))
            for probe in row["subclass_probes"]:
                self.assertEqual(probe["source"], stem + "-subclass-" + probe["superclass"] + ".m")
                source = self.output / probe["source"]
                self.assertTrue(source.read_text().startswith(self.prefix(profile) + "@interface "))
                self.assertEqual(probe["sha256"], self.sha(source))
                self.assertEqual(probe["prefix_sha256"], row["prefix"]["sha256"])
                self.assertEqual(probe["status"], "success")
                self.assertIs(probe["instance_layout_verified"], False)
        for kind in ("ast", "macros", "probe"):
            self.assertEqual(sum(call["kind"] == kind for call in self.calls), 12, kind)
        collector.check_source_profiles(report["source_profiles"], "26.5")

    def test_combined_second_probe_failure_preserves_first_and_later_incomplete(self):
        self.failure = lambda call: 1 if (call.get("sdk") == "iphoneos"
            and call.get("profile_id") == "messages-user-notifications"
            and call.get("superclass") == "UNNotificationServiceExtension") else None
        with self.assertRaisesRegex(RuntimeError, "exited 1"):
            self.collect()
        report = self.report()
        self.assertEqual(report["status"], "failed")
        row = next(row for row in report["source_profiles"] if row["sdk"] == "iphoneos"
                   and row["profile_id"] == "messages-user-notifications")
        self.assertEqual(row["status"], "failed")
        first = next(probe for probe in row["subclass_probes"]
                     if probe["superclass"] == "MSStickerBrowserViewController")
        self.assertEqual(first["status"], "success")
        self.assertTrue(all(row["status"] == "incomplete" for row in report["source_profiles"]
                            if row["sdk"] == "iphonesimulator"))
        command = report["commands"][-1]
        self.assertEqual(command["exitcode"], 1)
        self.assertIn("exited 1", command["error"])
        for stream in ("stdout", "stderr"):
            self.assertEqual(command[stream + "_sha256"], self.sha(self.output / command[stream]))
        self.assertIn("mock rejected superclass", (self.output / command["stderr"]).read_text())

    def test_missing_required_ast_parent_cannot_reach_macro_or_probe_success(self):
        self.missing_ast_parent = ("messages-user-notifications", "UNNotificationServiceExtension")
        with self.assertRaisesRegex(ValueError, "requested system superclass"):
            self.collect()
        report = self.report()
        self.assertEqual(report["status"], "failed")
        row = next(row for row in report["source_profiles"] if row["sdk"] == "iphoneos"
                   and row["profile_id"] == "messages-user-notifications")
        self.assertEqual(row["status"], "failed")
        self.assertEqual(row["subclass_probes"], [])
        self.assertEqual(self.calls[-1]["kind"], "ast")

    def test_wrong_sdk_version_stops_before_compiler_and_keeps_all_profiles_incomplete(self):
        self.versions["iphoneos"] = "26.4"
        with self.assertRaisesRegex(RuntimeError, "SDK version differs"):
            self.collect()
        report = self.report()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(len(self.calls), 1)
        self.assertTrue(all(row["status"] == "incomplete" for row in report["source_profiles"]))

    def timeout_on_source_macros(self, call):
        if call.get("sdk") == "iphoneos" and call.get("profile_id") == "foundation" and call["kind"] == "macros":
            return "timeout"
        return None

    def test_subprocess_timeout_preserves_partial_output_hashes_and_null_exit(self):
        self.failure = self.timeout_on_source_macros
        with self.assertRaises(subprocess.TimeoutExpired):
            self.collect()
        report = self.report()
        self.assertEqual(report["status"], "failed")
        command = report["commands"][-1]
        self.assertIsNone(command["exitcode"])
        self.assertIn("timed out", command["error"])
        for stream in ("stdout", "stderr"):
            path = self.output / command[stream]
            self.assertIn("partial " + stream, path.read_text())
            self.assertEqual(command[stream + "_sha256"], self.sha(path))
        row = next(row for row in report["source_profiles"] if row["sdk"] == "iphoneos"
                   and row["profile_id"] == "foundation")
        self.assertEqual(row["status"], "failed")
        self.assertEqual(self.calls[-1]["kind"], "macros")

    def test_digest_failure_during_timeout_preserves_the_original_error(self):
        self.failure = self.timeout_on_source_macros
        original = collector.digest

        def fail_partial_stdout(path, *args, **kwargs):
            if path.name == "iphoneos-source-foundation-macros.stdout":
                raise OSError("mock evidence hashing failed")
            return original(path, *args, **kwargs)

        with mock.patch.object(collector, "digest", side_effect=fail_partial_stdout), \
                self.assertRaises(subprocess.TimeoutExpired):
            self.collect()
        report = self.report()
        self.assertEqual(report["status"], "failed")
        self.assertIn("timed out", report["error"])
        command = report["commands"][-1]
        self.assertIsNone(command["exitcode"])
        self.assertIn("timed out", command["error"])
        self.assertIn("mock evidence hashing failed", str(command["evidence_errors"]))
        self.assertEqual(command["stderr_sha256"], self.sha(self.output / command["stderr"]))

    def test_deadline_after_successful_command_cannot_publish_success(self):
        def expire_after_ast(call):
            if call.get("profile_id") == "foundation" and call["kind"] == "ast":
                self.now = 1201.0

        self.after_command = expire_after_ast
        with self.assertRaisesRegex(RuntimeError, "deadline"):
            self.collect()
        report = self.report()
        self.assertEqual(report["status"], "failed")
        self.assertIn("deadline", report["error"])
        command = report["commands"][-1]
        self.assertEqual(command["exitcode"], 0)
        self.assertIn("deadline", str(command["evidence_errors"]))
        self.assertTrue((self.output / command["stdout"]).is_file())
        self.assertEqual(self.calls[-1]["kind"], "ast")

    def test_last_second_command_timeout_is_clipped_to_remaining_total_budget(self):
        def approach_deadline(call):
            if call.get("profile_id") == "foundation" and call["kind"] == "ast":
                self.now = 1199.0

        self.after_command = approach_deadline
        self.failure = self.timeout_on_source_macros
        with self.assertRaises(subprocess.TimeoutExpired):
            self.collect()
        self.assertEqual(self.calls[-1]["timeout"], 1.0)
        self.assertEqual(self.report()["status"], "failed")

    def test_deadline_exhausted_before_first_command_preserves_preregistered_profiles(self):
        original = collector.CollectionDeadline.remaining

        def expire_before_first_check(deadline):
            self.now = 1200.0
            return original(deadline)

        with mock.patch.object(collector.CollectionDeadline, "remaining", expire_before_first_check), \
                self.assertRaisesRegex(RuntimeError, "deadline"):
            self.collect()
        report = self.report()
        self.assertEqual(report["status"], "failed")
        self.assertEqual(self.calls, [])
        self.assertEqual(len(report["source_profiles"]), 8)
        self.assertTrue(all(row["status"] == "incomplete" for row in report["source_profiles"]))

    def test_complete_profile_gate_rejects_missing_duplicate_and_mismatched_evidence(self):
        records = self.collect()["source_profiles"]
        collector.check_source_profiles(records, "26.5")
        for mutation in ("missing", "duplicate", "unknown", "incomplete", "wrong-target", "wrong-sdk",
                         "reordered-headers", "wrong-prefix-path", "missing-ast", "wrong-macro-path",
                         "missing-probe", "duplicate-probe", "wrong-probe-prefix", "layout-claim"):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(records)
                combined = next(row for row in changed if row["sdk"] == "iphoneos"
                                and row["profile_id"] == "messages-user-notifications")
                if mutation == "missing":
                    changed.pop()
                elif mutation == "duplicate":
                    changed[-1] = copy.deepcopy(changed[0])
                elif mutation == "unknown":
                    changed[0]["profile_id"] = "unqualified-framework"
                elif mutation == "incomplete":
                    changed[0]["status"] = "incomplete"
                elif mutation == "wrong-target":
                    changed[0]["target"] = "arm64-apple-ios18.0-simulator"
                elif mutation == "wrong-sdk":
                    changed[0]["version"] = "26.4"
                elif mutation == "reordered-headers":
                    combined["headers"][-2:] = list(reversed(combined["headers"][-2:]))
                elif mutation == "wrong-prefix-path":
                    changed[0]["prefix"]["path"] = "iphoneos-Messages.m"
                elif mutation == "missing-ast":
                    changed[0].pop("ast_evidence")
                elif mutation == "wrong-macro-path":
                    changed[0]["macro_evidence"]["path"] = changed[1]["macro_evidence"]["path"]
                elif mutation == "missing-probe":
                    combined["subclass_probes"].pop()
                elif mutation == "duplicate-probe":
                    combined["subclass_probes"][1] = copy.deepcopy(combined["subclass_probes"][0])
                elif mutation == "wrong-probe-prefix":
                    combined["subclass_probes"][0]["prefix_sha256"] = "0" * 64
                else:
                    combined["subclass_probes"][0]["instance_layout_verified"] = True
                with self.assertRaises(ValueError):
                    collector.check_source_profiles(changed, "26.5")


if __name__ == "__main__":
    unittest.main()
