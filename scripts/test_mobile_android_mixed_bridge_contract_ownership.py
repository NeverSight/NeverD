#!/usr/bin/env python3
"""Verify fixed source and SDK interface contracts regenerate the original checked bridge."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-mixed-bridge-contract-ownership-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "mixed_bridge_contract_ownership"
        originals, harness = work / "originals", work / "harness"
        name = "fixture.MixedBridgeContractOwnershipHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in ("MixedBridgeContractOwnershipHarness", "MixedBridgeContractOwnershipCaller")], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 18):
            raise RuntimeError("Owned mixed bridge contract ownership inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Base;", "Lfixture/Reader;", "Lfixture/Result;", "Lfixture/Parent;"}
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

        def execute(runtime, method):
            outputs = []
            for route in range(5):
                for branch in range(2):
                    for mode in range(24):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(branch), str(mode)],
                                          f"{method}-{route}-{branch}-{mode}")
                        if text != "attempts=2,mixed-bridge-contract-ownership=matched\n":
                            raise RuntimeError("Frozen mixed bridge contract ownership observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        wrong = [('lost-receiver-effects', 'Core.java', 'Trace.before.run();', ';', 1, 0, 0), ('lost-key', 'Core.java', 'receiver().read(key)', 'receiver().read(null)', 0, 0, 0), ('lost-generic-key', 'MixedBridgeContractOwnershipCaller.java', 'return receiver.read(key);', 'return receiver.read(null);', 2, 0, 0), ('lost-parent-call', 'Leaf.java', 'return super.read(key);', 'return Trace.result;', 1, 0, 0), ('lost-leaf-prefix', 'Leaf.java', 'Trace.leaves++;Trace.hit(8);', ';', 1, 0, 0), ('changed-parent-receiver', 'Leaf.java', 'super.read(key)', 'new Parent().read(key)', 3, 1, 0), ('changed-receiver', 'Parent.java', 'Trace.actualReceiver=this;', 'Trace.actualReceiver=null;', 1, 0, 0), ('changed-key', 'Parent.java', 'Trace.key=key;', 'Trace.key=null;', 1, 0, 0), ('lost-null-key-read', 'Parent.java', 'Trace.observed=key.length();', 'Trace.observed=9;', 1, 0, 1), ('lost-callback', 'Parent.java', 'Trace.action.run();', ';', 1, 0, 0), ('repeated-callback', 'Parent.java', 'Trace.action.run();', 'Trace.action.run();Trace.action.run();', 1, 0, 0), ('wrapped-failure', 'Parent.java', 'Trace.action.run();', 'try{Trace.action.run();}catch(Throwable failure){throw new IllegalArgumentException(failure);}', 1, 0, 8), ('fresh-result', 'Parent.java', 'return Trace.result;', 'return new Result();', 1, 0, 0), ('early-result-read', 'Parent.java', 'Trace.observed=key.length();Trace.hit(2);Trace.action.run();Trace.hit(3);\n        return Trace.result;', 'Result saved=Trace.result;Trace.observed=key.length();Trace.hit(2);Trace.action.run();Trace.hit(3);\n        return saved;', 1, 0, 9), ('lost-final-stage', 'Parent.java', 'Trace.hit(3);', ';', 4, 1, 0), ('early-cast', 'Core.java', 'return ((Reader)receiver()).read(key);', 'return typed((String)key);', 1, 0, 2), ('lost-null-key-call', 'Core.java', 'return ((Reader)receiver()).read(key);', 'return key==null ? null : ((Reader)receiver()).read(key);', 1, 0, 1), ('lost-seed-callback', 'Parent.java', 'Trace.seedAction.run();', ';', 1, 0, 0), ('repeated-seed-callback', 'Parent.java', 'Trace.seedAction.run();', 'Trace.seedAction.run();Trace.seedAction.run();', 1, 0, 0), ('changed-seed-receiver', 'Parent.java', 'Trace.seedReceiver=this;', 'Trace.seedReceiver=null;', 1, 0, 0), ('changed-seed-argument', 'Parent.java', 'Trace.seedArgument=value;', 'Trace.seedArgument=null;', 1, 0, 0), ('fresh-seed-result', 'Parent.java', 'return Trace.selected;', 'return new Leaf();', 1, 0, 0), ('early-seed-result', 'Parent.java', 'Trace.seedAction.run();Trace.hit(5);return Trace.selected;', 'Parent saved=Trace.selected;Trace.seedAction.run();Trace.hit(5);return saved;', 1, 0, 22), ('lost-seed-override', 'Leaf.java', 'Trace.seedLeaves++;Trace.hit(6);', ';', 3, 1, 0), ('lost-seed-super-call', 'Leaf.java', 'return (Leaf)super.accept(value);', 'return (Leaf)Trace.selected;', 1, 0, 0), ('wrapped-seed-failure', 'Parent.java', 'Trace.seedAction.run();', 'try{Trace.seedAction.run();}catch(Throwable failure){throw new IllegalArgumentException(failure);}', 1, 0, 18), ('wrong-seed-receiver', 'Parent.java', 'Trace.chosen=accept(key);', 'Trace.chosen=new Parent().accept(key);', 1, 0, 0), ('lost-chosen-result', 'Parent.java', 'Trace.chosen=accept(key);', 'accept(key);Trace.chosen=null;', 1, 0, 0)]
        for label, filename, anchor, replacement, route, branch, mode in wrong:
            original_source = (FIXTURES / "harness" / filename) if filename.endswith("Caller.java") else fixture / "java/fixture" / filename
            text = original_source.read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Mixed bridge contract ownership counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(branch), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned mixed bridge contract ownership mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)
        classpath = ["--classpath", sdk]
        if excluded:
            classpath += ["--classpath", dependency]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original mixed bridge contract ownership definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        minimum_java_release = coverage.get("minimum_java_release", 8)
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) != (2 if excluded else 3) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings") or coverage.get("requires_java_rebuild_postprocessing") or minimum_java_release != 8):
            raise RuntimeError("Mixed bridge contract ownership recovery changed definitions or generic metadata")
        declarations = {m for owner in classes for m, row in facts[owner]["methods"].items() if row["access"] & 0x500}
        actual_declarations = {row["identity"] for row in coverage["methods"] if row["status"] == "declaration-only"}
        if actual_declarations != declarations or coverage["declaration_only_method_count"] != len(declarations):
            raise RuntimeError("Mixed bridge contract ownership abstract declarations changed")
        if any(row["instruction_count"] for row in coverage["methods"] if row["status"] == "declaration-only"):
            raise RuntimeError("Original declaration unexpectedly acquired instructions")
        bridge_owners = {"Lfixture/Leaf;"} | (set() if excluded else {"Lfixture/Parent;"})
        expected_bridges = {owner + "->read(Ljava/lang/Object;)Lfixture/Result;":
                            ("generic-interface-bridge", owner + "->read(Ljava/lang/String;)Lfixture/Result;", "Lfixture/Reader;->read(Ljava/lang/Object;)Lfixture/Result;") for owner in bridge_owners}
        expected_bridges["Lfixture/Leaf;->accept(Ljava/lang/Object;)Lfixture/Parent;"] = (
            "generic-superclass-bridge", "Lfixture/Leaf;->accept(Ljava/lang/Object;)Lfixture/Leaf;", "Lfixture/Parent;->accept(Ljava/lang/Object;)Lfixture/Parent;")
        projected = {row["identity"]: row for row in coverage["methods"] if row["status"] == "source-projected"}
        if set(projected) != set(expected_bridges):
            raise RuntimeError("Original mixed bridge identity set changed")
        for identity, row in projected.items():
            if ((row["projection_kind"], row["bridge_target"], row["bridge_dispatch"]) != expected_bridges[identity]
                    or "remain unverified" not in row["reason"]):
                raise RuntimeError("Mixed bridge contract mapping or status changed")
        if any(row["status"] not in ("recovered", "source-projected", "declaration-only") for row in coverage["methods"]):
            raise RuntimeError("Original mixed bridge recovery status changed")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=minimum_java_release,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Mixed bridge contract ownership recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + minimum_java_release:
                raise RuntimeError("Mixed bridge contract ownership compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Mixed bridge contract ownership metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Mixed bridge contract ownership dispatch, original effects or exceptions changed")
        controls = [('wrong-closed-slot', 'Lfixture/Parent;', None, 'Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;Lfixture/Reader<Ljava/lang/String;>;', 'Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;Lfixture/Reader<Ljava/lang/Integer;>;'), ('star-interface', 'Lfixture/Parent;', None, 'Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;Lfixture/Reader<Ljava/lang/String;>;', 'Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;Lfixture/Reader<*>;'), ('foreign-formal', 'Lfixture/Parent;', None, 'Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;Lfixture/Reader<Ljava/lang/String;>;', 'Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;Lfixture/Reader<TAbsent;>;'), ('wrong-super-erasure', 'Lfixture/Parent;', None, 'Lfixture/Base<Ljava/lang/String;Ljava/lang/Object;>;Lfixture/Reader<Ljava/lang/String;>;', 'Ljava/lang/Object;Lfixture/Reader<Ljava/lang/String;>;'), ('foreign-seed-formal', 'Lfixture/Leaf;', 'Lfixture/Leaf;->accept(Ljava/lang/Object;)Lfixture/Leaf;', '<U:Ljava/lang/Object;>(TU;)Lfixture/Leaf;', '<U:Ljava/lang/Object;>(TMissing;)Lfixture/Leaf;'), ('extra-unused-formal', 'Lfixture/Leaf;', 'Lfixture/Leaf;->accept(Ljava/lang/Object;)Lfixture/Leaf;', '<U:Ljava/lang/Object;>(TU;)Lfixture/Leaf;', '<U:Ljava/lang/Object;V:Ljava/lang/Object;>(TU;)Lfixture/Leaf;'), ('erased-instead-of-method-formal', 'Lfixture/Leaf;', 'Lfixture/Leaf;->accept(Ljava/lang/Object;)Lfixture/Leaf;', '<U:Ljava/lang/Object;>(TU;)Lfixture/Leaf;', '<U:Ljava/lang/Object;>(Ljava/lang/Object;)Lfixture/Leaf;'), ('changed-seed-bound', 'Lfixture/Leaf;', 'Lfixture/Leaf;->accept(Ljava/lang/Object;)Lfixture/Leaf;', '<U:Ljava/lang/Object;>(TU;)Lfixture/Leaf;', '<U:Ljava/lang/Number;>(TU;)Lfixture/Leaf;')]
        for label, target_class, target_member, original_signature, changed_signature in controls:
            changed_originals = work / (label + "-originals")
            shutil.copytree(originals, changed_originals)
            target = changed_originals / (target_class[1:-1] + ".class")
            code_before = method_code(target.read_bytes())
            receipt = change_signature(target, original_signature.encode(), changed_signature.encode())
            receipt["target_class"] = target_class
            receipt["target_identity"] = target_member or target_class
            metadata = compiler_facts(changed_originals)[target_class]
            actual_signature = metadata["methods"][target_member]["signature"] if target_member else metadata["signature"]
            if method_code(target.read_bytes()) != code_before or actual_signature != changed_signature:
                raise RuntimeError("Signature control changed Code or failed to retain exact metadata")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            counter_classpath = ["--classpath", sdk]
            if excluded:
                counter_provider = work / (label + "-provider.jar")
                write_provider(counter_provider, changed_originals)
                counter_classpath += ["--classpath", counter_provider]
            counter_dex = work / (label + "-dex")
            counter_dex.mkdir()
            counter_paths = [p for p in sorted(changed_originals.rglob("*.class")) if "L" + p.relative_to(changed_originals).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--release", "--min-api", "24", *counter_classpath, "--output", counter_dex, *counter_paths], label + "-d8")
            if not excluded or target_class not in providers:
                annotations = verify.run([verify.dexdump, "-a", "-f", "-h", "-l", "plain", counter_dex / "classes.dex"], label + "-annotations")
                owners = re.findall(r"Class #(\d+)\s+-\n\s*Class descriptor\s*:\s*'" + re.escape(target_class) + "'", annotations)
                if len(owners) != 1:
                    raise RuntimeError("Missing exact class Signature annotation owner")
                blocks = re.findall(r"Class #" + owners[0] + r" annotations:\n(.*?)\nClass #" + owners[0] + r"\s+-", annotations, re.S)
                if len(blocks) != 1:
                    raise RuntimeError("Missing exact Signature annotation owner block")
                if target_member:
                    name = re.escape(target_member.split("->", 1)[1].split("(", 1)[0])
                    members = re.findall(r"Annotations on method #[0-9]+ '" + name + r"'\n((?:  [^\n]*\n)+)", blocks[0] + "\n")
                    values = [value for part in members for value in re.findall(r"VISIBILITY_SYSTEM Ldalvik/annotation/Signature; value=\{ (.*?) \}", part)]
                else:
                    if blocks[0].count("Annotations on class") != 1:
                        raise RuntimeError("Missing exact class Signature annotation")
                    classblock = blocks[0].split("Annotations on class", 1)[1].split("Annotations on ", 1)[0]
                    values = re.findall(r"VISIBILITY_SYSTEM Ldalvik/annotation/Signature; value=\{ (.*?) \}", classblock)
                if len(values) != 1:
                    raise RuntimeError("Missing exact class Signature annotation value")
                pattern = r'"(?:[^"\\]|\\.)*"'
                pieces = re.findall(pattern, values[0])
                encoded = "".join(json.loads(piece) for piece in pieces)
                if re.sub(pattern, "", values[0]).strip() or encoded != changed_signature:
                    raise RuntimeError("D8 normalized the intended class Signature control: " + label)
                (work / (label + "-dex-signature.json")).open("x").write(json.dumps({"expected": changed_signature, "encoded": encoded, "status": "exact"}, indent=2) + "\n")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(counter_dex / "classes.dex"), *map(str, counter_classpath), "--timeout", "600", "-o", str(work / (label + "-rejected"))], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-rejection.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode != 1 or (work / (label + "-rejected")).exists():
                raise RuntimeError("Unproved mixed bridge contract ownership Signature was accepted: " + label)

        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": minimum_java_release}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
