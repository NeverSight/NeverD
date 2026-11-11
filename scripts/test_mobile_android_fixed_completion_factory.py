#!/usr/bin/env python3
"""Verify erased unary/seeded callback factories with closed completion results."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
    from scripts.mobile_real_apps_android import dex_header,parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from scripts.test_mobile_android_interface_default_access import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
    if error.name!="scripts":raise
    from mobile_real_apps_android import dex_header,parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-fixed-completion-factory-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"fixed_completion_factory";originals=w/"originals";harness=w/"harness";sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
        android=args.android_jar.resolve();android_hash=sha(android)
        v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        names=("FixedCompletionFactoryCaller","FixedCompletionFactoryHarness","FixedCompletionFactoryProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=str(originals),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(7,13):raise RuntimeError("Original fixed completion definition inventory changed")
        sdk_names=("Factory","Unary","Binary","Completion","Leaf")
        excluded={"Lfixture/"+n+";" for n in sdk_names} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for name in sdk_names:jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        provider_hash=sha(provider);runtime_deps=[provider] if excluded else []
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for kind in range(2):
                    for mode in range(21):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(kind),str(mode)],f"{label}-{route}-{kind}-{mode}")
                        pattern=f"route={route},kind={kind},mode={mode},attempt=[01],events=[0-9]+"
                        if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen fixed completion observer changed: "+text)
                        result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline")
        (w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        core=(fixture/"java/fixture/Core.java").read_text();factory=(fixture/"java/fixture/Factory.java").read_text();trace=(fixture/"java/fixture/Trace.java").read_text()
        invoke="Object value=kind==0?((Unary)block).call(completion):((Binary)block).call(receiver,completion);"
        wrong=(("lost-receiver","Core",core,"Factory.seeded(block,receiver,completion)","Factory.seeded(block,null,completion)",1,1),
               ("lost-completion","Core",core,"Factory.unary(block,completion)","Factory.unary(block,null)",0,1),
               ("wrong-unary-dispatch","Factory",factory,"Trace.transfer(block,null,completion,0)","Trace.transfer(block,null,completion,1)",0,0),
               ("wrong-seeded-dispatch","Factory",factory,"Trace.transfer(block,receiver,completion,1)","Trace.transfer(block,receiver,completion,0)",1,0),
               ("late-return-snapshot","Trace",trace,"Completion value=result;step(6);return value;","step(6);Completion value=result;return value;",1,0),
               ("wrong-callback-result","Trace",trace,"callbackAnswer=value;","callbackAnswer=receiver;",1,0),
               ("unconditional-callback","Trace",trace,"if(run)","if(true)",1,16),
               ("changed-completion-control","Trace",trace,"if(run)","complete=true;if(run)",1,17),
               ("wrong-callback-receiver","Trace",trace,"((Binary)block).call(receiver,completion)","((Binary)block).call(completion,completion)",1,1),
               ("wrong-unary-input","Trace",trace,"((Unary)block).call(completion)","((Unary)block).call(receiver)",0,0),
               ("late-argument-observations","Trace",trace,"blockSeen=block;receiverSeen=receiver;completionSeen=completion;step(1);","step(1);blockSeen=block;receiverSeen=receiver;completionSeen=completion;",1,4),
               ("lost-block-identity","Trace",trace,"blockSeen=block;","blockSeen=null;",0,0),
               ("duplicate-callback","Trace",trace,invoke,invoke+"if(kind==0)((Unary)block).call(completion);else((Binary)block).call(receiver,completion);",1,0),
               ("lost-return-step","Trace",trace,"step(5);Completion value=result;","Completion value=result;",1,0),
               ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",1,4),
               ("lost-error","Trace",trace,"throw (Error)failure;","return;",0,10),
               ("bypassed-factory","Factory",factory,"return Trace.transfer(block,receiver,completion,1);","return Trace.result;",1,0),
               ("predicted-result","Trace",trace,"Completion value=result;","Completion value=(Completion)callbackAnswer;",1,0))
        for label,owner,body,old,new,kind,mode in wrong:
            if body.count(old)!=1:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+".java");source.parent.mkdir();source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            v.compile_local([source],compiled,classpath=str(originals),release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(kind),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned fixed completion factory" not in r.stderr:raise RuntimeError("Frozen observer missed wrong fixed completion program: "+label)
        classpath=["--classpath",android]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir()
        paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex"
        frozen_dex={str(dex):sha(dex)};(w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original fixed completion definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="recovered" or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Original fixed completion definitions or release changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):raise RuntimeError("Unnecessary fixed completion class or Signature projection")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)),release=8)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Fixed completion source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            wanted=facts[owner];actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Fixed completion declaration metadata changed: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Fixed completion method metadata/Exceptions changed: "+identity)
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Fixed completion identities, dispatch, snapshots or effects changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(android)!=android_hash or sha(provider)!=provider_hash or any(sha(Path(f))!=h for f,h in frozen_dex.items()):raise RuntimeError("Frozen inputs, observer or dependencies changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":168,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
