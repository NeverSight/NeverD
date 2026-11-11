#!/usr/bin/env python3
"""Verify closed nominal superclass arguments without inventing a Signature."""
from __future__ import annotations
import argparse,hashlib,json,os,re,subprocess,tempfile,zipfile
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nominal-super-constructor-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"nominal_super_constructor";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs={str(args.android_jar.resolve()):sha(args.android_jar.resolve())}
        originals=w/"originals";harness=w/"harness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        names=("NominalSuperConstructorCaller","NominalSuperConstructorHarness","NominalSuperConstructorProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=str(originals),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(4,6):raise RuntimeError("Original nominal superclass inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        entry="Lfixture/Core;-><init>(Lfixture/Token;J)V"
        if facts["Lfixture/Core;"]["signature"]!="Lfixture/Parent<Lfixture/Token;>;" or facts["Lfixture/Core;"]["methods"][entry].get("signature"):raise RuntimeError("Genuine descriptor-only child constructor changed")
        if facts["Lfixture/Parent;"]["methods"]["Lfixture/Parent;-><init>(Ljava/lang/Object;J)V"]["signature"]!="(TT;J)V":raise RuntimeError("Original class-formal constructor changed")
        excluded={"Lfixture/Parent;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Parent.class",(originals/"fixture/Parent.class").read_bytes())
        provider_hash=sha(provider);runtime_deps=[provider] if excluded else []
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for mode in range(36):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],events=[0-9]+,stamp=[0-9a-f]{{1,16}}"
                    if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen nominal constructor observer changed: "+text)
                    result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline")
        parent=(fixture/"java/fixture/Parent.java").read_text();core=(fixture/"java/fixture/Core.java").read_text();trace=(fixture/"java/fixture/Trace.java").read_text()
        early_parent=parent.replace("Trace.step(1);\n        this.value=value;this.stamp=stamp;","this.value=value;this.stamp=stamp;\n        Trace.step(1);")
        early_child=core.replace("Trace.step(3);\n        saved=value;childStamp=stamp;","saved=value;childStamp=stamp;\n        Trace.step(3);")
        late_seen=core.replace("Trace.saved=value;Trace.stampSeen=stamp;\n        Trace.step(4);","Trace.step(4);\n        Trace.saved=value;Trace.stampSeen=stamp;")
        early_publication=core.replace("Trace.step(4);\n        Trace.corePartial=this;","Trace.corePartial=this;\n        Trace.step(4);")
        wrong=(("wrong-parent-value","Parent",parent,"this.value=value;","this.value=null;",0),
               ("wrong-parent-stamp","Parent",parent,"this.stamp=stamp;","this.stamp=stamp^1L;",0),
               ("lost-parent-publication","Parent",parent,"Trace.parentPartial=this;","Trace.parentPartial=null;",0),
               ("early-parent-store","Parent",parent,parent,early_parent,4),
               ("wrong-parent-observation","Parent",parent,"Trace.seen=value;","Trace.seen=null;",0),
               ("lost-generic-getter","Parent",parent,"return value;","return null;",0),
               ("wrong-child-saved","Core",core,"        saved=value;","        saved=new Token(null);",0),
               ("wrong-child-stamp","Core",core,"childStamp=stamp;","childStamp=~stamp;",0),
               ("early-child-fields","Core",core,core,early_child,12),
               ("late-child-observation","Core",core,core,late_seen,16),
               ("early-core-publication","Core",core,core,early_publication,16),
               ("lost-step","Trace",trace,"events=events*10+value;","events=0;",0),
               ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",4),
               ("lost-failure","Trace",trace,"if(fail==value)","if(false)",4),
               ("duplicate-parent-step","Parent",parent,"Trace.step(2);","Trace.step(2);Trace.step(2);",0),
               ("wrong-super-value","Core",core,"super(value,stamp);","super(new Token(value),stamp);",0))
        for label,owner,body,old,new,mode in wrong:
            if body.count(old)!=1 or old==new:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+".java");source.parent.mkdir();source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            v.compile_local([source],compiled,classpath=str(originals),release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned nominal super constructor" not in r.stderr:raise RuntimeError("Frozen observer missed wrong nominal constructor program: "+label)
        classpath=["--classpath",args.android_jar.resolve()]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir()
        paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex"
        if list(dexdir.glob("classes*.dex"))!=[dex]:raise RuntimeError("DEX count changed")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original nominal constructor definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        annotations=v.run([v.dexdump,"-a",dex],"sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or "VISIBILITY_RUNTIME" in annotations or len(re.findall(r"VISIBILITY_SYSTEM Ldalvik/annotation/Signature;",annotations))!=(1 if excluded else 5):raise RuntimeError("D8 generic Signature inventory changed")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage["status"]!="recovered" or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Original nominal constructor definitions or source contract changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or rows[entry].get("metadata_differences"):raise RuntimeError("Nominal constructor invented a declaration projection")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)) if excluded else None,release=8)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Nominal constructor source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            wanted=facts[owner];actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Nominal constructor declaration metadata changed: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Nominal constructor method metadata/Exceptions changed: "+identity)
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Nominal constructor references, partial fields or failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(provider)!=provider_hash or any(sha(Path(f))!=h for f,h in inputs.items()):raise RuntimeError("Frozen inputs/observer/provider changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":144,"attempts_per_case":2,"wrong_programs":16,"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
