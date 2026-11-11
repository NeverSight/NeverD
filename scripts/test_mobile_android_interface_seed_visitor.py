#!/usr/bin/env python3
"""Verify interface-bounded direct seeds and complete closed-result visitors."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
from copy import deepcopy
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
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code

ACTION = "Lfixture/Node$Companion$Action;"
SIGNATURE = b"<T::Lfixture/Node;>(TT;Lfixture/Fn<-TT;+Lfixture/Node$Companion$Action;>;)V"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--private-body", action="store_true")
    args = parser.parse_args()
    if args.private_body and args.dependency_provider:
        parser.error("--private-body requires an owned source provider")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interface-seed-visitor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "interface_seed_visitor/java"
        names = ("InterfaceSeedVisitorCaller", "InterfaceSeedVisitorHarness")
        originals, harness = work / "originals", work / "harness"
        inputs = {p.name: p for p in sorted(fixture.rglob("*.java"))}
        observers = [FIXTURES / "harness" / (n + ".java") for n in names]
        if args.private_body:
            inputs.update({p.name: p for p in sorted((fixture.parent / "private_java").rglob("*.java"))})
            observers[0] = fixture.parent / "private_harness/InterfaceSeedVisitorCaller.java"
        verify.compile_local(list(inputs.values()), originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 18 if args.private_body else 17):
            raise RuntimeError("Original interface visitor definition inventory changed")
        providers = all_classes - {"Lfixture/Client;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        target = "Lfixture/Provider;->visit(Lfixture/Node;Lfixture/Fn;)V"
        if records["Lfixture/Provider;"]["methods"][target]["signature"] != SIGNATURE.decode():
            raise RuntimeError("Original interface seed and complete callback relation changed")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(providers):
                path = owner[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), names[1],
                                         str(route), str(mode)], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen interface visitor observer changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original interface visitor DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original interface visitor definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-visitor-disassembly")
        if "Lfixture/Provider;.visit:(Lfixture/Node;Lfixture/Fn;)V" not in disassembly or "invoke-static" not in disassembly:
            raise RuntimeError("D8 lost the exact interface-bounded invocation witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        projected = 0 if args.dependency_provider else 4
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != projected
                or coverage["status"] != ("partial" if projected else "recovered")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Interface visitor recovery changed original definitions or contracts")
        bindings = coverage.get("class_source_bindings", [])
        if args.dependency_provider:
            if bindings:
                raise RuntimeError("Declaration-only result invented a source binding")
        elif (len(bindings) != 1 or bindings[0].get("class") != ACTION or bindings[0].get("binding_kind") != "enum-declaration"
              or bindings[0].get("compiler_generated_members_status") != "unverified"
              or not bindings[0].get("metadata_differences")
              or [(c["field"], c["ordinal"]) for c in bindings[0]["constants"]] != [("NEXT", 0), ("STOP", 1), ("SKIP", 2)]):
            raise RuntimeError("Missing original enum constants and compiler boundary disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_provider else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for c in classes - {"Lfixture/Fn;", "Lfixture/Node;"}}
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        compiler_helpers = set()
        if not args.dependency_provider:
            compiler_helpers = set(actual_methods) - methods - helpers
            if (len(compiler_helpers) != 1 or not all(re.fullmatch(re.escape(ACTION) + r"->\$values\$+\(\)\[" + re.escape(ACTION), m) for m in compiler_helpers)
                    or any(actual_records[ACTION]["methods"][m]["access"] != 0x100A
                           or actual_records[ACTION]["methods"][m]["signature"] is not None
                           or actual_exceptions[m] for m in compiler_helpers)):
                raise RuntimeError("Enum compiler regenerated an unaccounted backing-array helper")
        if actual_classes != classes or helpers != expected_helpers or set(actual_methods) != methods | helpers | compiler_helpers:
            raise RuntimeError("Interface visitor source omitted or invented an unaccounted declaration")
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Undisclosed interface visitor class metadata change: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = deepcopy(metadata)
                if identity == ACTION + "->$values()[" + ACTION:
                    if wanted["access"] != 0x100A or not bindings:
                        raise RuntimeError("Original enum backing helper changed")
                    wanted["access"] &= ~0x1000
                if after["methods"].get(identity) != wanted or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed interface visitor method metadata or Exceptions change: " + identity)
        (work / "enum-compiler-boundary.json").open("x").write(json.dumps({"binding": bindings, "generated_methods": sorted(compiler_helpers),
            "source_synthetic_flag_loss": [ACTION + "->$values()[" + ACTION] if compiler_helpers else []}, indent=2) + "\n")
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Recovered visitor changed overloads, snapshots, identities or partial failures")
        mutations = (
            ("producer-input", SIGNATURE.replace(b"<-TT;", b"<+TT;")),
            ("consumer-result", SIGNATURE.replace(b";+Lfixture/Node$Companion$Action;", b";-Lfixture/Node$Companion$Action;")),
            ("missing-seed", SIGNATURE.replace(b">(TT;", b">(Lfixture/Node;")),
            ("unseeded-input-formal", SIGNATURE.replace(b"Node;>", b"Node;U:Ljava/lang/Object;>").replace(b"<-TT;", b"<-TU;")),
            ("formal-result", SIGNATURE.replace(b"+Lfixture/Node$Companion$Action;", b"+TT;")),
            ("unused-formal", SIGNATURE.replace(b"Node;>", b"Node;U:Ljava/lang/Object;>")),
            ("class-interface-bound", SIGNATURE.replace(b"<T::", b"<T:")),
            ("unavailable-bound", SIGNATURE.replace(b"<T::Lfixture/Node;>", b"<T::Lfixture/Missing;>")),
        )
        for label, changed in mutations:
            if changed == SIGNATURE:
                raise RuntimeError("Signature counterexample did not change its contract")
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / "fixture/Provider.class", SIGNATURE, changed)
            if method_code((negative / "fixture/Provider.class").read_bytes()) != method_code((originals / "fixture/Provider.class").read_bytes()):
                raise RuntimeError("Signature control changed original compiler Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if args.dependency_provider:
                jar_path = work / (label + "-provider.jar")
                with zipfile.ZipFile(jar_path, "x") as jar:
                    for owner in sorted(providers):
                        name = owner[1:-1] + ".class"
                        jar.writestr(name, (negative / name).read_bytes())
                negative_classpath[-1] = jar_path
            negative_dex = work / (label + "-dex")
            negative_dex.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *negative_classpath, "--output", negative_dex,
                        *[negative / p.relative_to(originals) for p in paths]], label + "-d8")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex / "classes.dex"),
                                     *map(str, negative_classpath), "-o", str(rejected)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode != 1 or rejected.exists() or "generic" not in result.stderr.lower():
                raise RuntimeError("Interface visitor accepted an unproved complete contract: " + label)
        provider = inputs["Provider.java"].read_text()
        client = inputs["Client.java"].read_text()
        call = "Provider.forward(start, operation);" if args.private_body else "Provider.visit(start, operation);"
        wrong_programs = (
            ("wrong-overload", "Client", client, call, "Provider.visit((Object)start, operation);", 0),
            ("bypassed-call", "Client", client, call, "", 0),
            ("repeated-call", "Client", client, call, call + " " + call, 0),
            ("wrong-input", "Provider", provider, "operation.apply(start)", "operation.apply(null)", 0),
            ("dropped-before", "Provider", provider, "Trace.before(start);", "", 0),
            ("dropped-after", "Provider", provider, "Trace.after(start, choice);", "", 0),
            ("reloaded-seed", "Provider", provider, "Trace.after(start, choice);", "Trace.after(Trace.seed, choice);", 0),
            ("reloaded-choice", "Provider", provider, "Trace.after(start, choice);", "Trace.after(start, Trace.produced);", 0),
            ("duplicate-callback", "Provider", provider, "Node.Companion.Action choice = operation.apply(start);", "Node.Companion.Action choice = operation.apply(start); operation.apply(start);", 0),
            ("early-after", "Provider", provider, "Trace.before(start);", "Trace.before(start); Trace.after(start, null);", 0),
            ("wrapped-failure", "Provider", provider, "Node.Companion.Action choice = operation.apply(start);", "Node.Companion.Action choice; try { choice = operation.apply(start); } catch (RuntimeException failure) { throw new IllegalStateException(); }", 8),
            ("extra-input-cast", "Client", client, call, "String value = (String)(Object)start; " + call, 0),
        )
        for label, owner, body, anchor, changed, mode in wrong_programs:
            if body.count(anchor) != 1:
                raise RuntimeError("Owned visitor counterexample anchor changed: " + label)
            source = work / label / ("fixture/" + owner + ".java")
            source.parent.mkdir(parents=True)
            source.open("x").write(body.replace(anchor, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), names[1], "0", str(mode)],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned interface seed visitor" not in result.stderr:
                raise RuntimeError("Frozen visitor observer missed " + label)
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash:
            raise RuntimeError("Frozen original, observer or dependency bytes changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 64, "attempts_per_case": 2, "same_code_signature_controls": len(mutations),
                   "wrong_programs": len(wrong_programs), "dependency_provider": args.dependency_provider,
                   "private_body": args.private_body}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
