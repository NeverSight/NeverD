#!/usr/bin/env python3
"""Verify closed constructor types retain exact owned private sibling scope."""
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
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-inputs", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-sibling-constructor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "private_sibling_constructor/java"
        prefix = "PrivateSiblingConstructor"
        originals, harness = work / "originals", work / "harness"
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (prefix + suffix + ".java") for suffix in ("Caller", "Harness")], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (7, 15):
            raise RuntimeError("Original private sibling constructor inventory changed")
        signature = "(Lfixture/Core$Context;Ljava/util/List<Lfixture/Core$Value;>;Lfixture/Probe;)V"
        target = "Lfixture/Core$Entry;-><init>(Lfixture/Core$Context;Ljava/util/List;Lfixture/Probe;)V"
        if records["Lfixture/Core$Entry;"]["methods"][target]["signature"] != signature:
            raise RuntimeError("Original private sibling constructor Signature changed")
        excluded = {"Lfixture/Probe;"} if args.dependency_inputs else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-inputs.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(excluded):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        inputs = {p: sha(p) for p in (args.android_jar.resolve(), dependency)}
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded: classpath += ["--classpath", dependency]

        def execute(runtime, label, marker="fixture.Core$1", rebuilt=False):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), prefix + "Harness",
                        str(route), str(mode), marker, str(rebuilt).lower()], f"{label}-{route}-{mode}")
                    stage = 1 if mode in (3, 4, 5, 6, 14) else 2 if mode in (7, 8, 9) else 3
                    expected = [f"route={route},mode={mode},attempt={attempt},stage={stage}" for attempt in range(2)]
                    if text.splitlines() != expected: raise RuntimeError("Frozen private sibling constructor observer changed")
                    outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        wrongs = (
            ("lost-values", "this.values=values;", "this.values=null;", 0),
            ("lost-context", "this.context=context;", "this.context=null;", 0),
            ("lost-marker", "marker=probe.observe(this,values,context);", "probe.observe(this,values,context);marker=null;", 0),
            ("duplicate-probe", "marker=probe.observe(this,values,context);", "probe.observe(this,values,context);marker=probe.observe(this,values,context);", 0),
            ("lost-completion", "stage=3;", "stage=2;", 0),
            ("wrapped-checked", "throw Trace.checked;", "throw new IOException(Trace.checked);", 8),
            ("wrong-context", "new Entry((Context)context,(List)values,probe)", "new Entry(null,(List)values,probe)", 0),
            ("copied-list", "new Entry((Context)context,(List)values,probe)", "new Entry((Context)context,new java.util.ArrayList(values),probe)", 0),
            ("lost-private-value", "private static final class Value", "public static final class Value", 0),
            ("lost-context-result", "Trace.hit(5);return payload;", "Trace.hit(5);return null;", 0),
        )
        for label, before, after, mode in wrongs:
            source = work / (label + "-source")
            shutil.copytree(fixture, source)
            path = source / "fixture/Core.java"
            text = path.read_text()
            if text.count(before) != 1: raise RuntimeError("Private sibling constructor counterexample anchor changed")
            path.write_text(text.replace(before, after))
            compiled = work / (label + "-classes")
            verify.compile_local(sorted(source.rglob("*.java")), compiled, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, harness))), prefix + "Harness",
                "0", str(mode), "fixture.Core$1", "false"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + ".log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned private sibling constructor" not in result.stderr:
                raise RuntimeError("Observer missed incorrect private sibling constructor behavior: " + label)
        paths = [p.relative_to(originals) for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, label):
            out = work / label
            out.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", out, *[root / p for p in paths]], label)
            return out / "classes.dex"

        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original private sibling constructor entry")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        projected = {
            "Lfixture/Core;->value(Ljava/lang/Object;)Ljava/lang/Object;",
            "Lfixture/Core;->context(Ljava/lang/Object;)Ljava/lang/Object;",
            "Lfixture/Core;->create(Ljava/lang/Object;Ljava/util/List;Lfixture/Probe;)Ljava/lang/Object;",
            "Lfixture/Core$Value;-><init>(Ljava/lang/Object;Lfixture/Core$1;)V",
            "Lfixture/Core$Context;-><init>(Ljava/lang/Object;Lfixture/Core$1;)V",
            "Lfixture/Core$Entry;-><init>(Lfixture/Core$Context;Ljava/util/List;Lfixture/Probe;Lfixture/Core$1;)V",
        }
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage["projected_method_count"] != len(projected) or coverage.get("generic_signature_projections")
                or {m for m, r in rows.items() if r["status"] == "source-projected"} != projected
                or any(rows[m].get("projection_kind") != "anonymous-to-named-member" for m in projected)):
            raise RuntimeError("Private sibling constructor recovery changed original entries or projection accounting")
        bindings = coverage["class_source_bindings"]
        if (len(bindings) != 1 or bindings[0]["class"] != "Lfixture/Core$1;"
                or bindings[0]["binding_kind"] != "anonymous-to-named-member" or bindings[0]["binary_name_status"] != "changed"
                or len(bindings[0]["metadata_differences"]) < 5):
            raise RuntimeError("Missing exact constructorless marker projection")
        binding = bindings[0]
        replacement = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        def mapped(value):
            if isinstance(value, str): return value.replace(binding["class"], replacement)
            if isinstance(value, list): return [mapped(v) for v in value]
            if isinstance(value, dict): return {mapped(k): mapped(v) for k, v in value.items()}
            return value
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency if excluded else None,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helper_rows = coverage.get("generated_source_helpers", [])
        helpers = {mapped(r["class"] + "->" + r["name"] + r["prototype"]) for r in helper_rows}
        allowed = {mapped(c) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes} | {replacement + "-><init>()V"}
        if actual_classes != {mapped(c) for c in classes} or set(actual_methods) != {mapped(m) for m in methods} | helpers or not helpers <= allowed:
            raise RuntimeError("Private sibling constructor recovery invented or omitted a runtime entry")
        if (sum(r["kind"] == "default-constructor" and r["class"] == binding["class"] for r in helper_rows) != 1
                or any(r["kind"] not in ("throw-helper", "default-constructor") for r in helper_rows)):
            raise RuntimeError("Unexpected private sibling constructor generated helper")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        differences = {}
        for owner in classes:
            before_record, after_record = mapped(records[owner]), after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before_record.get(key) != after_record.get(key):
                    if owner != binding["class"] or key not in ("access", "inner_class", "enclosing_method"):
                        raise RuntimeError("Undisclosed private sibling class metadata changed: " + owner + ":" + key)
                    if key == "enclosing_method" and (before_record[key] != {"owner": binding["source_enclosing_class"], "name": None, "prototype": None} or after_record.get(key) is not None):
                        raise RuntimeError("Unexpected marker enclosure metadata")
                    differences[owner + ":" + key] = [before_record.get(key), after_record.get(key)]
            for identity, metadata in records[owner]["methods"].items():
                expected = mapped(metadata)
                if metadata["access"] & 0x1000:
                    if "-><init>(" not in identity: raise RuntimeError("Unexpected synthetic method")
                    expected["access"] &= ~0x1000
                    if not any("synthetic" in d for d in rows[identity].get("metadata_differences", [])):
                        raise RuntimeError("Synthetic access flag change lacks its original report receipt")
                    differences[identity + ":access"] = [metadata["access"], expected["access"]]
                if after_record["methods"].get(mapped(identity)) != expected or after_exceptions[mapped(identity)] != mapped(exceptions[identity]):
                    raise RuntimeError("Original private sibling method metadata or Exceptions changed: " + identity)
        (work / "disclosed-metadata-differences.json").open("x").write(json.dumps(differences, indent=2) + "\n")
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt", replacement[1:-1].replace("/", "."), True) != baseline:
            raise RuntimeError("Private sibling constructor forwarding, identity or effects changed")
        original_signature = signature.encode()
        negatives = (
            ("missing-leaf", original_signature.replace(b"Core$Value;", b"Missing;")),
            ("unbound-leaf", original_signature.replace(b"Lfixture/Core$Value;", b"TMissing;")),
            ("wrong-carrier", original_signature.replace(b"java/util/List<", b"java/util/Set<")),
            ("method-formal", b"<T:Ljava/lang/Object;>" + original_signature),
        )
        for label, changed in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            file = negative / "fixture/Core$Entry.class"
            before_code = method_code(file.read_bytes())
            receipt = change_signature(file, original_signature, changed)
            if method_code(file.read_bytes()) != before_code: raise RuntimeError("Signature control changed Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_dex = dex(negative, label + "-dex")
            rejected = work / (label + "-rejected")
            result = subprocess.run([args.neverd.resolve(), "mobile", negative_dex, *classpath, "-o", rejected], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists(): raise RuntimeError("Unproved private sibling constructor Signature accepted: " + label)
        if {"originals": hashes(originals), "harness": hashes(harness)} != frozen or any(sha(p) != h for p, h in inputs.items()):
            raise RuntimeError("Original private sibling constructor inputs or observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "dependency_inputs": args.dependency_inputs,
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "independent_wrong_programs": len(wrongs),
                    "same_code_signature_mutations": len(negatives), "disclosed_marker_projection": "accounted", "all_original_entries_and_metadata": "accounted"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0

if __name__ == "__main__": raise SystemExit(main())
