#!/usr/bin/env python3
"""Verify a nested instance factory binds its private enclosing result carrier."""
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
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_interface_default_access import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-unary-carrier-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"nested_unary_carrier/java";originals=w/"originals";harness=w/"harness";name="NestedUnaryCarrierHarness"
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        v.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(6,12):raise RuntimeError("Original nested unary carrier definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        excluded={"Lfixture/Source;"} if args.dependency_inputs else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-input.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Source.class",(originals/"fixture/Source.class").read_bytes())
        dependencies=[provider] if excluded else [];input_hashes={str(p):sha(p) for p in (args.android_jar,provider)}
        def execute(runtime,label):
            result=[]
            for route in range(3):
                for mode in range(20):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
                    if text!="attempts=2,nested-unary-carrier=matched\n":raise RuntimeError("Frozen nested unary carrier observer changed")
                    result.append(text)
            return result
        baseline=execute((originals,harness),"baseline")
        (w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        core=(fixture/"fixture/Core.java").read_text();source=(fixture/"fixture/Source.java").read_text()
        allocation="Cell<U> allocated=new Cell<U>(source.read());"
        result="Trace.hit(4,this,source,allocated);\n    return Trace.override?(Cell<U>)Trace.replacement:allocated;"
        wrong=[("lost-getter","Core",core,"source.read()","(U)null",0),
               ("double-getter","Core",core,allocation,"source.read();"+allocation,0),
               ("lost-allocation","Core",core,allocation,"Cell<U> allocated=(Cell<U>)Trace.replacement;",0),
               ("double-allocation","Core",core,allocation,"Cell<U> allocated=new Cell<U>(new Cell<U>(source.read()).value);",0),
               ("wrong-result","Core",core,"Trace.override?(Cell<U>)Trace.replacement:allocated","Trace.override?(Cell<U>)Trace.replacement:null",0),
               ("early-result","Core",core,result,"boolean saved=Trace.override;Object replacement=Trace.replacement;Trace.hit(4,this,source,allocated);return saved?(Cell<U>)replacement:allocated;",15),
               ("late-getter-read","Source",source,"return chosen;","return value;",14),
               ("lost-final-field","Core",core,"value=input;","value=null;",0),
               ("wrapped-failure","Core",core,allocation,"Cell<U> allocated;try{allocated=new Cell<U>(source.read());}catch(Throwable error){throw new IllegalStateException(error);}",4),
               ("swallowed-failure","Core",core,"Trace.hit(4,this,source,allocated);","try{Trace.hit(4,this,source,allocated);}catch(Throwable ignored){}",7)]
        for label,owner,text,before,after,mode in wrong:
            if text.count(before)!=1:raise RuntimeError("Nested unary carrier mutation anchor changed")
            path=w/label/(owner+".java");path.parent.mkdir();path.open("x").write(text.replace(before,after));compiled=w/(label+"-classes")
            v.compile_local([path],compiled,classpath=originals,release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned nested unary carrier" not in r.stderr:raise RuntimeError("Observer missed wrong nested unary carrier: "+label)
        cp=["--classpath",args.android_jar.resolve()]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original")
        dex=dexdir/"classes.dex";frozen_dex=sha(dex)
        (w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n")
        (w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a nested unary carrier definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Nested unary carrier definitions or complete signatures changed")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Point;"}
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)) if dependencies else None,release=8)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Nested unary carrier source invented or omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for c in classes:
            wanted=copy.deepcopy(facts[c]);actual=copy.deepcopy(after[c]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            if wanted!=actual:raise RuntimeError("Complete nested unary carrier metadata changed: "+c)
            if any(exceptions[m]!=after_exceptions[m] for m in wanted["methods"]):raise RuntimeError("Nested unary carrier Exceptions changed")
        if execute((rebuilt,harness,*dependencies),"rebuilt")!=baseline:raise RuntimeError("Nested unary carrier receiver, snapshots, allocations, fields or failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or any(sha(Path(p))!=h for p,h in input_hashes.items()):raise RuntimeError("Frozen nested unary carrier inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":60,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"dependency_inputs":args.dependency_inputs}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0


if __name__=="__main__":raise SystemExit(main())
