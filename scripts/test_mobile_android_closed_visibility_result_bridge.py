#!/usr/bin/env python3
"""Verify erased visibility entries for a closed superclass result."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


MAIN_CLASS = "probe.ClosedVisibilityResultBridgeHarness"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + option, type=Path, required=True)
    for option in ("d8", "work-dir"):
        parser.add_argument("--" + option, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-visibility-result-bridge-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "closed_visibility_result_bridge/java"
        originals, harness = work / "originals", work / "harness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / ("ClosedVisibilityResultBridge" + suffix + ".java")
                                for suffix in ("Caller", "Harness")], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 12):
            raise RuntimeError("Owned closed visibility result bridge inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Base;", "Lfixture/Input;", "Lfixture/Value;", "Lfixture/Root;"}
        excluded = providers if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {method for method in all_methods if method.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                entry = owner[1:-1] + ".class"
                jar.writestr(entry, (originals / entry).read_bytes())
        dependency_hash = sha(dependency)

        def execute(runtime, label):
            outputs = []
            for route in range(7):
                for mode in range(12):
                    for future in (False, True):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), MAIN_CLASS,
                                           str(route), str(mode), str(future).lower()],
                                          f"{label}-{route}-{mode}-{future}")
                        if text != "attempts=2,closed-visibility-result-bridge=matched\n":
                            raise RuntimeError("Frozen closed visibility result bridge observer changed")
                        outputs.append(text)
            return outputs

        def bridge_code(root, label):
            javap = Path(verify.java).with_name("javap.exe" if os.name == "nt" else "javap")
            dump = verify.run([javap, "-classpath", root, "-c", "-s", "-p", "fixture.Child"], label)
            descriptor = "()Lfixture/Input;"
            header = re.search(r"(?m)^  public fixture\.Input first\(\);\n    descriptor: " + re.escape(descriptor) + r"\n", dump)
            if not header:
                raise RuntimeError("Original visibility result bridge header changed")
            instructions = []
            for line in dump[header.end():].splitlines():
                match = re.fullmatch(r"\s*([0-9]+):\s+(.*)", line)
                if match:
                    instructions.append(" ".join(re.sub(r"#\d+", "#", match.group(2)).split()))
                elif instructions:
                    break
            expected = ["aload_0", "invokespecial # // Method fixture/Base.first:" + descriptor, "areturn"]
            if instructions != expected:
                raise RuntimeError("Original erased bridge forwarding bytecodes changed: " + repr(instructions))
            return {"descriptor": descriptor, "instructions": instructions, "javap_sha256": hashlib.sha256(dump.encode()).hexdigest()}

        original_bridge = bridge_code(originals, "original-bridge-code")
        (work / "original-bridge-code.json").open("x").write(json.dumps(original_bridge, indent=2) + "\n")
        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        wrong = [
            ("wrong-receiver", "Base.java", "Trace.receiver = this;", "Trace.receiver = null;", 0, 0),
            ("lost-snapshot", "Base.java", "Trace.before = result;", "Trace.before = null;", 0, 0),
            ("lost-action", "Base.java", "Trace.action.run();", ";", 0, 0),
            ("duplicate-action", "Base.java", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", 0, 0),
            ("late-result-read", "Base.java", "return result;", "return value;", 0, 8),
            ("wrong-result", "Base.java", "return result;", "return null;", 0, 0),
            ("lost-completion", "Base.java", "Trace.hit(4);", ";", 0, 0),
            ("wrapped-checked", "Base.java", "Trace.action.run();", "try { Trace.action.run(); } catch(Throwable failure) { throw new IllegalStateException(failure); }", 0, 4),
            ("early-result-check", "Base.java", "Trace.action.run();", "Value checked = (Value) (Object) result; Trace.action.run();", 2, 9),
            ("invented-result-check", "Base.java", "return result;", "Value checked = (Value) (Object) result; return result;", 2, 2),
            ("explicit-result-override", "Child.java", "public Child() { super(); }", "public Child() { super(); } public Value first() { return (Value) super.first(); }", 0, 0),
            ("lost-final", "Child.java", "public final class Child", "public class Child", 0, 0),
            ("wrong-value", "Value.java", "return Trace.answer;", "return 0;", 0, 0),
            ("lost-value-action", "Value.java", "Trace.valueAction.run();", ";", 0, 11),
        ]
        for label, file_name, before, replacement, route, mode in wrong:
            text = (fixture / "fixture" / file_name).read_text()
            if text.count(before) != 1:
                raise RuntimeError("Owned wrong-program anchor changed: " + label)
            changed = work / label / file_name
            changed.parent.mkdir()
            changed.open("x").write(text.replace(before, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([changed], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     MAIN_CLASS, str(route), str(mode), "true"], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed visibility result bridge mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)
        mutations = [
            ("raw-parent", "Lfixture/Child;", None, "Lfixture/Base<Lfixture/Value;>;", "Lfixture/Base;"),
            ("wrong-parent-element", "Lfixture/Child;", None, "Lfixture/Base<Lfixture/Value;>;", "Lfixture/Base<Lfixture/Input;>;"),
            ("fixed-parent-result", "Lfixture/Base;", "Lfixture/Base;->first()Lfixture/Input;", "()TP;", "()Lfixture/Input;"),
        ]
        receipts = {}
        for label, owner, member, signature, changed_signature in mutations:
            control = work / label
            shutil.copytree(originals, control)
            changed = control / (owner[1:-1] + ".class")
            code_before = method_code(changed.read_bytes())
            receipts[label] = change_signature(changed, signature.encode(), changed_signature.encode())
            wanted = copy.deepcopy(facts)
            (wanted[owner]["methods"][member] if member else wanted[owner])["signature"] = changed_signature
            if compiler_facts(control) != wanted or method_code(changed.read_bytes()) != code_before:
                raise RuntimeError("Signature control changed unrelated metadata or Code")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (control, harness))),
                                     MAIN_CLASS, "0", "0", "true"], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed visibility result bridge mismatch" not in result.stderr:
                raise RuntimeError("Observer missed unchanged-Code Signature control: " + label)
        (work / "signature-controls.json").open("x").write(json.dumps(receipts, indent=2) + "\n")

        classpath = ["--classpath", sdk] + (["--classpath", dependency] if excluded else [])
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dexdir = work / "dex"
        dexdir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        if list(dexdir.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned closed visibility result bridge DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original closed visibility result bridge declaration")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        bridges = {"Lfixture/Child;->first()Lfixture/Input;"}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 1 or projected != bridges
                or coverage["status"] != "partial" or coverage["recovered_method_count"] != len(methods) - 1
                or coverage.get("declaration_only_method_count", 0)
                or any(row["status"] != "recovered" for identity, row in rows.items() if identity not in bridges)
                or any(rows[identity].get("projection_kind") != "generated-public-superclass-access-bridge" for identity in bridges)
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Visibility result bridge recovery changed original identities or contracts")
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if excluded else None, release=release)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        allowed_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed_helpers:
            raise RuntimeError("Closed visibility result bridge recovery added or omitted an original runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for helper in helpers:
            wanted = {"name": "__neverdThrow", "prototype": "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;",
                      "access": 0xA, "code": True,
                      "signature": "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;",
                      "deprecated_attribute": False, "runtime_visible_annotations": [],
                      "runtime_invisible_annotations": [], "runtime_invisible_parameter_annotations": [], "method_parameters": None}
            if after[helper.split("->", 1)[0]]["methods"][helper] != wanted or after_exceptions[helper] != ["Ljava/lang/Throwable;"]:
                raise RuntimeError("Generated throw helper changed its exact admitted contract")
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {method: row for method, row in actual["methods"].items() if method not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Closed visibility result bridge fixture compiler release changed")
            if wanted != actual or any(exceptions[method] != after_exceptions[method] for method in facts[owner]["methods"]):
                raise RuntimeError("Original class, Code roles, access, Signature, Exceptions or parameters changed: " + owner)

        witness = {"status": "verified-original-and-rebuilt-bridge", "identities": sorted(bridges),
                   "original": original_bridge,
                   "rebuilt": bridge_code(rebuilt, "rebuilt-bridge-code")}
        (work / "disclosed-metadata.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Closed visibility result bridge recovery changed input evaluation, dispatch, partial writes or exact failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "unchanged_code_signature_controls": len(mutations),
                   "all_original_class_method_roles_and_metadata": "matched",
                   "erased_result_bridge_caller_checks_and_failure_identity": "matched",
                   "dependency_provider": args.dependency_provider, "minimum_java_release": release}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
