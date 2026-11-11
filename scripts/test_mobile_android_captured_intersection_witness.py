#!/usr/bin/env python3
"""Verify a captured member remains the original nominal intersection argument."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


MAIN_CLASS = "probe.CapturedIntersectionWitnessHarness"


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-intersection-witness-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "captured_intersection_witness/java"
        originals, harness = work / "originals", work / "harness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / ("CapturedIntersectionWitness" + suffix + ".java")
                                for suffix in ("Caller", "Harness")], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 14):
            raise RuntimeError("Owned captured intersection witness void inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        original_member = facts["Lfixture/Owner$Member;"]
        constructors = [row for identity, row in original_member["methods"].items() if "-><init>(" in identity]
        if len(constructors) != 1 or constructors[0]["signature"] is not None:
            raise RuntimeError("Owned captured member constructor must retain its complete raw descriptor")
        implicit_parameters = [{"name": None, "access": 0x1010}] + [{"name": None, "access": 0} for _ in range(3)]
        if constructors[0]["method_parameters"] != implicit_parameters:
            raise RuntimeError("Original private capture constructor implicit parameter flags changed")
        providers = {"Lfixture/Base;", "Lfixture/First;", "Lfixture/Second;", "Lfixture/Provider;"}
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
                for mode in range(12):
                    for future in (False, True):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), MAIN_CLASS,
                                           str(route), str(mode), str(future).lower(), str(label == "rebuilt").lower()], f"{label}-{route}-{mode}-{future}")
                        if text != "attempts=2,captured-intersection-witness=matched\n":
                            raise RuntimeError("Frozen captured intersection witness observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        wrong = [
            ("lost-capture", "Owner.java", "return Owner.this;", "return new Owner();", 0),
            ("lost-context-call", "Owner.java", "new Member<Object>(context(), index, values)", "new Member<Object>(Trace.context, index, values)", 0),
            ("wrong-context", "Owner.java", "new Member<Object>(context(), index, values)", "new Member<Object>(context(), index, null)", 0),
            ("wrong-index", "Owner.java", "new Member<Object>(context(), index, values)", "new Member<Object>(context(), 0, values)", 0),
            ("early-index", "Owner.java", "accept(new Member<Object>(context(), index, values));", "int saved = index; accept(new Member<Object>(context(), saved, values));", 10),
            ("lost-dispatch", "Owner.java", "accept(new Member<Object>(context(), index, values));", "new Provider().accept(new Member<Object>(context(), index, values));", 0),
            ("lost-completion", "Owner.java", "Trace.hit(8);", ";", 0),
            ("lost-construct", "Base.java", "Trace.construct.run();", ";", 0),
            ("lost-input-store", "Base.java", "Trace.inputs = values;", "Trace.inputs = null;", 0),
            ("late-capture-read", "Base.java", "Trace.captured = owner();", "Trace.captured = null;", 0),
            ("wrapped-checked", "Provider.java", "Trace.before.run();", "try { Trace.before.run(); } catch(Throwable failure) { throw new IllegalStateException(failure); }", 6),
            ("wrapped-error", "Base.java", "Trace.action.run();", "try { Trace.action.run(); } catch(Throwable failure) { throw new IllegalStateException(failure); }", 8),
            ("repeated-action", "Base.java", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", 0),
        ]
        for label, file_name, before, replacement, mode in wrong:
            text = (fixture / "fixture" / file_name).read_text()
            if text.count(before) != 1:
                raise RuntimeError("Owned wrong-program anchor changed: " + label)
            changed = work / label / file_name
            changed.parent.mkdir()
            changed.open("x").write(text.replace(before, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([changed], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     MAIN_CLASS, "0", str(mode), "true", "false"], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned captured intersection witness mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)
        signature = b"<T:Ljava/lang/Object;>Lfixture/Base<Ljava/lang/String;>;"
        mutations = {"raw-parent": b"<T:Ljava/lang/Object;>Lfixture/Base;",
                     "wrong-parent-element": signature.replace(b"Ljava/lang/String;", b"Ljava/lang/Integer;")}
        receipts = {}
        for label, changed_signature in mutations.items():
            changed = work / label / "fixture/Owner$Member.class"
            changed.parent.mkdir(parents=True)
            shutil.copy2(originals / "fixture/Owner$Member.class", changed)
            receipts[label] = change_signature(changed, signature, changed_signature)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (changed.parents[1], originals, harness))),
                                     MAIN_CLASS, "0", "0", "true", "false"], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned captured intersection witness mismatch" not in result.stderr:
                raise RuntimeError("Observer missed unchanged-Code bound drift: " + label)
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
            raise RuntimeError("Owned captured intersection witness DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original captured intersection witness declaration")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        declarations = {method for owner in classes for method, row in facts[owner]["methods"].items() if not row["code"]}
        member = "Lfixture/Owner$Member;"
        projected = {identity for identity in methods if identity.startswith(member + "->")}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 2 or coverage["status"] != "partial"
                or coverage["recovered_method_count"] != len(methods) - len(declarations) - 2
                or coverage.get("declaration_only_method_count", 0) != len(declarations)
                or coverage.get("minimum_java_release") != 25
                or any(row["status"] != ("source-projected" if identity in projected else "declaration-only" if identity in declarations else "recovered")
                       for identity, row in rows.items())
                or any(rows[identity].get("projection_kind") != "inner-to-static-member" for identity in projected)
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Captured intersection recovery changed original identities or contracts")
        bindings = coverage.get("class_source_bindings", [])
        capture = member + "->this$0:Lfixture/Owner;"
        if (len(bindings) != 1 or bindings[0].get("class") != member
                or bindings[0].get("binding_kind") != "inner-to-static-member"
                or bindings[0].get("outer_capture_field") != capture
                or "constructor generic signatures and implicit parameter metadata may change" not in bindings[0].get("metadata_differences", [])
                or not any("synthetic capture-field" in note for note in bindings[0].get("metadata_differences", []))):
            raise RuntimeError("Captured intersection lost its exact existing source disclosure")
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if excluded else None, release=release)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        allowed_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed_helpers:
            raise RuntimeError("Unused private intersection recovery added or omitted an original runtime entry")
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
            if owner == member:
                if wanted["fields"][capture]["access"] != 0x1010 or wanted["inner_class"]["access"] != 2:
                    raise RuntimeError("Original capture field or private member flags changed")
                wanted["fields"][capture]["access"] = 0x10
                wanted["inner_class"]["access"] = 0xA
                constructor = member + "-><init>(Lfixture/Owner;Ljava/lang/Object;I[Ljava/lang/String;)V"
                if (wanted["methods"][constructor]["method_parameters"] != implicit_parameters
                        or actual["methods"][constructor]["method_parameters"] is not None):
                    raise RuntimeError("Captured constructor exceeded its exact implicit parameter disclosure")
                wanted["methods"][constructor]["method_parameters"] = None
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Unused private intersection fixture compiler release changed")
            if wanted != actual or any(exceptions[method] != after_exceptions[method] for method in facts[owner]["methods"]):
                raise RuntimeError("Original class, Code roles, access, Signature, Exceptions or parameters changed: " + owner)

        (work / "disclosed-metadata.json").open("x").write(json.dumps({"status": "verified",
            "class": member, "captured_field": capture, "original_capture_flags": 0x1010,
            "compiled_capture_flags": 0x10, "original_inner_flags": 2, "compiled_inner_flags": 0xA,
            "original_implicit_constructor_parameters": implicit_parameters, "compiled_implicit_constructor_parameters": None,
            "all_other_original_class_member_Code_access_Signature_Exceptions_parameters": "unchanged",
            "binding": bindings[0]}, indent=2) + "\n")
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Unused private intersection recovery changed input evaluation, dispatch, partial writes or exact failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "unchanged_code_signature_controls": len(mutations),
                   "all_original_class_method_roles_and_metadata": "matched except the exact existing capture-field synthetic, member static and implicit constructor parameter disclosures",
                   "captured_original_argument_constructor_order_and_failure_identity": "matched",
                   "dependency_provider": args.dependency_provider, "minimum_java_release": release}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
