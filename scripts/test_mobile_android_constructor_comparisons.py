#!/usr/bin/env python3
"""Verify shared scalar comparisons and conditional allocations before constructor delegation."""
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
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-constructor-comparisons-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "constructor_comparisons/java"
        names = ("ConstructorComparisonCaller", "ConstructorComparisonHarness")
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in names], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (3, 10):
            raise RuntimeError("Original constructor comparison definition inventory changed")
        providers = {"Lfixture/" + n + ";" for n in ("Trace", "Value")}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
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
                        raise RuntimeError("Frozen constructor comparison observer changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original constructor comparison DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original constructor comparison definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-instance-read-disassembly")
        witnessed = set()
        guarded = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            prototype = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "<init>" or not prototype: continue
            ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
            witnessed.update(op for op, _ in ops if op.startswith("cmp"))
            if prototype[1] == "(D)V":
                calls = [i for i, (op, tail) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Core;.<init>:(ILjava/lang/Object;)V" in tail]
                allocations = [i for i, (op, tail) in enumerate(ops) if op == "new-instance" and "Lfixture/Value;" in tail]
                chooses = [i for i, (op, tail) in enumerate(ops) if op.startswith("invoke-static") and "Lfixture/Trace;.choose:(D)D" in tail]
                if (len(calls) != 1 or len(allocations) != 1 or len(chooses) != 2 or max(chooses) >= allocations[0]
                        or allocations[0] >= calls[0] or sum(op.startswith("cmp") for op, _ in ops) != 2):
                    raise RuntimeError("Original comparison/allocation constructor witness changed")
                guarded = True
        if witnessed != {"cmp-long", "cmpl-float", "cmpg-float", "cmpl-double", "cmpg-double"} or not guarded:
            raise RuntimeError("Missing original comparison opcode or conditional allocation")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Constructor comparison recovery changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_provider else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or helpers != expected_helpers or set(actual_methods) != methods | helpers:
            raise RuntimeError("Constructor comparison source invented or omitted an original runtime definition/helper")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before = records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after[owner].get(key):
                    raise RuntimeError("Original constructor comparison class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original constructor comparison method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Recovered constructor comparison changed ordering, float bits, identity or failure state")
        core = (fixture / "fixture/Core.java").read_text()
        value = (fixture / "fixture/Value.java").read_text()
        trace = (fixture / "fixture/Trace.java").read_text()
        long_body = "public Core(long first,long second) { this(first<second ? -1 : first==second ? 0 : 1,null); }"
        float_body = long_body.replace("long", "float")
        double_body = long_body.replace("long", "double")
        early_result = core.replace("this.result=result;", "").replace("Trace.partial=this;Trace.step(3);", "Trace.partial=this;this.result=result;Trace.step(3);")
        late_publication = core.replace("Trace.partial=this;Trace.step(3);", "Trace.step(3);Trace.partial=this;")
        wrong_programs = (
            ("reversed-long", "Core", core, long_body, long_body.replace("first<second", "first>second"), 0),
            ("overflow-long", "Core", core, long_body, "public Core(long first,long second) { this((int)(first-second),null); }", 0),
            ("float-total-order", "Core", core, float_body, "public Core(float first,float second) { this(Float.compare(first,second),null); }", 1),
            ("double-total-order", "Core", core, double_body, "public Core(double first,double second) { this(Double.compare(first,second),null); }", 2),
            ("wrong-first-branch", "Core", core, "Trace.choose(input)==2.0 ? 7 : 9", "Trace.choose(input)!=2.0 ? 7 : 9", 3),
            ("eager-allocation", "Core", core, "Trace.choose(input)==2.0 ? Trace.identity : new Value(input)", "Trace.choose(input)==2.0 ? new Value(input) : new Value(input)", 3),
            ("wrong-capture", "Core", core, "new Value(input)", "new Value(Trace.second)", 4),
            ("duplicated-choose", "Core", core, "Trace.choose(input)==2.0 ? 7 : 9", "(Trace.choose(input)+Trace.choose(input))==4.0 ? 7 : 9", 3),
            ("lost-choose", "Core", core, "Trace.choose(input)==2.0 ? 7 : 9", "Trace.first==2.0 ? 7 : 9", 3),
            ("late-publication", "Core", core, core, late_publication, 13),
            ("early-result", "Core", core, core, early_result, 13),
            ("wrapped-failure", "Trace", trace, "step(index);", "try { step(index); } catch (RuntimeException failure) { throw new IllegalArgumentException(); }", 9),
            ("narrowed-capture", "Value", value, "this.input=input;", "this.input=(double)(float)input;", 6),
        )
        for label, owner, body, anchor, changed, mode in wrong_programs:
            if body.count(anchor) != 1:
                raise RuntimeError("Owned constructor comparison counterexample anchor changed: " + label)
            source = work / label / ("fixture/" + owner + ".java")
            source.parent.mkdir(parents=True)
            source.open("x").write(body.replace(anchor, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), names[1], "0", str(mode)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned constructor comparisons" not in result.stderr:
                raise RuntimeError("Frozen constructor comparison observer missed " + label)
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash:
            raise RuntimeError("Frozen original compiler, observer or SDK bytes changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 64, "attempts_per_case": 2, "wrong_programs": len(wrong_programs),
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
