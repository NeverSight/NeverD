#!/usr/bin/env python3
"""Verify SDK generic default dispatch through known plain class ancestors."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    parser.add_argument("--member-signatures", action="store_true")
    parser.add_argument("--nonpublic-methods", action="store_true")
    args = parser.parse_args()
    args.member_signatures |= args.nonpublic_methods
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-plain-dependency-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture_name = "plain_member_contracts" if args.member_signatures else "plain_dependency_defaults"
        harness_name = "PlainMemberContractsHarness" if args.member_signatures else "PlainDependencyDefaultsHarness"
        inputs = sorted((FIXTURES / (fixture_name + "/java")).rglob("*.java"))
        harness_source = FIXTURES / ("harness/" + harness_name + ".java")
        harness_entry = harness_name
        if args.nonpublic_methods:
            changed_inputs = []
            for source in inputs:
                content = source.read_text()
                if source.name == "PlainState.java":
                    declaration = "public <M> M echo(M item)"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing original ordinary parent method formal")
                    content = content.replace(declaration, "<M> M echo(M item)", 1)
                if source.name == "Worker.java":
                    declaration = "    public void accept(T item) {"
                    if content.count(declaration) != 1:
                        raise RuntimeError("Missing original ordinary child method anchor")
                    content = content.replace(declaration, "    public int echoCalls;\n    <N> N echo(N item) { echoCalls++; return item; }\n\n" + declaration, 1)
                destination = work / "nonpublic-java/fixture" / source.name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text(content)
                changed_inputs.append(destination)
            inputs = changed_inputs
            content = "package fixture;\n\n" + harness_source.read_text()
            declaration = 'fixture.PlainState.class.getMethod("echo", Object.class)'
            if content.count(declaration) != 1:
                raise RuntimeError("Missing original full method-formal reflection")
            content = content.replace(declaration, declaration.replace("getMethod", "getDeclaredMethod"), 1)
            marker = '        System.out.println("values="'
            if content.count(marker) != 1:
                raise RuntimeError("Missing immutable package-method harness anchor")
            content = content.replace(marker, """
        value((fixture.PlainState.class.getDeclaredMethod("echo", Object.class).getModifiers() & 7) == 0);
        Method childEcho = Worker.class.getDeclaredMethod("echo", Object.class);
        value((childEcho.getModifiers() & 7) == 0);
        value(childEcho.getTypeParameters().length == 1);
        identity(childEcho.getGenericParameterTypes()[0], childEcho.getTypeParameters()[0]);
        identity(childEcho.getGenericReturnType(), childEcho.getTypeParameters()[0]);
        Worker<Object,Object> packageWorker = new Worker<>(false, new java.util.ArrayList<>(), item -> {});
        Object first = new Object();
        Object second = new Object();
        for (Object item : new Object[] {first, second, null, first}) {
            int calls = packageWorker.echoCalls;
            identity(packageWorker.echo(item), item);
            identity(((fixture.PlainState) packageWorker).echo(item), item);
            value(packageWorker.echoCalls == calls + 2);
        }
""" + marker, 1)
            harness_source = work / harness_source.name
            harness_source.write_text(content)
            harness_entry = "fixture." + harness_name
        verify.compile_local(inputs, originals)
        verify.compile_local([harness_source], harness, classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        parent_owners = {"Lfixture/PlainState;", "Lfixture/View;"} if args.member_signatures else {"Lfixture/PlainState;"}
        classes = all_classes - parent_owners if args.dependency_parent else all_classes
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        dependency = work / "owned-parent.jar"

        def write_parent(root: Path, destination: Path) -> None:
            with zipfile.ZipFile(destination, "x", zipfile.ZIP_DEFLATED) as archive:
                for owner in sorted(parent_owners):
                    relative = owner[1:-1] + ".class"
                    archive.writestr(relative, (root / relative).read_bytes())

        write_parent(originals, dependency)
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               harness_entry], "baseline")
        wanted_counts = ((140, 111, 24) if args.nonpublic_methods else (133, 101, 24)) if args.member_signatures else (100, 56, 24)
        wanted_summary = ("sdk-default-owner=retained,complete-parent-member-contracts-and-ordered-effects=matched"
                          if args.member_signatures else
                          "sdk-default-owner=retained,atomic-state-and-ordered-dispatch=matched")
        if baseline.splitlines() != ["values=%d,identities=%d,failures=%d" % wanted_counts,
                                    wanted_summary]:
            raise RuntimeError("Owned SDK default baseline missed expected cases: " + baseline)

        def dex(root: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", destination,
                        *[p for p in sorted(root.rglob("*.class"))
                          if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]], "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("D8 changed original SDK default DEX inventory")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the independently authored SDK default declarations")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        classpath = ["--classpath", str(args.android_jar.resolve())]
        if args.dependency_parent:
            classpath += ["--classpath", str(dependency)]
        verify.run([args.neverd.resolve(), "mobile", owned, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or
                coverage["status"] != "recovered" or coverage.get("projected_method_count") or
                coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("SDK default inheritance changed original declarations or introduced a bridge")
        if any("andThen(" in k for k in rows):
            raise RuntimeError("Inherited SDK default was copied into the input inventory")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_parent else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed original inventory beyond reported helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for owner in classes:
            wanted = records[owner]
            after = actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original plain ancestor class metadata changed: " + owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original plain ancestor method metadata changed: " + identity)
        runtime_path = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_path)),
                             harness_entry], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("SDK default owner, atomic state, callback order or effects changed")

        labels = ("formal-scope", "final-slot", "complete-parent-result", "method-formal-bound") if args.member_signatures else ("formal-scope", "final-slot")
        if args.nonpublic_methods:
            labels += ("reduced-visibility", "made-public-contract", "made-protected-contract")
        for label in labels:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            if label == "formal-scope":
                receipt = change_signature(negative / "fixture/Worker.class",
                    b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>Lfixture/PlainState;Ljava/util/function/Consumer<TT;>;",
                    b"<T:Ljava/lang/Object;U:Ljava/lang/Object;>Lfixture/PlainState;Ljava/util/function/Consumer<TU;>;")
            elif label == "complete-parent-result":
                receipt = change_signature(negative / "fixture/PlainState.class",
                    b"()Ljava/util/List<Ljava/lang/String;>;",
                    b"()Ljava/util/List<Ljava/lang/Integer;>;")
            elif label == "method-formal-bound":
                receipt = change_signature(negative / "fixture/PlainState.class",
                    b"<M:Ljava/lang/Object;>(TM;)TM;",
                    b"<M:Ljava/lang/String;>(TM;)TM;")
            elif label == "reduced-visibility":
                receipt = change_method_flags(negative / "fixture/Worker.class",
                    b"(Ljava/lang/Object;)Ljava/lang/Object;", 0x0, 0x2, name=b"echo")
            elif label in ("made-public-contract", "made-protected-contract"):
                receipt = change_method_flags(negative / "fixture/PlainState.class",
                    b"(Ljava/lang/Object;)Ljava/lang/Object;", 0x0,
                    0x1 if label == "made-public-contract" else 0x4, name=b"echo")
            else:
                receipt = change_method_flags(negative / "fixture/PlainState.class",
                    b"()Ljava/lang/String;", 0x1, 0x11, name=b"toString")
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, work / (label + "-dex"))
            negative_classpath = list(classpath)
            if args.dependency_parent:
                changed_parent = work / (label + "-parent.jar")
                write_parent(negative, changed_parent)
                negative_classpath[-1] = str(changed_parent)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *negative_classpath, "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("SDK default proof accepted a different erased formal or a final ancestor override")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, immutable harness or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_parent": args.dependency_parent,
                    "member_signatures": args.member_signatures,
                    "nonpublic_methods": args.nonpublic_methods,
                    "package_override_and_method_formals": "retained" if args.nonpublic_methods else "not applicable",
                    "visibility_counterexamples": "rejected with original Code" if args.nonpublic_methods else "not applicable",
                    "value_cases": wanted_counts[0], "identity_cases": wanted_counts[1], "failure_cases": wanted_counts[2],
                    "sdk_consumer_default_declaring_owner": "retained without adding methods",
                    "atomic_state_and_ordered_callback_dispatch": "matched",
                    "complete_metadata_and_inventory": "unchanged except reported helpers",
                    "formal_scope_and_final_ancestor_counterexamples": "rejected with original Code",
                    "original_input_harness_and_sdk_bytes": "unchanged", "coverage": "recovered"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
