#!/usr/bin/env python3
"""Verify exact empty instructions compose with constructor allocation copies."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_interface_default_access import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--spread-padding",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-padded-constructor-allocation-")
    with context as directory:
        work=Path(directory).resolve();v=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"padded_constructor_allocation";originals=work/"originals";harness=work/"harness";name="PaddedConstructorAllocationHarness"
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(4,6):raise RuntimeError("Original padded constructor definition inventory changed")
        classes={"Lfixture/Core;"};methods={m for m in all_methods if m.split("->",1)[0] in classes}
        if len(methods)!=2:raise RuntimeError("Original constructor identities changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        provider=work/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for owner in sorted(all_classes-classes):
                path=owner[1:-1]+".class";jar.writestr(path,(originals/path).read_bytes())
        input_hashes={str(p):sha(p) for p in (args.android_jar,provider)}
        def execute(runtime,label):
            result=[]
            for route in range(3):
                for mode in range(16):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
                    if text!="attempts=2,padded-constructor-allocation=matched\n":raise RuntimeError("Frozen padded constructor observer changed")
                    result.append(text)
            return result
        baseline=execute((originals,harness),"baseline")
        (work/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        core=(fixture/"java/fixture/Core.java").read_text();value=(fixture/"java/fixture/Value.java").read_text()
        wrong=[("swapped-arguments","Core",core,"new Value(a,b,c,d,e,f)","new Value(b,a,c,d,e,f)",0),
               ("wrong-last","Core",core,"new Value(a,b,c,d,e,f)","new Value(a,b,c,d,e,a)",0),
               ("lost-allocation","Core",core,"new Value(a,b,c,d,e,f)","(Value)null",0),
               ("double-allocation","Core",core,"new Value(a,b,c,d,e,f)","new Value(new Value(a,b,c,d,e,f),b,c,d,e,f)",0),
               ("lost-field","Value",value,"this.a=a;","this.a=null;",0),
               ("lost-after","Value",value,"Trace.hit(1,this,new Object[]{a,b,c,d,e,f});","",0),
               ("lost-value","Core",core,"this.value=value;","this.value=null;",0),
               ("early-delegation-event","Core",core,"this.value=value;\n  Trace.hit(2,this,new Object[]{value});","Trace.hit(2,this,new Object[]{value});\n  this.value=value;",0),
               ("wrapped-failure","Value",value,"Trace.hit(0,this,new Object[]{a,b,c,d,e,f});","try{Trace.hit(0,this,new Object[]{a,b,c,d,e,f});}catch(Throwable error){throw new IllegalStateException(error);}",4),
               ("swallowed-failure","Value",value,"Trace.hit(1,this,new Object[]{a,b,c,d,e,f});","try{Trace.hit(1,this,new Object[]{a,b,c,d,e,f});}catch(Throwable ignored){}",5)]
        for label,owner,text,before,after,mode in wrong:
            if text.count(before)!=1:raise RuntimeError("Padded constructor mutation anchor changed")
            path=work/label/(owner+".java");path.parent.mkdir();path.open("x").write(text.replace(before,after));compiled=work/(label+"-classes")
            v.compile_local([path],compiled,classpath=originals,release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (work/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned padded constructor allocation" not in r.stderr:raise RuntimeError("Observer missed wrong padded constructor: "+label)
        text=(fixture/"smali/Core.smali").read_text()
        if text.count("    nop\n")!=6:raise RuntimeError("Authored padding changed")
        if args.spread_padding:
            text=text.replace("    nop\n","")
            for i in range(1,7):
                anchor=f"    move-object/from16 v{i}, p{i}\n"
                if text.count(anchor)!=1:raise RuntimeError("Authored copy changed")
                text=text.replace(anchor,anchor+"    nop\n")
        source=work/"authored-smali";source.mkdir();smali=source/"Core.smali";smali.open("x").write(text)
        input_hashes[str(smali)]=sha(smali)
        (work/"frozen-source-input-hashes.json").open("x").write(json.dumps(input_hashes,indent=2)+"\n")
        (work/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        cp=["--classpath",args.android_jar.resolve(),"--classpath",provider]
        output=work/"recovered";v.run([args.neverd.resolve(),"mobile",source,*cp,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=1 or coverage["method_count"]!=2 or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings") or coverage.get("minimum_java_release")!=25:raise RuntimeError("Padded constructor definitions or source contracts changed")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={"Lfixture/Core;->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"}
        rebuilt=work/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=provider,release=25)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Padded constructor source invented or omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);versions={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            if (wanted["major"],wanted["minor"],actual["major"],actual["minor"])!=(52,0,69,0):raise RuntimeError("Unexpected padded constructor class version")
            versions[owner]={"original_major":52,"rebuilt_major":69,"minor":0};wanted["major"]=69
            if wanted!=actual:raise RuntimeError("Complete padded constructor metadata changed: "+owner)
            if any(exceptions[m]!=after_exceptions[m] for m in wanted["methods"]):raise RuntimeError("Padded constructor Exceptions changed")
        (work/"compiler-class-version-differences.json").open("x").write(json.dumps(versions,indent=2)+"\n")
        if execute((rebuilt,harness,provider),"rebuilt")!=baseline:raise RuntimeError("Padded constructor allocation, aliases, partial state or effects changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(p))!=h for p,h in input_hashes.items()):raise RuntimeError("Frozen padded constructor inputs changed")
        summary={"status":"verified","class_count":1,"method_count":2,"fresh_jvm_cases":48,"attempts_per_case":2,"wrong_programs":len(wrong),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":25,"input_method_bytecode":"independently authored smali","spread_padding":args.spread_padding}
        (work/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0


if __name__=="__main__":raise SystemExit(main())
