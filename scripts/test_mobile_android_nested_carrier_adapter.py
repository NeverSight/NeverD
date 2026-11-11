#!/usr/bin/env python3
"""Verify nested carrier adapters retain the scoped and Object wrapper slots."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-carrier-adapter-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "nested_carrier_adapter"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk_hash = sha(args.android_jar)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 11:
            raise RuntimeError("Owned independent-seed array inventory changed")
        providers = {"Lfixture/Provider;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for route in range(4):
                for operation in range(3):
                    for mode in range(16):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),
                            "CarrierAdapterHarness",str(route),str(operation),str(mode)],f"{label}-{route}-{operation}-{mode}")
                        pattern=f"route={route},operation={operation},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Frozen nested carrier adapter observer changed")
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned independent-seed array DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original independent-seed array definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-call-disassembly")
        if "Lfixture/Provider;.adapt:(Lfixture/Pair;)Lfixture/Pair;" not in disassembly:
            raise RuntimeError("D8 lost the original private independent-seed array call")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release", 8) not in (8,25)):
            raise RuntimeError("Independent-seed array recovery changed original definitions or source contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=coverage.get("minimum_java_release",8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"]
                   for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Independent-seed array recovery invented or omitted a runtime entry/helper")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, actual = records[owner], after[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != actual.get(key):
                    raise RuntimeError("Original independent-seed array class metadata changed: " + key)
            for method, metadata in before["methods"].items():
                if actual["methods"].get(method) != metadata or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original independent-seed array method metadata or Exceptions changed: " + method)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Independent-seed array recovery changed casts, overloads, references or failure state")
        original=b"<T:Ljava/lang/Object;>(Lfixture/Pair<TT;*>;)Lfixture/Pair<Lfixture/Box<TT;>;Lfixture/Box<Ljava/lang/Object;>;>;"
        mutations=(
            ("unused-formal",original.replace(b";>(",b";U:Ljava/lang/Object;>(")),
            ("changed-bound",original.replace(b"T:Ljava/lang/Object;",b"T:Ljava/lang/Number;")),
            ("unbound-input",original.replace(b"Pair<TT;*>",b"Pair<**>")),
            ("variant-wrapper",original.replace(b"Box<TT;>",b"Box<+TT;>")),
            ("nonclosed-tail",original.replace(b"Box<Ljava/lang/Object;>",b"Box<TT;>")),
            ("changed-tail",original.replace(b"Box<Ljava/lang/Object;>",b"Box<Ljava/lang/String;>")),
        )
        for label,changed in mutations:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            receipt=change_signature(negative/"fixture/Provider.class",original,changed)
            (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
            negative_classpath=list(classpath)
            if args.dependency_provider:
                jar=work/(label+"-provider.jar")
                with zipfile.ZipFile(jar,"x") as archive:
                    archive.writestr("fixture/Provider.class",(negative/"fixture/Provider.class").read_bytes())
                negative_classpath[-1]=jar
            negative_dex=work/(label+"-dex");negative_dex.mkdir()
            verify.run([*verify.d8,"--debug","--min-api","24",*negative_classpath,"--output",negative_dex,
                        *[negative/path.relative_to(originals) for path in paths]],label+"-d8")
            rejected=work/(label+"-rejected")
            result=subprocess.run([args.neverd.resolve(),"mobile",negative_dex/"classes.dex",*negative_classpath,
                                   "--timeout","600","-o",rejected],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or rejected.exists():
                raise RuntimeError("Unproved nested carrier adapter contract was accepted: "+label)
        bodies={owner:(fixture/("java/fixture/"+owner+".java")).read_text() for owner in ("Client","Provider")}
        call="Provider.adapt(input)"
        wrong_programs=(
            ("bypass","Client",call,"(Pair)Trace.result",0,0,3),
            ("duplicate","Client","Pair<Box<Object>,Box<Object>> result=Provider.adapt(input);","Provider.adapt(input);Pair<Box<Object>,Box<Object>> result=Provider.adapt(input);",0,0,2),
            ("copied-input","Client",call,"Provider.adapt(input==null?null:new Pair(input.left,input.right))",0,0,3),
            ("early-nested-cast","Client","Trace.after=result;Trace.hit(5);return result.left.value;","Box<String> selected=result.left;Trace.after=result;Trace.hit(5);return selected.value;",1,8,1),
            ("wrong-result-slot","Client","return result.left.value;","return (String)result.right.value;",1,0,1),
            ("copied-result","Provider","return (Pair<Box<T>,Box<Object>>)result;","Pair<Box<T>,Box<Object>> old=(Pair<Box<T>,Box<Object>>)result;return new Pair<Box<T>,Box<Object>>(old.left,old.right);",0,0,1),
            ("swapped-snapshots","Provider","Trace.left=input.left;Trace.right=input.right;","Trace.left=input.right;Trace.right=input.left;",0,0,1),
            ("lost-input-read","Provider","Trace.left=input.left;Trace.right=input.right;","",0,5,1),
        )
        for label,owner,before,changed,operation,mode,count in wrong_programs:
            text=bodies[owner]
            if text.count(before)!=count:raise RuntimeError("Owned adapter counterexample source anchor changed: "+label)
            wrong=work/label/("fixture/"+owner+".java");wrong.parent.mkdir(parents=True);wrong.open("x").write(text.replace(before,changed))
            compiled=work/(label+"-classes");verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),
                                   "CarrierAdapterHarness","1",str(operation),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned nested carrier adapter changed" not in result.stderr:
                raise RuntimeError("Frozen adapter observer missed "+label)
        if ({"originals": hashes(originals), "harness": hashes(harness)} != frozen
                or sha(args.android_jar) != sdk_hash or sha(dependency) != dependency_hash):
            raise RuntimeError("Original independent-seed array inputs, frozen observers or SDK bytes changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
                  "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
                  "same_code_signature_mutations":len(mutations),"independent_wrong_programs":len(wrong_programs),
                  "original_metadata_and_exceptions":"unchanged","nested_result_identity_delayed_casts_and_input_snapshots":"matched",
                  "input_and_oracle_bytes":"unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
