#!/usr/bin/env python3
"""Verify same-package provider reads before final-field constructor delegation."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-package-owner-constructor-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"package_owner_constructor";originals=w/"originals";harness=w/"harness";sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
        android=args.android_jar.resolve();android_hash=sha(android)
        v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        names=("PackageOwnerConstructorCaller","PackageOwnerConstructorHarness","PackageOwnerConstructorProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=str(originals),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(5,11):raise RuntimeError("Original package constructor definition inventory changed")
        excluded={"Lfixture/Pool;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        if facts["Lfixture/Pool;"]["access"]&1:raise RuntimeError("Provider must remain an ordinary package class")
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Pool.class",(originals/"fixture/Pool.class").read_bytes())
        provider_hash=sha(provider);runtime_deps=[provider] if excluded else []
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for kind in range(3):
                    for mode in range(25):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(kind),str(mode)],f"{label}-{route}-{kind}-{mode}")
                        pattern=f"route={route},kind={kind},mode={mode},attempt=[01],events=[0-9]+"
                        if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen package constructor observer changed: "+text)
                        result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline")
        (w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        core=(fixture/"java/fixture/Core.java").read_text();trace=(fixture/"java/fixture/Trace.java").read_text();pool=(fixture/"java/fixture/Pool.java").read_text();factory=(fixture/"java/fixture/Factory.java").read_text()
        wrong=(("wrong-read-snapshot","Core",core,"this.policy=policy;","this.policy=(Policy)Pool.value;",0,23),
               ("wrong-final-policy","Core",core,"this.policy=policy;","this.policy=null;",2,0),
               ("wrong-first-field","Core",core,"this.first=Trace.produce(5);","this.first=null;Trace.produce(5);",2,1),
               ("wrong-second-field","Core",core,"this.second=Trace.produce(6);","this.second=null;Trace.produce(6);",1,1),
               ("early-first-field","Core",core,"this.first=Trace.produce(5);","this.first=Trace.firstValue;Trace.produce(5);",2,1),
               ("early-second-field","Core",core,"this.second=Trace.produce(6);","this.second=Trace.secondValue;Trace.produce(6);",2,1),
               ("late-policy-field","Core",core,"this.policy=policy;Trace.step(4);","Trace.step(4);this.policy=policy;",1,6),
               ("late-escape","Core",core,"Trace.escaped=this;Trace.step(3);","Trace.step(3);Trace.escaped=this;",2,5),
               ("wrong-factory-hint","Factory",factory,"Trace.hint=hint;","Trace.hint=hint+1;",1,0),
               ("wrong-factory-policy","Factory",factory,"return Trace.factoryPolicy;","return null;",1,0),
               ("lost-class-init-step","Pool",pool,"Trace.step(1);","",0,0),
               ("wrong-initial-value","Pool",pool,"value=Trace.seed;","value=Trace.next;",0,0),
               ("lost-mutation","Trace",trace,"Factory.swap(next);",";",0,23),
               ("lost-events","Trace",trace,"events=events*10+value;","events=value;",2,0),
               ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",1,6),
               ("lost-error","Trace",trace,"throw (Error)failure;","return;",0,12),
               ("lost-failure","Trace",trace,"if(fail==value)","if(false)",2,7),
               ("wrong-return-value","Trace",trace,"return value==5?firstValue:secondValue;","return value==5?secondValue:firstValue;",2,1))
        for label,owner,body,old,new,kind,mode in wrong:
            if body.count(old)!=1:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+".java");source.parent.mkdir();source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            v.compile_local([source],compiled,classpath=str(originals),release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(kind),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned package owner constructor" not in r.stderr:raise RuntimeError("Frozen observer missed wrong constructor program: "+label)
        classpath=["--classpath",android]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir()
        paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex"
        frozen_dex={str(dex):sha(dex)};(w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original package constructor definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="recovered" or coverage.get("minimum_java_release",8)!=25:raise RuntimeError("Original package constructor definitions or release changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):raise RuntimeError("Unnecessary package constructor class or Signature projection")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)),release=25)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Package constructor source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            wanted=facts[owner];actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Package constructor declaration metadata changed: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Package constructor method metadata/Exceptions changed: "+identity)
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Package constructor identities, dispatch, snapshots or effects changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(android)!=android_hash or sha(provider)!=provider_hash or any(sha(Path(f))!=h for f,h in frozen_dex.items()):raise RuntimeError("Frozen inputs, observer or dependencies changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":300,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
