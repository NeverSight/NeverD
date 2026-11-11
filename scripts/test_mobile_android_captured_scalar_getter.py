#!/usr/bin/env python3
"""Verify complete three- and four-formal multiple seed scopes and partial stores."""
from __future__ import annotations

import argparse
import copy
from contextlib import nullcontext
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-scalar-getter-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "captured_scalar_getter/java"
        observers = [FIXTURES / "harness" / (name + ".java") for name in ("CapturedScalarGetterCaller", "CapturedScalarGetterHarness")]
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 15:
            raise RuntimeError("Owned multiple seed void inventory changed")
        providers = {"Lfixture/Api;"}
        classes = all_classes - (providers if args.dependency_interface else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_interface:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for route in range(6):
                for kind in range(3):
                    for mode in range(12):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "CapturedScalarGetterHarness", str(route), str(kind), str(mode)], f"{label}-{route}-{kind}-{mode}")
                        expected = f"route={route},kind={kind},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Owned captured scalar getter behavior matrix changed")
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned multiple seed void DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original multiple seed definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if "Lfixture/Owner$Cursor;.get:" not in disassembly or "invoke-virtual" not in disassembly or "invoke-interface" not in disassembly:
            raise RuntimeError("D8 lost an original concrete/interface captured getter witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        captured = "Lfixture/Owner$Cursor;"
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity in methods if identity.split("->", 1)[0] == captured}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != 3 or coverage["status"] != "partial"
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release") != 25
                or {identity for identity, row in rows.items() if row["status"] == "source-projected"} != projected
                or any(rows[identity].get("projection_kind") != "inner-to-static-member" for identity in projected)):
            raise RuntimeError("Captured getter recovery changed original definitions or projection disclosure")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != captured
                or bindings[0].get("binding_kind") != "inner-to-static-member"
                or bindings[0].get("outer_capture_field") != captured + "->this$0:Lfixture/Owner;"
                or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Captured getter lost the exact original outer field receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=dependency if args.dependency_interface else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner != "Lfixture/Api;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Multiple seed recovery invented a runtime entry or omitted helper accounting")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = copy.deepcopy(records[owner]), actual_records[owner]
            if owner == captured:
                field = before["fields"][captured + "->this$0:Lfixture/Owner;"]
                if field["access"] != 0x1010 or not before["inner_class"]:
                    raise RuntimeError("Owned captured getter original field metadata changed")
                field["access"] &= ~0x1000
                before["inner_class"]["access"] |= 8
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Multiple seed class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                if after["methods"].get(method) != metadata or actual_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Multiple seed method metadata/Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_interface else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Multiple seed call changed original effects or failure state")
        original_signature = b"<T:Ljava/lang/Object;>(I)TT;"
        mutations = (
            ("narrower-result-bound", original_signature.replace(b"T:Ljava/lang/Object;", b"T:Ljava/lang/Number;")),
            ("extra-formal", original_signature.replace(b">(", b"U:Ljava/lang/Object;>(")),
            ("unbound-result", original_signature.replace(b")TT;", b")TU;")),
            ("formal-input-descriptor", original_signature.replace(b"(I)", b"(TT;)")),
            ("lost-formal-result", original_signature.replace(b")TT;", b")Ljava/lang/Object;")),
            ("generic-throws", original_signature + b"^TT;"),
        )
        for label, changed in mutations:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Owner$Cursor.class", original_signature, changed)
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_interface:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                    for owner in sorted(providers):
                        name = owner[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            negative_paths = [negative / path.relative_to(originals) for path in paths]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath,
                        "--output", negative_dex, *negative_paths], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr.lower():
                raise RuntimeError("Multiple seed invocation accepted an unproved signature: " + label)
        owner_body = (fixture / "fixture/Owner.java").read_text()
        client_body = (fixture / "fixture/Client.java").read_text()
        trace_body = (fixture / "fixture/Trace.java").read_text()
        direct = "Object value = cursor.get(index); Trace.hit(7); return value;"
        casts = "Object value = cursor.get(index); Trace.hit(7); return (String) value;"
        snapshots = "Trace.hit(3);\n            Object[] cells = Owner.this.cells;"
        wrong_programs = (
            ("wrong-outer", "Owner", owner_body, "Object[] cells = Owner.this.cells;", "Object[] cells = new Owner().cells;", 0, 0, 0, 1),
            ("wrong-index", "Owner", owner_body, "cells[offset + index]", "cells[index]", 0, 0, 0, 1),
            ("early-array-snapshot", "Owner", owner_body, snapshots,
             "Object[] cells = Owner.this.cells;\n            Trace.hit(3);", 0, 0, 8, 1),
            ("repeated-getter", "Client", client_body, "Object value = cursor.get(index);",
             "Object value = cursor.get(index); cursor.get(index);", 0, 0, 0, 6),
            ("early-result-cast", "Client", client_body, casts,
             "String value = cursor.get(index); Trace.hit(7); return value;", 2, 0, 0, 2),
            ("changed-append-overload", "Client", client_body,
             "return new StringBuilder().append(value).toString();",
             "return new StringBuilder().append((char[])value).toString();", 4, 0, 0, 2),
            ("bypassed-virtual-dispatch", "Client", client_body,
             "public static Object direct(Owner.Cursor cursor, int index) {\n        " + direct,
             "public static Object direct(Owner.Cursor cursor, int index) {\n        Object value = cursor.owner().cells[cursor.offset + index]; Trace.hit(7); return value;", 0, 1, 1, 1),
            ("changed-failure-identity", "Trace", trace_body, "callback.run();",
             "try { callback.run(); } catch (RuntimeException failure) { throw new IllegalStateException(); }", 0, 0, 9, 1),
            ("wrong-factory-owner", "Owner", owner_body, "return new Cursor(offset);",
             "return new Owner().new Cursor(offset);", 0, 0, 0, 1),
        )
        for label, owner, body, before, after, route, kind, mode, occurrences in wrong_programs:
            if body.count(before) != occurrences:
                raise RuntimeError("Owned captured getter counterexample anchor changed: " + label)
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.open("x").write(body.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     "CapturedScalarGetterHarness", str(route), str(kind), str(mode)], text=True, capture_output=True,
                                     timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "captured scalar getter changed" not in result.stderr:
                raise RuntimeError("Frozen captured getter observer missed " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable observers or SDK bytes changed")
        evidence = {"status": "success", "coverage": "partial", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "projected_methods": 3,
                    "dependency_interface": args.dependency_interface, "original_method_signatures_and_exceptions": "unchanged",
                    "captured_outer_receiver_dispatch_raw_identity_late_casts_and_overloads": "matched",
                    "original_exception_and_error_identity": "matched", "minimum_java_release": 25,
                    "same_code_signature_mutations": len(mutations), "independent_wrong_programs": len(wrong_programs),
                    "disclosed_metadata_projection": "static-member-and-capture-field-synthetic-flag",
                    "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
