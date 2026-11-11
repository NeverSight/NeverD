#!/usr/bin/env python3
"""Verify closed parent marker constructors retain local type and callback scopes."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--plain-parent", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-parent-marker-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "closed_parent_marker/java"
        harness_sources = [FIXTURES / "harness" / (n + ".java") for n in
                           ("ClosedParentMarkerCaller", "ClosedParentMarkerHarness")]
        if args.plain_parent:
            copied = work / "fixture"
            shutil.copytree(fixture, copied)
            fixture = copied
            def replace_once(path, old, new):
                text = path.read_text()
                if text.count(old) != 1:
                    raise RuntimeError("Original plain parent witness anchor changed")
                path.write_text(text.replace(old, new))
            replace_once(fixture / "fixture/Parent.java", "class Parent<T extends Token>", "class Parent")
            replace_once(fixture / "fixture/Core.java", "extends Parent<LocalToken>", "extends Parent")
            sources = work / "harness-source"
            sources.mkdir()
            frozen = sources / "ClosedParentMarkerHarness.java"
            frozen.open("x").write(harness_sources[1].read_text())
            replace_once(frozen,
                "ParameterizedType parent=(ParameterizedType)core.getGenericSuperclass();require(parent.getRawType()==Parent.class&&parent.getActualTypeArguments()[0]==local&&local.getSuperclass()==Token.class);",
                "require(core.getGenericSuperclass()==Parent.class&&Parent.class.getTypeParameters().length==0&&local.getSuperclass()==Token.class);")
            harness_sources = [harness_sources[0], frozen]
        authored, originals, harness = work / "authored", work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), authored, release=8)
        verify.compile_local(harness_sources, harness, classpath=authored, release=8)
        authored_hashes = hashes(authored)
        before = class_identity.compiler_classes(authored)
        shutil.copytree(authored, originals)
        prototype = "(JLfixture/Context;FLfixture/Token;Lapi/Function;Lkotlin/jvm/internal/DefaultConstructorMarker;)V"
        marker = "Lfixture/Core;-><init>" + prototype
        witness = change_method_flags(originals / "fixture/Core.class", prototype.encode(), 0x1, 0x1001, name=b"<init>")
        (work / "marker-flag-witness.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 10 or len(all_methods) != 13:
            raise RuntimeError("Original closed parent marker inventory changed")
        declarations = {"Lfixture/" + name + ";" for name in ("Token", "Parent", "Context", "Unit")} | {"Lapi/Function;"}
        classes = all_classes - (declarations if args.dependency_inputs else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        wanted = deepcopy(before)
        wanted["Lfixture/Core;"]["methods"][marker]["access"] |= 0x1000
        for owner in records:
            for key in wanted[owner]:
                if key != "sha256" and wanted[owner][key] != records[owner].get(key):
                    raise RuntimeError("Marker flag encoding changed other metadata: " + owner + ":" + key)
        primary = "Lfixture/Core;-><init>(JLfixture/Context;FLfixture/Token;Lapi/Function;)V"
        signature = "(JLfixture/Context;FLfixture/Token;Lapi/Function<-Lfixture/Context;Lfixture/Unit;>;)V"
        parent_signature = None if args.plain_parent else "Lfixture/Parent<Lfixture/LocalToken;>;"
        if (records["Lfixture/Core;"]["signature"] != parent_signature
                or records["Lfixture/Core;"]["methods"][primary]["signature"] != signature):
            raise RuntimeError("Original closed parent or callback type relation changed")
        dependency = work / "owned-contracts.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for name in sorted(declarations):
                path = name[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_inputs:
            classpath += ["--classpath", dependency]

        def execute(runtime, label, synthetic):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         "ClosedParentMarkerHarness", str(route), str(mode), str(synthetic).lower()],
                                        f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Original closed parent marker observer changed")
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "original", True)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original closed parent marker changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original closed parent marker entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        mutations = (("lost-wide", "Facade", "new Core(bits,context,weight,token,callback,marker)", "new Core((int)bits,context,weight,token,callback,marker)", 0),
                     ("wrong-context", "Core", "this(bits,context,weight,token,(Function<? super Context,Unit>)callback);", "this(bits,null,weight,token,(Function<? super Context,Unit>)callback);", 0),
                     ("late-parent-effect", "Parent", "Trace.parentCount++;Trace.hit(7);", "Trace.hit(7);Trace.parentCount++;", 7),
                     ("late-field", "Core", "this.bits=bits;this.context=context;this.weight=weight;this.token=token;this.callback=callback;Trace.hit(2);", "this.context=context;this.weight=weight;this.token=token;this.callback=callback;Trace.hit(2);this.bits=bits;", 9))
        for label, name, old, new, mode in mutations:
            text = (fixture / ("fixture/" + name + ".java")).read_text()
            if text.count(old) != 1:
                raise RuntimeError("Original closed parent marker counterexample anchor changed")
            changed = work / label / (name + ".java")
            changed.parent.mkdir()
            changed.open("x").write(text.replace(old, new))
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=authored, release=8)
            if name == "Core":
                change_method_flags(wrong / "fixture/Core.class", prototype.encode(), 0x1, 0x1001, name=b"<init>")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                                     "ClosedParentMarkerHarness", "1", str(mode), "true"], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed parent marker" not in result.stderr:
                raise RuntimeError("Frozen observer missed incorrect closed parent marker: " + label)
        if not args.baseline_only:
            output = work / "recovered"
            verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
            coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                    or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                    or coverage.get("projected_method_count", 0) != 1 or coverage["status"] != "partial"
                    or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
                raise RuntimeError("Closed parent marker lost an original entry or changed its type scopes")
            row = next(r for r in coverage["methods"] if r["identity"] == marker)
            if row.get("projection_kind") != "synthetic-constructor" or not row.get("metadata_differences"):
                raise RuntimeError("Missing exact original synthetic marker disclosure")
            rebuilt = work / "rebuilt"
            verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                                 classpath=dependency if args.dependency_inputs else None,
                                 release=coverage.get("minimum_java_release", 8))
            actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
            helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
            expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes - {"Lapi/Function;"}}
            if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
                raise RuntimeError("Closed parent marker invented or omitted a recompiled entry")
            after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
            for owner in classes:
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                            "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                    if records[owner].get(key) != after[owner].get(key):
                        raise RuntimeError("Original closed parent marker class metadata changed: " + owner + ":" + key)
                for identity, metadata in records[owner]["methods"].items():
                    wanted = deepcopy(metadata)
                    if identity == marker:
                        wanted["access"] &= ~0x1000
                    if after[owner]["methods"].get(identity) != wanted or after_exceptions[identity] != exceptions[identity]:
                        raise RuntimeError("Original closed parent marker method metadata or Exceptions changed: " + identity)
            runtime = (rebuilt, harness, dependency) if args.dependency_inputs else (rebuilt, harness)
            if execute(runtime, "rebuilt", False) != baseline:
                raise RuntimeError("Closed parent marker changed original parameters, callback, parent effects, partial fields or failures")
        if (hashes(authored) != authored_hashes or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original specification, encoded programs or immutable callers changed")
        evidence = {"status": "baseline-only" if args.baseline_only else "success", "class_count": len(classes),
                    "method_count": len(methods), "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "dependency_inputs": args.dependency_inputs, "independent_wrong_programs": len(mutations),
                    "plain_parent": args.plain_parent,
                    ("plain_parent_and_fixed_variant_callback" if args.plain_parent else "closed_parent_owned_package_argument_and_fixed_variant_callback"): "matched",
                    "all_original_entries_and_metadata": "unchanged except disclosed synthetic constructor flag",
                    "original_scalar_values_callback_parent_effects_partial_fields_and_exact_failures": "matched"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
