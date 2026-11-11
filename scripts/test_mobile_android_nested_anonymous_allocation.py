#!/usr/bin/env python3
"""Verify original anonymous callback identity inside ordered constructor arguments."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--conditional", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-anonymous-allocation-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "nested_anonymous_allocation/java"
        original_release = 17 if args.conditional else 8
        if args.conditional:
            authored = work / "conditional-java"
            shutil.copytree(fixture, authored)
            owner = authored / "fixture/Owner.java"
            text = owner.read_text()
            start = "value = new Target(Trace.context(), new Callback() {"
            end = "        });"
            if text.count(start) != 1 or text.count(end) != 1:
                raise RuntimeError("Owned anonymous argument anchors changed")
            text = text.replace(start, "value = new Target(Trace.context(), switch (0) { default -> {\n            Callback selected = new Callback() {")
            owner.write_text(text.replace(end, "        }; yield Trace.keep ? selected : null;\n        }});"))
            fixture = authored
        originals, harness = work / "originals", work / "harness"
        main_class = "fixture.NestedAnonymousAllocationHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=original_release)
        compiler.compile_local([FIXTURES / "harness" / "NestedAnonymousAllocationHarness.java"], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (6, 11):
            raise RuntimeError("Owned nested anonymous allocation inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Callback;", "Lfixture/Context;", "Lfixture/Target;"}
        excluded = providers if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"

        def write_provider(destination, source):
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                for owner in sorted(providers):
                    entry = owner[1:-1] + ".class"
                    jar.writestr(entry, (source / entry).read_bytes())

        write_provider(dependency, originals)
        dependency_hash = sha(dependency)

        def execute(runtime, label):
            outputs = []
            for route in range(3):
                for kind in range(3):
                    for mode in range(21 if args.conditional else 20):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), main_class, str(route), str(kind), str(mode)], f"{label}-{route}-{kind}-{mode}")
                        if text != "attempts=2,nested-anonymous-allocation=matched\n":
                            raise RuntimeError("Frozen nested anonymous allocation observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        wrong = [
            ("lost-initializer", "Target.java", "Trace.initializer.run();", ";", 0),
            ("repeated-initializer", "Target.java", "Trace.initializer.run();", "Trace.initializer.run(); Trace.initializer.run();", 0),
            ("wrapped-initializer", "Target.java", "Trace.initializer.run();", "try { Trace.initializer.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 2),
            ("lost-context-action", "Trace.java", "contextAction.run();", ";", 0),
            ("wrong-context-result", "Trace.java", "return current;", "return null;", 0),
            ("early-context", "Owner.java", "value = new Target(Trace.context(),", "Context context = Trace.context(); value = new Target(context,", 0),
            ("repeated-context", "Owner.java", "new Target(Trace.context(),", "new Target(Trace.context() == null ? Trace.context() : Trace.context(),", 0),
            ("wrong-constructor-receiver", "Target.java", "Trace.constructed = this;", "Trace.constructed = null;", 0),
            ("wrong-context-store", "Target.java", "this.context = context;", "this.context = null;", 0),
            ("lost-callback-store", "Target.java", "this.callback = callback;", "this.callback = null;", 0),
            ("lost-constructor-action", "Target.java", "Trace.constructorAction.run();", ";", 0),
            ("duplicate-constructor-action", "Target.java", "Trace.constructorAction.run();", "Trace.constructorAction.run(); Trace.constructorAction.run();", 0),
            ("wrapped-constructor", "Target.java", "Trace.constructorAction.run();", "try { Trace.constructorAction.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 8),
            ("wrong-callback-receiver", "Owner.java", "Trace.receiver = this;", "Trace.receiver = null;", 0),
            ("wrong-callback-argument", "Owner.java", "Trace.argument = value;", "Trace.argument = null;", 0),
            ("lost-callback-action", "Owner.java", "Trace.action.run();", ";", 0),
            ("duplicate-callback-action", "Owner.java", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", 0),
            ("wrapped-callback", "Owner.java", "Trace.action.run();", "try { Trace.action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 11),
            ("wrong-boolean-result", "Owner.java", "return Trace.flag;", "return !Trace.flag;", 0),
            ("early-boolean-result", "Owner.java", "Trace.action.run();\n                Trace.hit(6);\n                return Trace.flag;", "boolean saved = Trace.flag; Trace.action.run(); Trace.hit(6); return saved;", 19),
            ("lost-callback-completion", "Owner.java", "Trace.hit(6);", ";", 0),
            ("lost-owner-publication", "Owner.java", "value = new Target(", "new Target(", 0),
            ("extra-anonymous-entry", "Owner.java", "public boolean call(Object value)", "public int extra() { return 0; } public boolean call(Object value)", 0),
            ("invalid-helper-contract", "Owner.java", "public boolean call(Object value)", "private RuntimeException __neverdThrow(Throwable failure) { return new RuntimeException(failure); } public boolean call(Object value)", 0),
        ]
        if args.conditional:
            wrong += [
                ("lost-null-selection", "Owner.java", "yield Trace.keep ? selected : null;", "yield selected;", 20),
                ("lost-callback-selection", "Owner.java", "yield Trace.keep ? selected : null;", "yield (Callback) null;", 0),
            ]
        for label, filename, anchor, replacement, mode in wrong:
            text = (fixture / "fixture" / filename).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Nested anonymous allocation counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=original_release)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), main_class, "0", "0", str(mode)], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned nested anonymous allocation mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)

        classpath = ["--classpath", sdk] + (["--classpath", dependency] if excluded else [])
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dexdir = work / "dex"
        dexdir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        if list(dexdir.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned method DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original method declaration")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", dex], "original-allocation-disassembly")
        witnesses = []
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if not name or not prototype or block[1] + "->" + name[1] + prototype[1] != "Lfixture/Owner;-><clinit>()V":
                continue
            ops = [(int(pc, 16), op, tail) for pc, op, tail in re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])]
            outer = [pc for pc, op, tail in ops if op == "new-instance" and "Lfixture/Target;" in tail]
            context_reads = [pc for pc, op, tail in ops if op.startswith("invoke-static") and "Lfixture/Trace;.context:" in tail]
            allocate = [pc for pc, op, tail in ops if op == "new-instance" and "Lfixture/Owner$1;" in tail]
            initialize = [pc for pc, op, tail in ops if op.startswith("invoke-direct") and "Lfixture/Owner$1;.<init>:" in tail]
            finish = [pc for pc, op, tail in ops if op.startswith("invoke-direct") and "Lfixture/Target;.<init>:" in tail]
            guards = [pc for pc, op, tail in ops if op.startswith("if-")]
            if (any(len(pcs) != 1 for pcs in (outer, context_reads, allocate, initialize, finish))
                    or not outer[0] < context_reads[0] < allocate[0] < initialize[0] < finish[0]
                    or len(guards) != int(args.conditional)
                    or any(not initialize[0] < pc < finish[0] for pc in guards)):
                raise RuntimeError("Original nested anonymous allocation or conditional alias order changed")
            witnesses.append({"outer": outer, "context": context_reads, "anonymous": allocate, "initialize": initialize, "guards": guards, "finish": finish})
        if len(witnesses) != 1:
            raise RuntimeError("Missing original nested anonymous allocation witness")
        (work / "input-method-witness.json").open("x").write(json.dumps(witnesses[0], indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        anonymous = "Lfixture/Owner$1;"
        projected_methods = {"Lfixture/Owner;-><clinit>()V", anonymous + "-><init>()V", anonymous + "->call(Ljava/lang/Object;)Z"}
        expected_status = {m: "source-projected" if m in projected_methods else "recovered" if facts[m.split("->", 1)[0]]["methods"][m]["code"] else "declaration-only" for m in methods}
        bindings = coverage.get("class_source_bindings", [])
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != len(projected_methods) or coverage["status"] != "partial"
                or any(row["status"] != expected_status[row["identity"]] for row in coverage["methods"])
                or any(row.get("projection_kind") != "anonymous-interface" for row in coverage["methods"] if row["identity"] in projected_methods)
                or coverage["recovered_method_count"] != sum(s == "recovered" for s in expected_status.values())
                or coverage.get("declaration_only_method_count", 0) != sum(s == "declaration-only" for s in expected_status.values())
                or len(bindings) != 1 or bindings[0]["class"] != anonymous or bindings[0]["binding_kind"] != "anonymous-interface"
                or bindings[0]["allocation_method"] != "Lfixture/Owner;-><clinit>()V" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Nested anonymous allocation changed its original identities or coverage")
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency if excluded else None, release=release)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        allowed_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed_helpers:
            raise RuntimeError("Method recovery added or omitted an original runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for helper in helpers:
            wanted = {"name": "__neverdThrow", "prototype": "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;",
                      "access": 0x2 if helper.startswith(anonymous + "->") else 0xA, "code": True,
                      "signature": "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;",
                      "deprecated_attribute": False, "runtime_visible_annotations": [],
                      "runtime_invisible_annotations": [], "runtime_invisible_parameter_annotations": [], "method_parameters": None}
            if after[helper.split("->", 1)[0]]["methods"][helper] != wanted or after_exceptions[helper] != ["Ljava/lang/Throwable;"]:
                raise RuntimeError("Generated throw helper changed its exact admitted contract")
        for c in classes:
            wanted, actual = copy.deepcopy(facts[c]), copy.deepcopy(after[c])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 44 + original_release or actual.pop("major") != 44 + release:
                raise RuntimeError("Method fixture compiler release changed")
            if wanted != actual or any(exceptions[m] != after_exceptions[m] for m in facts[c]["methods"]):
                raise RuntimeError("Original method Code, roles, Signature, Exceptions or parameter metadata changed: " + c)
        (work / "disclosed-metadata.json").open("x").write(json.dumps({"status": "all-original-class-and-method-facts-matched", "anonymous_binding": bindings[0], "original_and_rebuilt_anonymous_identity": anonymous}, indent=2) + "\n")
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Method recovery changed initialization, checks, dispatch or exception identity")

        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original method program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "metadata_roles_exceptions": "unchanged", "original_anonymous_identity": "unchanged", "dependency_provider": args.dependency_provider, "conditional": args.conditional, "minimum_java_release": release}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
