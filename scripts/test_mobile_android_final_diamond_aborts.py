#!/usr/bin/env python3
"""Verify blank-final assignment diamonds with original abrupt exits."""
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-diamond-aborts-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "final_diamond_aborts/java"
        names = ("FinalDiamondAbortCaller", "FinalDiamondAbortHarness")
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (n + ".java") for n in names], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (2, 7):
            raise RuntimeError("Original final diamond abort definition inventory changed")
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
                for mode in range(32):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), names[1],
                                         str(route), str(mode)], f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen final diamond abort observer changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original final diamond abort DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original final diamond abort definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-final-diamond-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not name or name[1] != "<init>": continue
            ops = re.findall(r"\|([0-9a-fA-F]+):\s+(\S+)([^\n]*)", block[2])
            stores = [tail for _, op, tail in ops if op.startswith("iput")]
            returns = [int(pc,16) for pc,op,_ in ops if op == "return-void"]
            throws = [int(pc,16) for pc,op,_ in ops if op == "throw"]
            if (len(returns)!=1 or len(throws)!=4 or len(stores)!=5
                    or sum(".choice:" in tail for tail in stores)!=2
                    or sum(op.startswith("if-") for _,op,_ in ops)<5
                    or not any(pc>returns[0] for pc in throws)):
                raise RuntimeError("Original conditional assignment/throw witness changed")
            witnessed=True
        if not witnessed: raise RuntimeError("Missing original final diamond constructor")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Constructor comparison recovery changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_provider else None, release=8)
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
                    raise RuntimeError("Original final diamond abort class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original final diamond abort method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Recovered final diamond abort changed assignment order, snapshots, identity or abrupt state")
        core = (fixture / "fixture/Core.java").read_text()
        late_first = core.replace("first=seed;", "").replace("if (alternate==null)", "first=seed;\n        if (alternate==null)")
        wrong_programs = (
            ("lost-first", "Core", core, "first=seed;", "first=null;", 0),
            ("reversed-choice", "Core", core, "if (alternate==null)", "if (alternate!=null)", 0),
            ("lost-accept", "Core", core, "if (!Trace.accept(seed))", "if (false)", 0),
            ("lost-inline-exit", "Core", core, "if (Trace.inlineAbort)", "if (false)", 3),
            ("lost-alternate-exit", "Core", core, "if (Trace.rejectAlternate)", "if (false)", 4),
            ("changed-choice-snapshot", "Core", core, "choice=Trace.choose(seed);", "choice=(Trace.choose(seed)==seed?Trace.produced:Trace.produced);", 0),
            ("changed-last-snapshot", "Core", core, "last=Trace.finish(seed);", "last=(Trace.finish(seed)==seed?Trace.finalValue:Trace.finalValue);", 0),
            ("early-stamp", "Core", core, "Trace.step(12);\n        stamp=Trace.stamp;", "stamp=Trace.stamp;\n        Trace.step(12);", 0),
            ("late-stamp", "Core", core, "stamp=Trace.stamp;\n        Trace.step(13);", "Trace.step(13);\n        stamp=Trace.stamp;", 0),
            ("early-finish", "Core", core, "Trace.step(10);\n        last=Trace.finish(seed);", "last=Trace.finish(seed);\n        Trace.step(10);", 0),
            ("wrapped-branch-failure", "Core", core, "throw Trace.problem(4);", "throw new IllegalStateException(Trace.problem(4));", 2),
            ("late-first", "Core", core, core, late_first, 8),
            ("narrowed-stamp", "Core", core, "stamp=Trace.stamp;", "stamp=(int)Trace.stamp;", 0),
            ("lost-late-exit", "Core", core, "if (Trace.abortAfter)", "if (false)", 5),
        )
        for label, owner, body, anchor, changed, mode in wrong_programs:
            if body.count(anchor) != 1:
                raise RuntimeError("Owned final diamond abort counterexample anchor changed: " + label)
            source = work / label / ("fixture/" + owner + ".java")
            source.parent.mkdir(parents=True)
            source.open("x").write(body.replace(anchor, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), names[1], "0", str(mode)],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned final diamond aborts" not in result.stderr:
                raise RuntimeError("Frozen final diamond abort observer missed " + label)
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash:
            raise RuntimeError("Frozen original compiler, observer or SDK bytes changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 128, "attempts_per_case": 2, "wrong_programs": len(wrong_programs),
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
