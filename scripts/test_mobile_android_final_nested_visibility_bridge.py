#!/usr/bin/env python3
"""Verify final nested visibility bridges across known enclosing type scopes."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


MAIN_CLASS = "probe.FinalNestedVisibilityBridgeHarness"
DESCRIPTORS = {"apply": "(Lfixture/Payload;)V", "wide": "(J)J"}
BRIDGES = {"Lfixture/Owner$Child;->" + name + descriptor for name, descriptor in DESCRIPTORS.items()}


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-nested-visibility-bridge-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "final_nested_visibility_bridge/java"
        originals, harness = work / "originals", work / "harness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / ("FinalNestedVisibilityBridge" + suffix + ".java")
                                for suffix in ("Caller", "Harness")], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 14):
            raise RuntimeError("Owned final nested visibility bridge inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Base;", "Lfixture/Payload;", "Lfixture/Family;", "Lfixture/Family$Hidden;"}
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
            for route in range(4):
                for operation in range(2):
                    for kind in range(3):
                        for mode in range(8):
                            text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), MAIN_CLASS,
                                               str(route), str(operation), str(kind), str(mode)],
                                              f"{label}-{route}-{operation}-{kind}-{mode}")
                            if text != "attempts=2,final-nested-visibility-bridge=matched\n":
                                raise RuntimeError("Frozen final nested visibility bridge observer changed")
                            outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        apply_tail = "Trace.action.run(); Trace.hit(4);\n    }"
        wide_tail = "Trace.action.run(); Trace.hit(4); return Trace.result;"
        wrong = [
            ("lost-action", "Base.java", apply_tail, "Trace.hit(4);\n    }", 0, 0),
            ("repeated-action", "Base.java", apply_tail, "Trace.action.run(); Trace.action.run(); Trace.hit(4);\n    }", 0, 0),
            ("wrong-receiver", "Base.java", "Trace.receiver = this; Trace.argument = input;", "Trace.receiver = new Owner.Child(); Trace.argument = input;", 0, 0),
            ("wrong-argument", "Base.java", "Trace.argument = input;", "Trace.argument = null;", 0, 0),
            ("lost-argument-store", "Base.java", "Trace.argument = input;", ";", 0, 0),
            ("lost-completion", "Base.java", apply_tail, "Trace.action.run();\n    }", 0, 0),
            ("wrapped-checked", "Base.java", apply_tail, "try { Trace.action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); } Trace.hit(4);\n    }", 0, 2),
            ("narrowed-wide-argument", "Base.java", "Trace.numberArgument = input;", "Trace.numberArgument = (int) input;", 1, 0),
            ("early-wide-result", "Base.java", wide_tail, "long saved = Trace.result; Trace.action.run(); Trace.hit(4); return saved;", 1, 0),
            ("narrowed-wide-result", "Base.java", "return Trace.result;", "return (int) Trace.result;", 1, 0),
            ("wrapped-error", "Base.java", wide_tail, "try { Trace.action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); } Trace.hit(4); return Trace.result;", 1, 3),
            ("explicit-nonbridge-entry", "Owner.java", "extends Family.Hidden { }", "extends Family.Hidden { public void apply(Payload input) { super.apply(input); } }", 0, 0),
            ("extra-runtime-entry", "Owner.java", "extends Family.Hidden { }", "extends Family.Hidden { public int extra() { return 0; } }", 0, 0),
            ("invalid-helper-contract", "Owner.java", "extends Family.Hidden { }", "extends Family.Hidden { private static RuntimeException __neverdThrow(Throwable failure) { return new RuntimeException(failure); } }", 0, 0),
            ("lost-final-role", "Owner.java", "public static final class Child", "public static class Child", 0, 0),
            ("changed-parent-visibility", "Family.java", "protected static class Hidden", "public static class Hidden", 0, 0),
            ("lost-enclosing-substitution", "Owner.java", "extends Family<Owner>", "extends Family", 0, 0),
            ("lost-recursive-bound", "Family.java", "Family<B extends Family<B>>", "Family<B>", 0, 0),
        ]
        for label, filename, anchor, replacement, operation, mode in wrong:
            text = (fixture / "fixture" / filename).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Final nested visibility counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     MAIN_CLASS, "0", str(operation), "0", str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned final nested visibility bridge mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)

        classpath = ["--classpath", sdk] + (["--classpath", dependency] if excluded else [])
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dexdir = work / "dex"
        dexdir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        if list(dexdir.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned visibility bridge DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original visibility bridge declaration")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 2 or coverage["status"] != "partial"
                or coverage["recovered_method_count"] != len(methods) - 2
                or coverage.get("declaration_only_method_count", 0)
                or any(row["status"] != ("source-projected" if identity in BRIDGES else "recovered")
                       for identity, row in rows.items())
                or any(rows[identity].get("projection_kind") != "generated-public-superclass-access-bridge"
                       or rows[identity].get("bridge_dispatch") != identity for identity in BRIDGES)
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Final nested visibility recovery changed original identities or bridge contracts")
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if excluded else None, release=release)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        allowed_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed_helpers:
            raise RuntimeError("Visibility bridge recovery added or omitted an original runtime entry")
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
                raise RuntimeError("Visibility bridge fixture compiler release changed")
            if wanted != actual or any(exceptions[method] != after_exceptions[method] for method in facts[owner]["methods"]):
                raise RuntimeError("Original class, Code roles, access, Signature, Exceptions or parameters changed: " + owner)

        def bridge_code(root, label):
            javap = Path(verify.java).with_name("javap.exe" if os.name == "nt" else "javap")
            text = verify.run([javap, "-classpath", root, "-p", "-c", "-s", "fixture.Owner$Child"], label)
            result = {}
            for method_name, descriptor in DESCRIPTORS.items():
                header = re.search(r"(?m)^  public .*\b" + method_name + r"\([^\n]*\);\n    descriptor: " + re.escape(descriptor) + r"\n", text)
                if not header:
                    raise RuntimeError("Missing compiled original bridge header: " + method_name)
                instructions = []
                for line in text[header.end():].splitlines():
                    match = re.fullmatch(r"\s*[0-9]+:\s+(.*)", line)
                    if match:
                        instructions.append(" ".join(re.sub(r"#\d+", "#", match[1]).split()))
                    elif instructions:
                        break
                expected = ["aload_0", "aload_1" if method_name == "apply" else "lload_1",
                            "invokespecial # // Method fixture/Family$Hidden." + method_name + ":" + descriptor,
                            "return" if method_name == "apply" else "lreturn"]
                if instructions != expected:
                    raise RuntimeError("Original exact visibility forwarding instructions changed: " + method_name + ": " + repr(instructions))
                result[method_name] = instructions
            return {"instructions": result, "javap_sha256": hashlib.sha256(text.encode()).hexdigest()}

        witness = {"status": "all-original-class-method-facts-and-bridge-code-matched", "identities": sorted(BRIDGES),
                   "original": bridge_code(originals, "original-bridge-code"), "rebuilt": bridge_code(rebuilt, "rebuilt-bridge-code")}
        (work / "disclosed-metadata.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Visibility bridge recovery changed input evaluation, dispatch, words, state or exact failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "all_original_class_method_roles_and_metadata": "unchanged", "normalized_original_bridge_code": "matched",
                   "dependency_provider": args.dependency_provider, "minimum_java_release": release}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
