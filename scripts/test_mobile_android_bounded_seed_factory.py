#!/usr/bin/env python3
"""Verify bounded seed/factory calls with immutable, independently compiled observers."""
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

SIGNATURE = b"<T::Lfixture/Bound;>(Ljava/lang/Object;ILjava/util/List<TT;>;Ljava/lang/Object;JLfixture/Fn<-Ljava/lang/Integer;+TT;>;)Ljava/util/List<TT;>;"
IDENTITY = "Lfixture/Provider;->fill(Ljava/lang/Object;ILjava/util/List;Ljava/lang/Object;JLfixture/Fn;)Ljava/util/List;"

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"): parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"): parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--paired-seeds",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-seed-factory-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"bounded_seed_factory"
        signature,identity=SIGNATURE,IDENTITY
        if args.paired_seeds:
            copied=work/"paired-fixture";shutil.copytree(fixture,copied);fixture=copied
            signature=signature.replace(b"Ljava/util/List<TT;>;",b"Ljava/util/List<+TT;>;",1).replace(b"Lfixture/Fn<-Ljava/lang/Integer;+TT;>;",b"Ljava/util/List<+TT;>;")
            identity=identity.replace("Lfixture/Fn;","Ljava/util/List;")
            provider=fixture/"java/fixture/Provider.java";text=provider.read_text()
            text=text.replace("List<T> values","List<? extends T> values").replace("Fn<? super Integer,? extends T> factory)","List<? extends T> second)")
            text=text.replace("        Trace.context=context;","        Fn<? super Integer,? extends T> factory=(Fn<? super Integer,? extends T>)Trace.callbackInput;Trace.second=second;\n        Trace.context=context;")
            text=text.replace("values.add(item);","((List<T>)values).add(item);").replace("(List<T>)Trace.resultInput","(List<T>)second")
            provider.write_text(text)
            for name in ("java/fixture/Client.java","harness/BoundedSeedFactoryCaller.java"):
                path=fixture/name;text=path.read_text().replace("Fn factory","List second").replace("bits,factory)","bits,second)");path.write_text(text)
            trace=fixture/"java/fixture/Trace.java";trace.write_text(trace.read_text().replace("result,argument;","result,argument,callbackInput,second;"))
            observer=fixture/"harness/BoundedSeedFactoryHarness.java";text=observer.read_text()
            end=text.index("    static void signature()")
            call=text[:end].replace("        if(route==0)","        Trace.callbackInput=factory;List second=(List)Trace.resultInput;\n        if(route==0)")
            call=call.replace("Fn.class","List.class").replace("bits,factory)","bits,second)")
            text=call+text[end:]
            text=text.replace("long.class,Fn.class)","long.class,List.class)")
            text=text.replace("seed.getActualTypeArguments()[0].equals(formals[0])","((WildcardType)seed.getActualTypeArguments()[0]).getUpperBounds()[0].equals(formals[0])")
            start=text.index("        ParameterizedType factory=");end=text.index("\n    }",start)
            text=text[:start]+"""        for(int slot:new int[]{2,5}) {
            ParameterizedType input=(ParameterizedType)method.getGenericParameterTypes()[slot];
            require(input.getRawType()==List.class);
            WildcardType argument=(WildcardType)input.getActualTypeArguments()[0];
            require(argument.getLowerBounds().length==0 && argument.getUpperBounds().length==1 && argument.getUpperBounds()[0].equals(formals[0]));
        }"""+text[end:]
            text=text.replace("Trace.calls=0;","Trace.calls=0;Trace.second=null;")
            text=text.replace("Trace.factory==factory && Trace.bits==bits","Trace.factory==factory && Trace.second==resultInput && Trace.bits==bits")
            observer.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        verify.compile_local(sorted((fixture/"harness").glob("*.java")),harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals);all_methods=set(all_methods)
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        if len(all_classes)!=6 or len(all_methods)!=10 or records["Lfixture/Provider;"]["methods"][identity]["signature"]!=signature.decode():
            raise RuntimeError("Original bounded factory inventory/Signature changed: "+repr((len(all_classes),len(all_methods))))
        classes=all_classes-({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        dependency=work/"provider.jar"
        def provider_jar(root,path):
            with zipfile.ZipFile(path,"x") as jar: jar.writestr("fixture/Provider.class",(root/"fixture/Provider.class").read_bytes())
        provider_jar(originals,dependency)
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider: classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(16):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"BoundedSeedFactoryHarness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original bounded factory behavior matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        def dex(root,directory,cp):
            directory.mkdir()
            paths=[p for p in sorted(root.rglob("*.class")) if "L"+p.relative_to(root).as_posix()[:-6]+";" in classes]
            verify.run([*verify.d8,"--debug","--min-api","24",*cp,"--output",directory,*paths],"d8-"+directory.name)
            result=directory/"classes.dex"
            if list(directory.glob("classes*.dex"))!=[result]: raise RuntimeError("Bounded factory DEX count changed")
            return result
        source_dex=dex(originals,work/"dex",classpath)
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original bounded factory entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in coverage["methods"]}
        if (set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Bounded factory recovery changed an original entry or contract")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=coverage.get("minimum_java_release",8),classpath=dependency if args.dependency_provider else None)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes-{"Lfixture/Bound;","Lfixture/Fn;"}}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or helpers!=expected_helpers:
            raise RuntimeError("Bounded factory source invented or omitted an entry/helper")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[owner].get(key)!=after[owner].get(key): raise RuntimeError("Bounded factory class metadata changed: "+owner+":"+key)
            for identity,metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity)!=metadata or after_exceptions.get(identity)!=exceptions[identity]:
                    raise RuntimeError("Bounded factory method metadata/Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline: raise RuntimeError("Bounded factory changed identities, effects, casts or failures")
        text=(fixture/"java/fixture/Provider.java").read_text()
        mutations=(("lost-wide","Trace.bits=bits;","Trace.bits=(int)bits;",0),
                   ("duplicate-callback","T item=factory.apply(Integer.valueOf(index));","T item=factory.apply(Integer.valueOf(index));factory.apply(Integer.valueOf(index));",0),
                   ("seed-result","return result;","return (List<T>)values;" if args.paired_seeds else "return values;",0),
                   ("delayed-publication","Trace.result=result;Trace.hit(6);return result;","Trace.hit(6);Trace.result=result;return result;",12))
        for label,old,new,mode in mutations:
            if text.count(old)!=1: raise RuntimeError("Bounded factory counterexample anchor changed")
            wrong=work/label/"fixture/Provider.java";wrong.parent.mkdir(parents=True);wrong.open("x").write(text.replace(old,new))
            variant=work/(label+"-classes");verify.compile_local([wrong],variant,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness))),"BoundedSeedFactoryHarness","1",str(mode)],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned bounded seed factory" not in result.stderr: raise RuntimeError("Frozen observer missed "+label)
        wrong=work/"signature-drift/fixture/Provider.class";wrong.parent.mkdir(parents=True);shutil.copy2(originals/"fixture/Provider.class",wrong)
        drift=signature.replace(b"Ljava/util/List<+TT;>;",b"Ljava/util/List<+Lfixture/Bound;>;",1) if args.paired_seeds else signature.replace(b"-Ljava/lang/Integer;",b"-Ljava/lang/Number;")
        receipt=change_signature(wrong,signature,drift)
        (work/"signature-drift-encoding.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parent.parent,originals,harness))),"BoundedSeedFactoryHarness","1","0"],text=True,capture_output=True,timeout=600,env=verify.environment)
        (work/"signature-drift-counterexample.log").open("x").write(result.stdout+result.stderr)
        if result.returncode==0 or "owned bounded seed factory" not in result.stderr: raise RuntimeError("Frozen observer missed Signature drift")
        negative=work/"negative-originals";shutil.copytree(originals,negative)
        invalid=signature.replace(b"Ljava/util/List<+TT;>;",b"Ljava/util/List<-TT;>;",1) if args.paired_seeds else signature.replace(b"-Ljava/lang/Integer;",b"+Ljava/lang/Integer;")
        receipt=change_signature(negative/"fixture/Provider.class",signature,invalid)
        (work/"negative-encoding.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        cp=list(classpath)
        if args.dependency_provider:
            jar=work/"negative-provider.jar";provider_jar(negative,jar);cp[-1]=jar
        negative_dex=dex(negative,work/"negative-dex",cp);rejected=work/"negative-rejected"
        result=subprocess.run([str(args.neverd.resolve()),"mobile",str(negative_dex),*map(str,cp),"--timeout","600","-o",str(rejected)],text=True,capture_output=True,timeout=600,env=verify.environment)
        (work/"negative-recovery.log").open("x").write(result.stdout+result.stderr)
        if result.returncode==0 or rejected.exists() or "generic invocation requires" not in result.stderr: raise RuntimeError("Unproved factory input variance was accepted")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash:
            raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","dependency_provider":args.dependency_provider,"paired_seeds":args.paired_seeds,"class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases_per_stage":len(baseline),"stages":2,"attempts_per_case":2,
                  "full_metadata_and_Exceptions":"unchanged","original_Code":"compiler output unchanged","actual_result_identity_and_partial_effects":"matched","bound_casts_and_exact_failures":"matched",
                  "independent_counterexamples":5,"unproved_variance":"rejected","input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
