"""CI unit guards for the independent Android real-application evidence path.

The text below is a self-authored SDK-format fixture. Live SDK stdout is also
mutated inside run_android; no sample here is claimed to be a real app result.
"""
from __future__ import annotations

import copy
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from scripts.mobile_real_apps_android import (
    AndroidEvidenceError, STAGES, combine_inventory, compare_recovery,
    dex_header, extract_dex_inputs, inventory_mutation_checks, parse_dexdump,
    run_android, source_artifacts,
)


HEADER = {"version": "035", "file_size": 112, "class_defs_size": 2, "method_ids_size": 19}
SDK_TEXT = """Processing 'owned.dex'...
Opened 'owned.dex', DEX version '035'
DEX file header:
file_size           : 112
class_defs_size      : 2
method_ids_size     : 19
Class #0 header:
class_idx           : 0
static_fields_size  : 1
instance_fields_size: 0
direct_methods_size : 3
virtual_methods_size: 1
Class #0            -
  Class descriptor  : 'Lowned/Example;'
  Access flags      : 0x0401 (PUBLIC ABSTRACT)
  Superclass        : 'Ljava/lang/Object;'
  Interfaces        -
  Static fields     -
    #0              : (in Lowned/Example;)
      name          : 'notAMethod'
      type          : 'I'
      access        : 0x0009 (PUBLIC STATIC)
      value         : 9
  Instance fields   -
  Direct methods    -
    #0              : (in Lowned/Example;)
      name          : '<init>'
      type          : '()V'
      access        : 0x10001 (PUBLIC CONSTRUCTOR)
      method_idx    : 3
      code          -
      registers     : 1
      ins           : 1
      outs          : 1
      insns size    : 4 16-bit code units
      catches       : (none)
      positions     :
      locals        :
    #1              : (in Lowned/Example;)
      name          : '<clinit>'
      type          : '()V'
      access        : 0x10008 (STATIC CONSTRUCTOR)
      code          -
      registers     : 0
      ins           : 0
      outs          : 0
      insns size    : 1 16-bit code units
      catches       : (none)
      positions     :
      locals        :
    #2              : (in Lowned/Example;)
      name          : 'nativeValue'
      type          : '()I'
      access        : 0x0109 (PUBLIC STATIC NATIVE)
      code          : (none)
  Virtual methods   -
    #0              : (in Lowned/Example;)
      name          : 'abstractValue'
      type          : '([Ljava/lang/String;)V'
      access        : 0x0401 (PUBLIC ABSTRACT)
      code          : (none)
  source_file_idx   : 1 (Example.java)

Class #1 header:
class_idx           : 1
static_fields_size  : 0
instance_fields_size: 0
direct_methods_size : 0
virtual_methods_size: 0
Class #1            -
  Class descriptor  : 'Lowned/Marker;'
  Access flags      : 0x0601 (PUBLIC INTERFACE ABSTRACT)
  Superclass        : 'Ljava/lang/Object;'
  Interfaces        -
  Static fields     -
  Instance fields   -
  Direct methods    -
  Virtual methods   -
  source_file_idx   : 2 (Marker.java)
"""


def inventory():
    return combine_inventory([parse_dexdump(SDK_TEXT, "classes.dex", HEADER)])


def reports(original=None):
    original = original or inventory()
    rows = []
    for item in original["methods"].values():
        body = item["role"] == "body"
        row = {key: item[key] for key in ("identity", "class", "name", "prototype", "input")}
        row.update(status="recovered" if body else "declaration-only", instruction_count=1 if body else 0)
        if not body:
            row["reason"] = "Original abstract/native declaration"
        rows.append(row)
    coverage = {"schema_version": 1, "status": "recovered", "methods": rows,
                "class_count": len(original["classes"]), "method_count": len(rows),
                "recovered_method_count": original["body_count"],
                "declaration_only_method_count": original["abstract_count"] + original["native_count"],
                "unrecovered_method_count": 0}
    report = {"schema_version": 1, "status": "success", "platform": "android", "input_kind": "apk",
              "backend": {"name": "neverd", "version": "1", "execution": "builtin"},
              "android_method_recovery": coverage, "input_code_files": original["inputs"],
              "dex_count": len(original["inputs"]), "smali_count": 0,
              "java_sources": ["sources/owned/Example.java"], "java_source_count": 1}
    return report, coverage


def dex_bytes():
    # Only fixed header facts are needed by our ZIP evidence helper; these
    # bytes are deliberately not presented as an executable/verified DEX.
    data = bytearray(112)
    data[:8] = b"dex\n035\x00"
    struct.pack_into("<III", data, 32, 112, 112, 0x12345678)
    struct.pack_into("<I", data, 88, 19)
    struct.pack_into("<I", data, 96, 2)
    return bytes(data)


def apk_bytes(entries=None):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as archive:
        for name, data in entries or [("classes.dex", dex_bytes())]:
            archive.writestr(name, data)
    return output.getvalue()


class SDKInventoryTests(unittest.TestCase):
    def test_definitions_not_references_and_initializer_identity(self):
        result = inventory()
        self.assertEqual((result["body_count"], result["native_count"], result["abstract_count"]), (2, 1, 1))
        self.assertEqual(len(result["classes"]), 2)
        self.assertEqual(len(result["methods"]), 4)  # Header has 19 references.
        self.assertIn("Lowned/Example;-><init>()V", result["methods"])
        self.assertIn("Lowned/Example;-><clinit>()V", result["methods"])
        self.assertFalse(any("notAMethod" in identity for identity in result["methods"]))
        self.assertEqual(result["methods"]["Lowned/Example;->nativeValue()I"]["access_flags"], 0x109)

    def test_live_output_mutation_suite_checks_six_independent_changes(self):
        checks = inventory_mutation_checks(SDK_TEXT, "classes.dex", HEADER)
        self.assertEqual(set(checks), {"changed-file-class-count", "changed-class-method-count",
                                     "missing-method-name", "duplicate-method-name",
                                     "truncated-last-class", "changed-body-presence"})

    def test_mutation_guard_fails_if_parser_accepts_corrupted_output(self):
        with patch("scripts.mobile_real_apps_android.parse_dexdump", return_value={}):
            with self.assertRaisesRegex(AndroidEvidenceError, "accepted mutation"):
                inventory_mutation_checks(SDK_TEXT, "classes.dex", HEADER)

    def test_truncation_omission_duplicates_and_roles_are_rejected(self):
        changes = {
            "no-file-header": SDK_TEXT.replace("class_defs_size      : 2\n", ""),
            "reference-count-disagreement": SDK_TEXT.replace("method_ids_size     : 19", "method_ids_size     : 18"),
            "no-section-count": SDK_TEXT.replace("direct_methods_size : 3\n", ""),
            "no-name": SDK_TEXT.replace("      name          : '<init>'\n", ""),
            "duplicate-name": SDK_TEXT.replace("      name          : '<init>'", "      name : '<init>'\n      name : '<init>'"),
            "no-prototype": SDK_TEXT.replace("      type          : '()V'\n", "", 1),
            "bad-prototype": SDK_TEXT.replace("'()V'", "'([V)V'", 1),
            "no-access": SDK_TEXT.replace("      access        : 0x10001 (PUBLIC CONSTRUCTOR)\n", ""),
            "no-code": SDK_TEXT.replace("      code          -\n", "", 1),
            "no-code-size": SDK_TEXT.replace("      insns size    : 4 16-bit code units\n", ""),
            "wrong-body-role": SDK_TEXT.replace("0x10001 (PUBLIC CONSTRUCTOR)", "0x0101 (PUBLIC NATIVE)"),
            "wrong-declaration-role": SDK_TEXT.replace("0x0109 (PUBLIC STATIC NATIVE)", "0x0009 (PUBLIC STATIC)"),
            "both-declaration-flags": SDK_TEXT.replace("0x0109 (PUBLIC STATIC NATIVE)", "0x0509 (PUBLIC STATIC NATIVE ABSTRACT)"),
            "wrong-owner": SDK_TEXT.replace("(in Lowned/Example;)", "(in Lowned/Other;)", 2),
            "missing-method-ordinal": SDK_TEXT.replace("    #1              : (in Lowned/Example;)", "    #4              : (in Lowned/Example;)"),
            "duplicate-class-ordinal": SDK_TEXT.replace("Class #1 header:", "Class #0 header:"),
            "duplicate-class": SDK_TEXT.replace("'Lowned/Marker;'", "'Lowned/Example;'"),
            "missing-class": SDK_TEXT[:SDK_TEXT.index("Class #1 header:")],
            "missing-footer": SDK_TEXT[:SDK_TEXT.rindex("  source_file_idx")],
            "removed-section": SDK_TEXT.replace("  Virtual methods   -\n", "", 1),
        }
        for name, text in changes.items():
            with self.subTest(name=name), self.assertRaises(AndroidEvidenceError):
                parse_dexdump(text, "classes.dex", HEADER)

    def test_multidex_keeps_every_class_and_input(self):
        first = parse_dexdump(SDK_TEXT, "classes.dex", HEADER)
        second = parse_dexdump(SDK_TEXT.replace("Lowned/", "Lsecond/"), "classes2.dex", HEADER)
        combined = combine_inventory([first, second])
        self.assertEqual(len(combined["methods"]), 8)
        self.assertEqual(combined["inputs"], ["classes.dex", "classes2.dex"])
        self.assertEqual(combined["methods"]["Lsecond/Example;-><clinit>()V"]["input"], "classes2.dex")
        with self.assertRaisesRegex(AndroidEvidenceError, "Duplicate class"):
            combine_inventory([first, {**first, "input": "classes2.dex"}])


class RecoveryClaimTests(unittest.TestCase):
    def test_exact_complete_report_is_accepted_only_for_inventory(self):
        report, coverage = reports()
        counts = compare_recovery(report, coverage, inventory())
        self.assertEqual(counts["recovered_method_count"], 2)
        self.assertEqual(counts["declaration_only_method_count"], 2)

    def test_report_tampering_cannot_drop_or_reclassify_methods(self):
        def missing(report, coverage): coverage["methods"].pop()
        def duplicate(report, coverage): coverage["methods"].append(copy.deepcopy(coverage["methods"][0]))
        def wrong_role(report, coverage): coverage["methods"][0]["status"] = "declaration-only"
        def wrong_input(report, coverage): coverage["methods"][0]["input"] = "classes2.dex"
        def wrong_identity(report, coverage): coverage["methods"][0]["name"] = "replacement"
        def bool_count(report, coverage): coverage["methods"][0]["instruction_count"] = True
        def code_overclaim(report, coverage): coverage["methods"][0]["instruction_count"] = 999
        def no_body(report, coverage): coverage["methods"][0]["instruction_count"] = 0
        def declaration_body(report, coverage): coverage["methods"][-1]["instruction_count"] = 1
        def missing_reason(report, coverage): coverage["methods"][-1].pop("reason")
        def aggregate(report, coverage): coverage["method_count"] = 3
        def aggregate_bool(report, coverage): coverage["class_count"] = True
        def alternate_backend(report, coverage): report["backend"]["name"] = "jadx"
        def missing_dex(report, coverage): report["input_code_files"] = []
        def partial(report, coverage): coverage["status"] = "partial"
        def report_mismatch(report, coverage): report["android_method_recovery"] = {}
        for mutate in (missing, duplicate, wrong_role, wrong_input, wrong_identity, bool_count,
                       code_overclaim, no_body, declaration_body, missing_reason, aggregate,
                       aggregate_bool, alternate_backend, missing_dex, partial, report_mismatch):
            report, coverage = reports()
            mutate(report, coverage)
            with self.subTest(mutation=mutate.__name__), self.assertRaises(AndroidEvidenceError):
                compare_recovery(report, coverage, inventory())

    def test_source_inventory_rejects_empty_and_unlisted_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "sources/owned/Example.java"
            path.parent.mkdir(parents=True)
            path.write_text("class Example {}\n")
            report, _ = reports()
            self.assertEqual(len(source_artifacts(report, root)), 1)
            path.write_text("")
            with self.assertRaisesRegex(AndroidEvidenceError, "Empty"):
                source_artifacts(report, root)
            path.write_text("class Example {}\n")
            extra = path.with_name("Extra.java")
            extra.write_text("class Extra {}\n")
            with self.assertRaisesRegex(AndroidEvidenceError, "disagree"):
                source_artifacts(report, root)

    def test_source_artifact_budgets_apply_before_hashing(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "sources/owned/Example.java"
            path.parent.mkdir(parents=True)
            path.write_text("class Example {}\n")
            report, _ = reports()
            for name, value, expected in (("MAX_SOURCE_ENTRIES", 0, "count"),
                                          ("MAX_SOURCE_BYTES", 1, "byte limit")):
                with self.subTest(limit=name), patch("scripts.mobile_real_apps_android." + name, value):
                    with self.assertRaisesRegex(AndroidEvidenceError, expected):
                        source_artifacts(report, root)


class APKInputTests(unittest.TestCase):
    def test_fixed_header_cross_check(self):
        self.assertEqual(dex_header(dex_bytes()), HEADER)
        for changed in (dex_bytes()[:-1], dex_bytes()[:4] + b"036" + dex_bytes()[7:],
                        dex_bytes()[:4] + b"041" + dex_bytes()[7:]):
            with self.assertRaises(AndroidEvidenceError): dex_header(changed)

    def test_nonstandard_dex_assets_are_not_silently_excluded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apk = root / "input.apk"
            apk.write_bytes(apk_bytes([("classes.dex", dex_bytes()), ("assets/plugin.dex", dex_bytes())]))
            result = extract_dex_inputs(apk, root / "dex")
            self.assertEqual({entry["input"] for entry in result}, {"classes.dex", "assets/plugin.dex"})
            self.assertTrue(all(entry["path"].parent == root / "dex" for entry in result))

    def test_archive_paths_and_duplicate_names_fail_before_extraction(self):
        for name in ("../classes.dex", "/classes.dex", "assets\\classes.dex", "C:/classes.dex", "assets//classes.dex"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                apk = root / "input.apk"
                apk.write_bytes(apk_bytes([(name, dex_bytes())]))
                with self.assertRaises(AndroidEvidenceError): extract_dex_inputs(apk, root / "dex")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apk = root / "input.apk"
            apk.write_bytes(apk_bytes([("classes.dex", dex_bytes()), ("CLASSES.dex", dex_bytes())]))
            with self.assertRaisesRegex(AndroidEvidenceError, "case-colliding"):
                extract_dex_inputs(apk, root / "dex")

    def test_zip_symlinks_are_rejected_without_following_them(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            apk = root / "input.apk"
            with zipfile.ZipFile(apk, "w") as archive:
                entry = zipfile.ZipInfo("classes.dex")
                entry.create_system = 3
                entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(entry, b"/outside")
            with self.assertRaisesRegex(AndroidEvidenceError, "symlink"):
                extract_dex_inputs(apk, root / "dex")


class FakeContext:
    """No processes or network: simulate common's logged CI command API."""
    def __init__(self, root):
        self.work, self.source = root / "evidence", root / "upstream"
        self.work.mkdir()
        self.source.mkdir()
        self.neverd, self.timeout = root / "neverd", 90
        self.apk = apk_bytes()
        self.app = {"repository": "https://github.com/owned/application", "source_commit": "a" * 40,
                    "license": "Apache-2.0", "official_apk": {"url": "https://github.com/owned/application/releases/download/1/app.apk",
                    "sha256": hashlib.sha256(self.apk).hexdigest()}, "android": {"build_tools": "35.0.0"}}
        self.variant = {"profile": "official-release"}
        self.stages, self.failures, self.commands = {}, [], []
        self.cli_exit, self.sdk_text = 0, SDK_TEXT
        tool = root / "sdk/build-tools/35.0.0/dexdump"
        tool.parent.mkdir(parents=True)
        tool.write_bytes(b"self-owned simulated SDK executable")
        self.sdk_root = root / "sdk"

    def stage(self, name, status, **details): self.stages[name] = {"status": status, **details}
    def fail(self, reason): self.failures.append(reason)
    def write_json(self, name, data): (self.work / name).write_text(json.dumps(data))

    def command(self, name, argv, **kwargs):
        self.commands.append((name, argv, kwargs))
        stdout, code = "", 0
        if name == "android-source-head": stdout = self.app["source_commit"] + "\n"
        elif name == "android-download-official-apk": (self.work / "official.apk").write_bytes(self.apk)
        elif name.startswith("android-dexdump-"): stdout = self.sdk_text
        elif name == "android-neverd-mobile":
            code = self.cli_exit
            if not code:
                report, coverage = reports()
                output = self.work / "recovered"
                (output / "metadata").mkdir(parents=True)
                (output / "sources/owned").mkdir(parents=True)
                (output / "report.json").write_text(json.dumps(report))
                (output / "metadata/android-methods.json").write_text(json.dumps(coverage))
                (output / "sources/owned/Example.java").write_text("class Example {}\n")
                stdout = json.dumps(report)
            else:
                stdout = '{"status":"error","error":"Unsupported source body"}'
        return subprocess.CompletedProcess(argv, code, stdout, "")


class WorkflowStageTests(unittest.TestCase):
    def context(self, root):
        ctx = FakeContext(root)
        # Fake command handling is portable; the platform-specific executable
        # name must still match production's SDK lookup.
        if os.name == "nt":
            source = ctx.sdk_root / "build-tools/35.0.0/dexdump"
            source.rename(source.with_suffix(".exe"))
        return ctx

    def test_even_complete_inventory_leaves_rebuild_and_behavior_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = self.context(Path(temporary))
            with patch.dict(os.environ, {"ANDROID_SDK_ROOT": str(ctx.sdk_root)}): run_android(ctx)
            self.assertEqual(set(ctx.stages), set(STAGES))
            self.assertTrue(all(ctx.stages[name]["status"] == "success" for name in STAGES[:4]), ctx.stages)
            for name in STAGES[:4]:
                self.assertTrue(ctx.stages[name]["evidence"])
                self.assertTrue(all((ctx.work / item).is_file() for item in ctx.stages[name]["evidence"]))
            self.assertEqual(ctx.stages["recompile"]["status"], "incomplete")
            self.assertEqual(ctx.stages["behavior"]["status"], "incomplete")
            self.assertTrue(ctx.failures)
            self.assertFalse(ctx.stages["original_build"]["source_build"])
            self.assertTrue((ctx.work / "android-dex-mutation-checks-0000.json").is_file())
            dump = next(argv for name, argv, _ in ctx.commands if name.startswith("android-dexdump-"))
            self.assertEqual(dump[1:5], ["-f", "-h", "-l", "plain"])
            self.assertFalse(any(flag in dump for flag in ("-e", "-i", "-j")))

    def test_real_rejection_keeps_independent_inventory_and_never_becomes_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = self.context(Path(temporary))
            ctx.cli_exit = 1
            with patch.dict(os.environ, {"ANDROID_SDK_ROOT": str(ctx.sdk_root)}): run_android(ctx)
            self.assertEqual(ctx.stages["inventory"]["status"], "success")
            self.assertEqual(ctx.stages["recovery"]["status"], "failed")
            self.assertEqual(ctx.stages["behavior"]["status"], "incomplete")
            self.assertTrue((ctx.work / "android-inventory.json").is_file())
            self.assertTrue((ctx.work / "android-recovery-failure.json").is_file())
            self.assertFalse((ctx.work / "recovered").exists())
            self.assertTrue(ctx.failures)

    def test_checksum_failure_does_not_run_inventory_or_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = self.context(Path(temporary))
            ctx.app["official_apk"]["sha256"] = "0" * 64
            run_android(ctx)
            self.assertEqual(ctx.stages["original_build"]["status"], "failed")
            self.assertEqual(ctx.stages["inventory"]["status"], "incomplete")
            self.assertEqual(len(ctx.commands), 2)
            self.assertFalse(any(name == "android-neverd-mobile" for name, _, _ in ctx.commands))
            self.assertTrue((ctx.work / "official.apk").is_file())

    def test_failed_independent_inventory_still_analyzes_verified_complete_apk(self):
        for failure in ("sdk-format", "missing-sdk"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                ctx = self.context(Path(temporary))
                sdk_root = ctx.sdk_root
                if failure == "sdk-format":
                    ctx.sdk_text = "SDK output format is not understood\n"
                else:
                    sdk_root = ctx.sdk_root / "missing"
                with patch.dict(os.environ, {"ANDROID_SDK_ROOT": str(sdk_root)}):
                    run_android(ctx)
                self.assertEqual(set(ctx.stages), set(STAGES))
                self.assertEqual(ctx.stages["original_build"]["status"], "success")
                self.assertEqual(ctx.stages["inventory"]["status"], "failed")
                self.assertEqual(ctx.stages["recovery"]["status"], "incomplete")
                self.assertFalse(ctx.stages["recovery"]["independent_denominator_known"])
                self.assertEqual(ctx.stages["recompile"]["status"], "incomplete")
                self.assertEqual(ctx.stages["behavior"]["status"], "incomplete")
                calls = [argv for name, argv, _ in ctx.commands if name == "android-neverd-mobile"]
                self.assertEqual(len(calls), 1)
                self.assertEqual(calls[0][1:3], ["mobile", str(ctx.work / "official.apk")])
                self.assertTrue((ctx.work / "android-inventory-failure.json").is_file())
                self.assertTrue((ctx.work / "recovered/report.json").is_file())
                self.assertTrue((ctx.work / "recovered/metadata/android-methods.json").is_file())
                self.assertTrue(ctx.failures)

    def test_inventory_failure_and_native_rejection_both_remain_visible(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = self.context(Path(temporary))
            ctx.sdk_text = "unrecognized SDK output\n"
            ctx.cli_exit = 1
            with patch.dict(os.environ, {"ANDROID_SDK_ROOT": str(ctx.sdk_root)}):
                run_android(ctx)
            self.assertEqual(ctx.stages["inventory"]["status"], "failed")
            self.assertEqual(ctx.stages["recovery"]["status"], "failed")
            self.assertEqual(ctx.stages["behavior"]["status"], "incomplete")
            failure = json.loads((ctx.work / "android-recovery-failure.json").read_text())
            self.assertFalse(failure["independent_denominator_known"])
            self.assertIsNone(failure["expected_method_count"])
            self.assertFalse((ctx.work / "recovered").exists())
            self.assertTrue(any("inventory" in reason for reason in ctx.failures))
            self.assertTrue(any("recovery" in reason for reason in ctx.failures))

    def test_source_profiles_are_explicitly_incomplete(self):
        with tempfile.TemporaryDirectory() as temporary:
            ctx = self.context(Path(temporary))
            ctx.variant["profile"] = "gradle-release"
            run_android(ctx)
            self.assertTrue(all(item["status"] == "incomplete" for item in ctx.stages.values()))
            self.assertFalse(ctx.commands)
            self.assertTrue(ctx.failures)


if __name__ == "__main__":
    unittest.main()
