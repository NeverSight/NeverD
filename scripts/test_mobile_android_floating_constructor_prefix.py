#!/usr/bin/env python3
"""Verify floating arithmetic and ordered constructor delegation prefixes."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-floating-constructor-prefix-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "floating_constructor_prefix/java"
        names = ("FloatingConstructorPrefixCaller", "FloatingConstructorPrefixHarness")
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in names], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (2, 8):
            raise RuntimeError("Original floating constructor prefix definition inventory changed")
        providers = {"Lfixture/" + n + ";" for n in ("Trace",)}
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
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+,values=(?:none|[0-9a-f:n]+)"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen floating constructor prefix observer changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original floating constructor prefix DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original floating constructor prefix definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-floating-prefix-disassembly")
        operations=set(re.findall(r"\|[0-9a-fA-F]+:\s+((?:add|sub|mul|div|rem)-(?:float|double)(?:/2addr)?)\s",disassembly))
        expected={a+"-"+t for a in ("add","sub","mul","div","rem") for t in ("float","double")}
        if {a.split("/")[0] for a in operations}!=expected or not any(a.endswith("/2addr") for a in operations):
            raise RuntimeError("Original floating arithmetic witnesses changed")
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
                    raise RuntimeError("Original floating constructor prefix class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original floating constructor prefix method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Recovered floating constructor prefix changed floating results, snapshots, identities or partial initialization")
        core=(fixture/"fixture/Core.java").read_text()
        trace=(fixture/"fixture/Trace.java").read_text()
        early=core.replace("this.add=fa;","").replace("Trace.partial=this;Trace.step(2);","Trace.partial=this;this.add=fa;Trace.step(2);")
        wrong_programs=(
            ("wrong-add","Core",core,"Trace.nameValue(),a+b,a-b","Trace.nameValue(),a-b,a-b",0),
            ("wrong-sub","Core",core,",a-b,a*b",",a+b,a*b",0),
            ("wrong-mul","Core",core,",a*b,a/b",",a+b,a/b",0),
            ("integer-divide","Core",core,",a/b,a%b",",(float)((int)a/(int)b),a%b",1),
            ("narrowed-wide-add","Core",core,",c+d,c-d",",(double)(float)(c+d),c-d",0),
            ("reversed-wide-sub","Core",core,",c-d,c*d",",d-c,c*d",0),
            ("wrong-wide-mul","Core",core,",c*d,c/d",",c+d,c/d",0),
            ("reversed-wide-div","Core",core,",c*d,c/d,c%d",",c*d,d/c,c%d",0),
            ("reversed-remainder","Core",core,",a%b,c+d",",b%a,c+d",0),
            ("reversed-wide-remainder","Core",core,",c*d,c/d,c%d,context",",c*d,c/d,d%c,context",0),
            ("reassociated-float","Core",core,"this((a+b)+e,(b-e)*a","this(a+(b+e),(b-e)*a",12),
            ("duplicated-getter","Core",core,"Trace.nameValue(),","(Trace.nameValue()==null?Trace.nameValue():Trace.nameValue()),",0),
            ("early-store","Core",core,core,early,9),
            ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",8),
        )
        for label, owner, body, anchor, changed, mode in wrong_programs:
            if body.count(anchor) != 1:
                raise RuntimeError("Owned floating constructor prefix counterexample anchor changed: " + label)
            source = work / label / ("fixture/" + owner + ".java")
            source.parent.mkdir(parents=True)
            source.open("x").write(body.replace(anchor, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), names[1], "0", str(mode)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            expected=baseline[mode]
            if result.returncode==0:
                if result.stdout==expected:
                    raise RuntimeError("Frozen floating constructor prefix observer missed "+label)
            elif "owned floating constructor prefix" not in result.stderr:
                raise RuntimeError("Invalid floating constructor counterexample "+label)
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
