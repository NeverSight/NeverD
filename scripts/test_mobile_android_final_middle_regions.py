#!/usr/bin/env python3
"""Verify structured final assignments compose with surrounding loop and switch stages."""
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
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--callee-abrupt-exit",action="store_true",help="Keep the constructor throw-free while a default interface call retains the abrupt exit")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-middle-regions-")
    with context as directory:
        work=Path(directory).resolve();v=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"final_middle_regions/java";originals=work/"originals";harness=work/"harness";name="FinalMiddleRegionsHarness"
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        if args.callee_abrupt_exit:
            source=work/"callee-abrupt-source"/"fixture";source.mkdir(parents=True,exist_ok=False)
            for original in sorted(fixture.rglob("*.java")):
                text=original.read_text()
                if original.name=="Core.java":
                    before="default:throw context.problem(this);"
                    if text.count(before)!=1:raise RuntimeError("Original constructor abrupt exit changed")
                    text=text.replace(before,"default:context.abort(this);break;")
                if original.name=="Context.java":
                    if text.count("}")!=1:raise RuntimeError("Original context interface changed")
                    text=text.replace("}"," default void abort(Core target) { throw problem(target); }\n}")
                (source/original.name).open("x").write(text)
            fixture=source.parent
        v.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(2,10 if args.callee_abrupt_exit else 9):raise RuntimeError("Original final middle region definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        declarations={"Lfixture/Context;"};excluded=declarations if args.dependency_provider else set()
        classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=work/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for owner in sorted(declarations):
                path=owner[1:-1]+".class";jar.writestr(path,(originals/path).read_bytes())
        dependencies=[provider] if excluded else [];input_hashes={str(p):sha(p) for p in (args.android_jar,provider)}
        def execute(runtime,label):
            result=[]
            for route in range(3):
                for mode in range(30):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
                    if text!="attempts=2,final-middle-regions=matched\n":raise RuntimeError("Frozen final middle region observer changed")
                    result.append(text)
            return result
        baseline=execute((originals,harness),"baseline")
        (work/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        file=fixture/"fixture/Core.java";text=file.read_text()
        default_exit="default:context.abort(this);break;" if args.callee_abrupt_exit else "default:throw context.problem(this);"
        wrong=[("lost-first","first=context.open(this);","first=null;",0),
               ("double-open","first=context.open(this);","context.open(this);first=context.open(this);",0),
               ("changed-branch","selected=context.left(this);","selected=context.right(this);",0),
               ("changed-after","context.after(this,0);","context.after(this,1);",0),
               ("lost-iteration","i<rounds","i<rounds-1",3),
               ("changed-step","context.step(this,i)","context.other(this,i)",3),
               ("lost-throw",default_exit,"default:current=null;break;",5),
               ("lost-finish","last=context.finish(this,current);","last=null;",0),
               ("swallow","context.after(this,0);","try{context.after(this,0);}catch(Throwable ignored){}",15),
               ("wrap","context.after(this,0);","try{context.after(this,0);}catch(Throwable error){throw new IllegalStateException(error);}",15)]
        for label,before,after,mode in wrong:
            if text.count(before)!=1:raise RuntimeError("Final middle region mutation anchor changed")
            path=work/label/"Core.java";path.parent.mkdir();path.open("x").write(text.replace(before,after));compiled=work/(label+"-classes")
            v.compile_local([path],compiled,classpath=originals,release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (work/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned final middle regions" not in r.stderr:raise RuntimeError("Observer missed wrong final middle region: "+label)
        cp=["--classpath",args.android_jar.resolve()]
        if excluded:cp += ["--classpath",provider]
        dexdir=work/"dex";dexdir.mkdir();paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original")
        dex=dexdir/"classes.dex";frozen_dex=sha(dex)
        (work/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n")
        (work/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a final middle region definition")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in coverage["methods"]}
        required_release=9 if args.callee_abrupt_exit and not args.dependency_provider else 8
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings") or coverage.get("minimum_java_release",8)!=required_release:raise RuntimeError("Final middle region definitions or complete signatures changed")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        rebuilt=work/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)) if dependencies else None,release=required_release)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Final middle region source invented or omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            if required_release==9:
                if (wanted["major"],actual["major"])!=(52,53):raise RuntimeError("Final middle region class versions changed")
                actual["major"]=wanted["major"]
            if wanted!=actual:raise RuntimeError("Complete final middle region metadata changed: "+owner)
            if any(exceptions[m]!=after_exceptions[m] for m in wanted["methods"]):raise RuntimeError("Final middle region Exceptions changed")
        if execute((rebuilt,harness,*dependencies),"rebuilt")!=baseline:raise RuntimeError("Final middle region effects or delayed result dispatch changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or any(sha(Path(p))!=h for p,h in input_hashes.items()):raise RuntimeError("Frozen final middle region inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":90,"attempts_per_case":2,"wrong_programs":len(wrong),"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":required_release,"dependency_provider":args.dependency_provider,"callee_abrupt_exit":args.callee_abrupt_exit}
        (work/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0


if __name__=="__main__":raise SystemExit(main())
