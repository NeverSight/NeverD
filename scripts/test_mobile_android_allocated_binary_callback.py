#!/usr/bin/env python3
"""Verify fresh raw binary callback allocation before a closed superclass call."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8, class_identity
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
except ModuleNotFoundError as error:
    if error.name != "scripts":raise
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8, class_identity
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from mobile_real_apps_android import dex_header, parse_dexdump


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-contracts",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocated-binary-callback-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"allocated_binary_callback"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        verify.compile_local(sorted((fixture/"harness").glob("*.java")),harness,classpath=originals,release=8)
        frozen={"originals":hashes(originals),"harness":hashes(harness)}
        (work/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n")
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();sdk_hash=sha(args.android_jar)
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=8 or len(all_methods)!=12:raise RuntimeError("Owned binary callback inventory changed")
        contracts={"Lfixture/Parent;","Lfixture/Callback;","Lfixture/Functor;","Lfixture/Marker;","Lfixture/Token;"}
        classes=all_classes-(contracts if args.dependency_contracts else set())
        methods={identity for identity in all_methods if identity.split("->",1)[0] in classes}
        records=class_identity.compiler_classes(originals);exceptions=exception_inventory(originals)
        if records["Lfixture/Adapter;"].get("signature") or records["Lfixture/Child;"].get("signature"):
            raise RuntimeError("Compiler invented a raw adapter/child Signature")
        dependency=work/"owned-contracts.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for owner in sorted(contracts):
                name=owner[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=sha(dependency);classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_contracts:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            rows=[]
            for route in range(4):
                for operation in range(3):
                    for mode in range(16):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"BinaryCallbackHarness",str(route),str(operation),str(mode)],f"{label}-{route}-{operation}-{mode}")
                        pattern=f"route={route},operation={operation},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError("Frozen binary callback observer changed")
                        rows.append(output)
            return rows
        baseline=execute((originals,harness),"baseline")
        paths=[path for path in sorted(originals.rglob("*.class")) if "L"+path.relative_to(originals).as_posix()[:-6]+";" in classes]
        dex=work/"dex";dex.mkdir()
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]}!=classes or {row["identity"] for row in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original binary callback definition")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        report=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in report["methods"]}!=methods or report["class_count"]!=len(classes) or report["method_count"]!=len(methods)
                or report["unrecovered_method_count"] or report.get("projected_method_count",0) or report.get("minimum_java_release")!=25
                or report.get("class_source_bindings") or report.get("generic_signature_projections") or report["status"]!="recovered"):
            raise RuntimeError("Binary callback recovery changed original inventory or source contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_contracts else None,release=25)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={row["class"]+"->"+row["name"]+row["prototype"] for row in report.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner not in {"Lfixture/Callback;","Lfixture/Functor;"}}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:raise RuntimeError("Binary callback recovery omitted an entry or invented a helper")
        after=class_identity.compiler_classes(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            before=records[owner];actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if before.get(key)!=actual.get(key):raise RuntimeError("Original binary callback class metadata changed: "+key)
            for identity,row in before["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions.get(identity)!=exceptions[identity]:raise RuntimeError("Original binary callback method metadata/Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_contracts else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Binary callback construction changed order, references, partial state, casts or failures")
        callback=b"<T:Ljava/lang/Object;R:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Functor<TR;>;"
        functor=b"<R:Ljava/lang/Object;>Ljava/lang/Object;"
        mutations=(
            ("input-bound","Callback",callback,callback.replace(b"T:Ljava/lang/Object;",b"T:Ljava/lang/Number;")),
            ("parent-bound","Functor",functor,functor.replace(b"R:Ljava/lang/Object;",b"R:Ljava/lang/Number;")),
            ("raw-parent","Callback",callback,callback.replace(b"Functor<TR;>",b"Functor")),
            ("closed-parent","Callback",callback,callback.replace(b"Functor<TR;>",b"Functor<Ljava/lang/Object;>")),
            ("variant-parent","Callback",callback,callback.replace(b"Functor<TR;>",b"Functor<+TR;>")),
            ("parent-arity","Functor",functor,functor.replace(b";>Ljava",b";S:Ljava/lang/Object;>Ljava")),
        )
        for label,owner,old,new in mutations:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            receipt=change_signature(negative/("fixture/"+owner+".class"),old,new)
            (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
            negative_classpath=list(classpath)
            if args.dependency_contracts:
                jar_path=work/(label+"-contracts.jar")
                with zipfile.ZipFile(jar_path,"x") as jar:
                    for descriptor in sorted(contracts):
                        name=descriptor[1:-1]+".class";jar.writestr(name,(negative/name).read_bytes())
                negative_classpath[-1]=jar_path
            negative_dex=work/(label+"-dex");negative_dex.mkdir()
            verify.run([*verify.d8,"--debug","--min-api","24",*negative_classpath,"--output",negative_dex,*[negative/path.relative_to(originals) for path in paths]],label+"-d8")
            rejected=work/(label+"-rejected")
            result=subprocess.run([args.neverd.resolve(),"mobile",negative_dex/"classes.dex",*negative_classpath,"--timeout","600","-o",rejected],capture_output=True,text=True,timeout=300,env=verify.environment)
            (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or rejected.exists():raise RuntimeError("Unproved binary callback graph accepted: "+label)
        bodies={name:(fixture/("java/fixture/"+name+".java")).read_text() for name in ("Child","Adapter","Parent")}
        wrongs=(
            ("lost-allocation","Child","null,new Adapter()","null,null",0,0),
            ("swapped-callbacks","Child","null,new Adapter()","new Adapter(),null",0,0),
            ("narrowed-wide","Child","super(id,marker,","super((int)id,marker,",0,0),
            ("copied-marker","Child","super(id,marker,","super(id,new Marker(),",0,10),
            ("early-result-cast","Adapter","return Trace.result;","return (Token)Trace.result;",2,13),
            ("lost-allocation-publication","Adapter","Trace.adapter=this;","",0,3),
            ("replaced-parent-throw","Parent","Trace.hit(5);","try{Trace.hit(5);}catch(RuntimeException problem){throw new IllegalArgumentException(problem);}",0,5),
            ("lost-callback-argument","Adapter","Trace.seen=value;","Trace.seen=null;",1,0),
        )
        for label,owner,old,new,operation,mode in wrongs:
            if bodies[owner].count(old)!=1:raise RuntimeError("Binary callback counterexample anchor changed: "+label)
            wrong=work/label/("fixture/"+owner+".java");wrong.parent.mkdir(parents=True);wrong.open("x").write(bodies[owner].replace(old,new))
            compiled=work/(label+"-classes");verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),"BinaryCallbackHarness","1",str(operation),str(mode)],capture_output=True,text=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned allocated binary callback changed" not in result.stderr:raise RuntimeError("Frozen observer missed "+label)
        if {"originals":hashes(originals),"harness":hashes(harness)}!=frozen or sha(args.android_jar)!=sdk_hash or sha(dependency)!=dependency_hash:raise RuntimeError("Frozen binary callback inputs or observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,
                  "dependency_contracts":args.dependency_contracts,"same_code_signature_mutations":6,"independent_wrong_programs":8,
                  "original_metadata_and_exceptions":"unchanged","allocation_initialization_partial_fields_and_delayed_results":"matched","input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
