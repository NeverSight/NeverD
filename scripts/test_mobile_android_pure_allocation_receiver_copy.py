#!/usr/bin/env python3
"""Verify independently authored pure receiver copies against frozen JVM callers."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--chain", action="store_true")
    parser.add_argument("--early", action="store_true")
    parser.add_argument("--conditional-preparation", action="store_true")
    parser.add_argument("--inverse-condition", action="store_true")
    args = parser.parse_args()
    if args.conditional_preparation and (args.chain or args.early):
        parser.error("Conditional preparation requires the final direct receiver copy")
    if args.inverse_condition and not args.conditional_preparation:
        parser.error("Inverse condition requires conditional preparation")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-pure-receiver-copy-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "pure_allocation_receiver_copy"
        originals, harness = work / "originals", work / "harness"
        sources=sorted((fixture / "java").rglob("*.java"))
        observers=[FIXTURES / "harness" / (n + ".java") for n in
                   ("PureAllocationReceiverCopyCaller", "PureAllocationReceiverCopyHarness")]
        value=("enabled?~bits:bits" if args.inverse_condition else "enabled?bits:~bits") if args.conditional_preparation else "bits"
        construction="new Target("+value+",enabled,tag,active)"
        if args.conditional_preparation:
            text=(fixture/"java/fixture/Client.java").read_text()
            if text.count("new Target(bits,enabled,tag,active)")!=1:raise RuntimeError("Original conditional receiver reference anchor changed")
            changed=work/"reference/Client.java";changed.parent.mkdir();changed.open("x").write(text.replace("new Target(bits,enabled,tag,active)",construction))
            sources=[changed if source.name=="Client.java" else source for source in sources]
            text=observers[1].read_text()
            if text.count("length>=2?bits:0")!=1:raise RuntimeError("Original frozen conditional observer anchor changed")
            changed=work/observers[1].name;changed.open("x").write(text.replace("length>=2?bits:0","length>=2?("+value+"):0"));observers[1]=changed
        verify.compile_local(sources, originals, release=8)
        verify.compile_local(observers,harness,classpath=originals,release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 3 or len(all_methods) != 5:
            raise RuntimeError("Original pure receiver copy specification inventory changed")
        classes = {"Lfixture/Client;"}
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-observers.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for name in ("Target", "Trace"):
                jar.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()

        def execute(runtime, label):
            outputs = []
            for route in range(4):
                for mode in range(9):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                         "PureAllocationReceiverCopyHarness", str(route), str(mode)], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Original pure receiver copy observer changed")
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "original")
        source = work / "smali"
        source.mkdir()
        text = (fixture / "smali/Client.smali").read_text()
        alias = "    move-object/from16 v3, v0\n"
        if text.count(alias) != 1:
            raise RuntimeError("Authored receiver alias anchor changed")
        replacement = ("    move-object/from16 v2, v0\n    move-object/from16 v3, v2\n"
                       if args.chain else alias)
        text = text.replace(alias, "" if args.early else replacement)
        if args.early:
            before = "    new-instance v0, Lfixture/Target;\n"
            if text.count(before) != 1:
                raise RuntimeError("Authored allocation anchor changed")
            text = text.replace(before, before + replacement)
        if args.conditional_preparation:
            before="    move-wide/from16 v4, p0\n"
            if text.count(before)!=1:raise RuntimeError("Authored conditional preparation anchor changed")
            test="if-eqz" if args.inverse_condition else "if-nez"
            text=text.replace(before,before+"    "+test+" v6, :ready\n    not-long v4, v4\n    :ready\n")
        path = source / "Client.smali"
        path.open("x").write(text)
        input_hash = hashlib.sha256(path.read_bytes()).hexdigest()
        body = text.split("    new-instance v0, Lfixture/Target;\n", 1)[1].split("    invoke-direct/range", 1)[0]
        operations = [line.strip().split()[0] for line in body.splitlines() if line.strip() and not line.strip().startswith(":" )]
        expected = {"move/from16", "move-wide/from16", "move-object/from16"}
        if args.conditional_preparation:expected |= {"if-eqz","if-nez","not-long"}
        if set(operations) - expected or len(operations) != (7 if args.conditional_preparation else 6 if args.chain else 5):
            raise RuntimeError("Pure constructor preparation gained an effect or lost its copies")
        (work / "owned-bytecode-witness.json").open("x").write(json.dumps({
            "input": "independently authored smali; not claimed compiler output", "registers": 15,
            "parameter_words": 5, "constructor_words": 6, "preparation_operations": operations,
            "chain": args.chain, "early": args.early, "conditional_preparation":args.conditional_preparation,
            "original_allocation_word_reused_as_wide_after_constructor": True,
            "sha256": input_hash}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source, "--classpath", args.android_jar.resolve(),
                    "--classpath", dependency, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != 1
                or coverage["method_count"] != 2 or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or coverage.get("minimum_java_release",8)!=(14 if args.conditional_preparation else 8)):
            raise RuntimeError("Pure receiver copy recovery changed original entries or metadata")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {"Lfixture/Client;->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Pure receiver copy invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original pure receiver copy class metadata changed: " + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original pure receiver copy method metadata changed: " + identity)
        if execute((rebuilt, harness, dependency), "rebuilt") != baseline:
            raise RuntimeError("Pure receiver copy changed returned identity, parameter values, partial fields or failures")
        mutations = (("lost-result", "Client", "return "+construction+";", construction+";return null;", 0),
                     ("lost-wide", "Client", construction, "new Target((int)("+value+"),enabled,tag,active)", 0),
                     ("swapped-flags", "Client", construction, "new Target("+value+",active,tag,enabled)", 0),
                     ("late-field", "Target", "this.bits=bits;Trace.hit(2);", "Trace.hit(2);this.bits=bits;", 5))
        if args.conditional_preparation:
            other="enabled?bits:~bits" if args.inverse_condition else "enabled?~bits:bits"
            mutations += (("wrong-condition","Client",value,other,1),)
        for label, name, old, new, mode in mutations:
            original = next(path for path in sources if path.name==name+".java").read_text()
            if original.count(old) != 1:
                raise RuntimeError("Original pure receiver copy counterexample anchor changed")
            changed = work / label / (name + ".java")
            changed.parent.mkdir()
            changed.open("x").write(original.replace(old, new))
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                                     "PureAllocationReceiverCopyHarness", "1", str(mode)], text=True, capture_output=True,
                                    timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned pure allocation receiver copy" not in result.stderr:
                raise RuntimeError("Frozen observer missed incorrect pure receiver copy: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(path.read_bytes()).hexdigest() != input_hash):
            raise RuntimeError("Original specification, authored bytecode or immutable callers changed")
        evidence = {"status": "success", "class_count": 1, "method_count": 2, "fresh_jvm_cases": len(baseline),
                    "attempts_per_case": 2, "chain": args.chain, "early": args.early,
                    "conditional_preparation":args.conditional_preparation,"inverse_condition":args.inverse_condition,
                    "independent_wrong_programs": len(mutations),
                    "input": "independently authored pure receiver alias smali", "all_original_client_entries_and_metadata": "unchanged",
                    "one_object_identity_wide_boolean_scalar_parameters_partial_fields_and_exact_failures": "matched"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
