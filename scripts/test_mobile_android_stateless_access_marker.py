#!/usr/bin/env python3
"""Verify a stateless anonymous implementation reused as an unused access marker."""
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
    if error.name != "scripts":
        raise
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
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-stateless-access-marker-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        fixture = FIXTURES / "stateless_access_marker/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        harness_name = "StatelessAccessMarkerHarness"
        verify.compile_local([FIXTURES / "harness" / (harness_name + ".java")], harness,
                             classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd.resolve()) + "\n")
        records = class_identity.compiler_classes(originals)
        exceptions = exception_inventory(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (6, 11):
            raise RuntimeError("Owned stateless marker inventory changed")
        excluded = {"Lfixture/Context;", "Lfixture/Probe;"} if args.dependency_inputs else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        sdk = work / "sdk"
        sdk.mkdir()
        dependencies = []
        if excluded:
            jar = work / "owned-inputs.jar"
            with zipfile.ZipFile(jar, "x") as archive:
                for owner in sorted(excluded):
                    name = owner[1:-1] + ".class"
                    path = sdk / name
                    path.parent.mkdir(exist_ok=True)
                    shutil.copyfile(originals / name, path)
                    archive.writestr(name, path.read_bytes())
            dependencies.append(jar)
        input_hashes = {str(p): sha(p) for p in [args.android_jar.resolve(), *dependencies]}
        classpath = [part for p in [args.android_jar.resolve(), *dependencies]
                     for part in ("--classpath", str(p))]

        def execute(runtime, label, marker_name="fixture.Core$1", rebuilt=False):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         harness_name, str(route), str(mode), marker_name, str(rebuilt).lower()], f"{label}-{route}-{mode}")
                    stage = 1 if mode in (3, 4, 5, 14) else 2 if mode in (6, 7) else 3
                    expected = [f"route={route},mode={mode},attempt={attempt},stage={stage}" for attempt in range(2)]
                    if result.splitlines() != expected:
                        raise RuntimeError("Independent final-field/effect oracle changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        paths = [p.relative_to(originals) for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(root, name, declarations=None):
            destination = work / name
            destination.mkdir()
            declared = classpath if declarations is None else declarations
            verify.run([*verify.d8, "--debug", "--min-api", "24", *declared, "--output", destination,
                        *[root / p for p in paths]], name)
            return destination / "classes.dex"

        wrongs = [('lost-values', 'Core', 'this.values = values;', 'this.values = null;', 0, 0), ('lost-context', 'Core', 'this.context = context;', 'this.context = null;', 0, 0), ('lost-probe', 'Core', 'this.probe = probe;', 'this.probe = null;', 0, 0), ('lost-marker', 'Core', 'marker = probe.observe(this, values, context);', 'probe.observe(this, values, context); marker = null;', 0, 0), ('duplicate-probe', 'Core', 'marker = probe.observe(this, values, context);', 'probe.observe(this, values, context); marker = probe.observe(this, values, context);', 0, 0), ('early-failure', 'Core', 'marker = probe.observe(this, values, context);', 'if (context != null && context.failure != null) throw context.failure; marker = probe.observe(this, values, context);', 6, 0), ('lost-completion', 'Core', 'stage = 3;', 'stage = 0;', 0, 0), ('wrong-factory-values', 'Core', 'return new Core(values, (Handler) implementation, context, probe);', 'return new Core(null, (Handler) implementation, context, probe);', 0, 0)]
        wrongs += [
            ("lost-private-context", "Core", "this.handler = handler;", "this.handler = null;", 0, 0),
            ("public-handler", "Core", "private interface Handler", "public interface Handler", 0, 0),
            ("lost-anonymous-input", "Core", "observed = value;", "observed = null;", 0, 0),
            ("repeated-anonymous-call", "Core", "calls++;", "calls += 2;", 0, 0),
            ("lost-anonymous-result", "Core", "return result;", "return null;", 0, 0),
            ("early-anonymous-result", "Core", "if (action != null) action.run();", "Object saved = result; if (action != null) action.run(); result = saved;", 0, 0),
            ("wrapped-anonymous-failure", "Core", "if (action != null) action.run();", "try { if (action != null) action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 2, 0),
            ("observed-unused-marker", "Core", "latest = this;", "if (handler != null) handler.apply(values); latest = this;", 0, 3)]
        for name, owner, before, changed, mode, route in wrongs:
            altered = work / (name + "-wrong-source")
            shutil.copytree(fixture, altered)
            path = altered / ("fixture/" + owner + ".java")
            text = path.read_text()
            assert text.count(before) == 1
            path.write_text(text.replace(before, changed))
            wrong = work / (name + "-wrong-classes")
            verify.compile_local(sorted(altered.rglob("*.java")), wrong, release=8)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (wrong, harness))),
                                     harness_name, str(route), str(mode), "fixture.Core$1", "false"], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (name + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed access marker" not in result.stderr:
                raise RuntimeError("Frozen caller missed independently compiled wrong final-field flow: " + name)
        source_dex = dex(originals, "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original closed access marker entry")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"] != "partial" or coverage["projected_method_count"] != 5
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Original closed access marker entries or projection accounting changed")
        expected_projected = {
            "Lfixture/Core;-><init>(Ljava/util/Set;Lfixture/Core$Handler;Lfixture/Context;Lfixture/Probe;Lfixture/Core$1;)V",
            "Lfixture/Core$Builder;->create(Ljava/util/Set;Lfixture/Context;Lfixture/Probe;)Lfixture/Core;",
            "Lfixture/Core;-><clinit>()V",
            "Lfixture/Core$1;-><init>()V",
            "Lfixture/Core$1;->apply(Ljava/lang/Object;)Ljava/lang/Object;"}
        if ({identity for identity, row in rows.items() if row["status"] == "source-projected"} != expected_projected
                or any(rows[identity].get("projection_kind") != "anonymous-to-named-member" for identity in expected_projected)):
            raise RuntimeError("Anonymous marker mapping changed an unrelated method role")
        bindings = coverage["class_source_bindings"]
        if (len(bindings) != 1 or bindings[0]["class"] != "Lfixture/Core$1;"
                or bindings[0]["binding_kind"] != "anonymous-to-named-member"
                or bindings[0]["binary_name_status"] != "changed"
                or len(bindings[0]["metadata_differences"]) != 5):
            raise RuntimeError("Missing complete constructorless marker source mapping")
        binding = bindings[0]
        replacement = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"
        marker_name = replacement[1:-1].replace("/", ".")
        def mapped(value):
            if isinstance(value, str): return value.replace(binding["class"], replacement)
            if isinstance(value, list): return [mapped(v) for v in value]
            if isinstance(value, dict): return {mapped(k): mapped(v) for k, v in value.items()}
            return value
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=sdk if excluded else None, release=8)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helper_rows = coverage["generated_source_helpers"]
        helpers = {mapped(r["class"] + "->" + r["name"] + r["prototype"]) for r in helper_rows}
        if (actual_classes != {mapped(c) for c in classes}
                or set(actual_methods) != {mapped(m) for m in methods} | helpers):
            raise RuntimeError("Closed access marker original definitions or helper roles changed")
        defaults = [r for r in helper_rows if r["kind"] == "default-constructor"]
        if defaults:
            raise RuntimeError("An existing marker constructor must not be replaced by a helper")
        if any(r["kind"] != "throw-helper"
               or r["kind"] == "throw-helper" and r.get("static") is not True for r in helper_rows):
            raise RuntimeError("Unexpected closed access marker generated helper")
        after = class_identity.compiler_classes(rebuilt)
        after_exceptions = exception_inventory(rebuilt)
        differences = {}
        for owner in classes:
            before_record = mapped(records[owner]); after_record = after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before_record.get(key) != after_record.get(key):
                    if owner != binding["class"] or key not in ("access", "inner_class", "enclosing_method"):
                        raise RuntimeError("Undisclosed original class metadata changed: " + owner + ":" + key)
                    if key == "enclosing_method" and (before_record[key] != {
                            "owner": binding["source_enclosing_class"], "name": None, "prototype": None}
                            or after_record.get(key) is not None):
                        raise RuntimeError("Unexpected anonymous-to-member enclosure metadata")
                    differences[owner + ":" + key] = [before_record.get(key), after_record.get(key)]
            for identity, metadata in records[owner]["methods"].items():
                expected = mapped(metadata)
                if metadata["access"] & 0x1000:
                    expected["access"] &= ~0x1000
                    if (not rows[identity].get("metadata_differences")
                            or not any("synthetic" in d for d in rows[identity]["metadata_differences"])):
                        raise RuntimeError("Synthetic constructor flag change lacks a report receipt")
                    differences[identity + ":access"] = [metadata["access"], expected["access"]]
                if (after_record["methods"].get(mapped(identity)) != expected
                        or after_exceptions[mapped(identity)] != mapped(exceptions[identity])):
                    raise RuntimeError("Undisclosed original method metadata or Exceptions changed: " + identity)
        marker = after[replacement]
        if (marker["access"] & 0x1000 or marker["inner_class"]["name"] != binding["source_name"]
                or marker["inner_class"]["outer"] != binding["source_enclosing_class"]):
            raise RuntimeError("Rebuilt marker differs from its exact named binding")
        (work / "disclosed-metadata-differences.json").open("x").write(json.dumps(differences, indent=2) + "\n")
        if execute((rebuilt, harness, *([sdk] if excluded else [])), "recovered", marker_name, True) != baseline:
            raise RuntimeError("Original raw forwarding, partial state or exact failure changed")
        controls = []
        signature = b"(Ljava/util/Set<Ljava/lang/Integer;>;Lfixture/Core$Handler;Lfixture/Context;Lfixture/Probe;)V"
        changes = (
            ("unknown-element", signature.replace(b"java/lang/Integer", b"fixture/Missing")),
            ("extra-slot", signature.replace(b"java/lang/Integer;>", b"java/lang/Integer;Ljava/lang/Object;>")),
            ("foreign-formal", signature.replace(b"Ljava/lang/Integer;", b"TUnknown;")),
            ("changed-erasure", signature.replace(b"java/util/Set", b"java/util/List")),
        )
        for label, changed in changes:
            altered = work / (label + "-originals")
            shutil.copytree(originals, altered)
            target = altered / "fixture/Core.class"
            code = method_code(target.read_bytes())
            receipt = change_signature(target, signature, changed)
            if method_code(target.read_bytes()) != code:
                raise RuntimeError("Owned member Signature control changed Code")
            negative_dex = dex(altered, label + "-dex")
            rejected = work / (label + "-rejected")
            result = subprocess.run([args.neverd.resolve(), "mobile", negative_dex, *classpath, "-o", rejected],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Unproved owned member constructor Signature accepted: " + label)
            controls.append((label, receipt))
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or any(sha(Path(p)) != value for p, value in input_hashes.items())):
            raise RuntimeError("Frozen owned factory inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_inputs": args.dependency_inputs, "fresh_jvm_cases": len(baseline),
                    "attempts_per_case": 2, "same_code_counterexamples": [row[0] for row in controls],
                    "independent_compiler_counterexamples": len(wrongs),
                    "original_anonymous_constructor_and_body": "preserved",
                    "private_context_and_unused_marker": "identity and effects matched"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
