#!/usr/bin/env python3
"""Verify anchored private generic array consumers with frozen JVM observers."""
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
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--instance-helper",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-anchored-array-void-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"anchored_array_void/java"
        if args.instance_helper:
            authored=work/"instance-source";shutil.copytree(fixture,authored);fixture=authored
            path=fixture/"fixture/Owner.java";text=path.read_text()
            for before,after in (("private static <T>","private <T>"),
                                 ("{ append(sink, values); }","{ new Owner().append(sink, values); }")):
                if text.count(before)!=1:raise RuntimeError("Private instance source anchor changed")
                text=text.replace(before,after)
            path.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(name+".java") for name in
            ("AnchoredArrayVoidCaller","AnchoredArrayVoidHarness")],harness,classpath=originals,release=11)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=3 or len(all_methods)!=7:raise RuntimeError("Original anchored array inventory changed")
        classes=all_classes-({"Lfixture/Bucket;"} if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        identity="Lfixture/Owner;->append(Lfixture/Bucket;[Ljava/lang/Object;)V"
        signature=b"<T:Ljava/lang/Object;>(Lfixture/Bucket<TT;>;[TT;)V"
        if records["Lfixture/Owner;"]["methods"][identity]["signature"]!=signature.decode():
            raise RuntimeError("Original invariant carrier and array formal changed")
        dependency=work/"owned-carrier.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Bucket.class",(originals/"fixture/Bucket.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(22):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"AnchoredArrayVoidHarness",
                        str(route),str(mode),str(args.instance_helper).lower()],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original anchored array observer matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original anchored array DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original anchored array entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Anchored array recovery changed original entries or contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Bucket;"}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Anchored array recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original anchored array class metadata changed: "+c+":"+key)
            for method,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(method)!=metadata or after_exceptions[method]!=exceptions[method]:
                    raise RuntimeError("Original anchored array method metadata or Exceptions changed: "+method)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Anchored array recovery changed partial additions, alias effects or failure precedence")
        mutations=(
            ("early-array-length","Trace.hit(1);","int ignored = values.length; Trace.hit(1);",12),
            ("ignored-null","sink.add(value);","if (value != null) sink.add(value);",0),
            ("duplicated-add","sink.add(value);","sink.add(value); sink.add(value);",0),
            ("false-result-exit","sink.add(value);","if (!sink.add(value)) return;",13),
            ("backward-traversal","int index = 0; index < values.length; index++","int index = values.length - 1; index >= 0; index--",0),
            ("changed-private-flag","private ","public ",0))
        original=(fixture/"fixture/Owner.java").read_text()
        for label,before,replacement,mode in mutations:
            if original.count(before)!=1:raise RuntimeError("Original anchored array counterexample anchor changed")
            changed=work/label/"Owner.java";changed.parent.mkdir();changed.open("x").write(original.replace(before,replacement))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),"AnchoredArrayVoidHarness",
                "1",str(mode),str(args.instance_helper).lower()],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned anchored array void" not in result.stderr:
                raise RuntimeError("Frozen observer missed incorrect array consumer: "+label)
        wrong=work/"signature-drift/fixture/Owner.class";wrong.parent.mkdir(parents=True);shutil.copy2(originals/"fixture/Owner.class",wrong)
        receipt=change_signature(wrong,signature,signature.replace(b"[TT;",b"[Ljava/lang/Object;"))
        (work/"signature-drift-encoding.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parents[1],originals,harness))),"AnchoredArrayVoidHarness",
            "1","0",str(args.instance_helper).lower()],text=True,capture_output=True,timeout=300,env=verify.environment)
        (work/"signature-drift-counterexample.log").open("x").write(result.stdout+result.stderr)
        if result.returncode==0 or "owned anchored array void" not in result.stderr:
            raise RuntimeError("Frozen observer missed unchanged-Code array Signature drift")
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):
            raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
            "instance_helper":args.instance_helper,"independent_wrong_programs":len(mutations)+1,
            "all_Java_original_entries_metadata_and_Exceptions":"matched",
            "private_scope_nested_array_contract_partial_additions_mutations_and_exact_failures":"matched",
            "original_inputs_and_frozen_observers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=="__main__":raise SystemExit(main())
