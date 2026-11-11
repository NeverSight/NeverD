#!/usr/bin/env python3
"""Verify guarded this calls retain complete closed constructor contracts."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-guarded-closed-delegation-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"guarded_closed_delegation";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash=sha(args.android_jar.resolve())
        originals=w/"originals";harness=w/"harness"
        java25=w/"compiled-java25";java8=w/"compiled-declarations-java8"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),java25,release=25)
        declarations={"fixture/Parent.class","fixture/Pair.class"}
        v.compile_local([fixture/"java/fixture"/(n+".java") for n in ("Parent","Pair")],java8,classpath=str(java25),release=8)
        originals.mkdir()
        for tree in (java25,java8):
            for f in tree.rglob("*.class"):
                name=f.relative_to(tree).as_posix()
                if tree==java25 and name in declarations:continue
                target=originals/name;target.parent.mkdir(parents=True,exist_ok=True)
                with target.open("xb") as stream:stream.write(f.read_bytes())
        names=("GuardedClosedDelegationCaller","GuardedClosedDelegationHarness","GuardedClosedDelegationProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=str(originals),release=25)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(7,13):raise RuntimeError("Original guarded closed delegation inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        entry="Lfixture/Core;-><init>(Ljava/util/List;Ljava/util/List;JFILfixture/Marker;)V"
        target="Lfixture/Core;-><init>(Ljava/util/List;Ljava/util/List;JF)V"
        if facts["Lfixture/Core;"]["signature"]!="Lfixture/Parent;Lfixture/Pair<Lfixture/Left;Lfixture/Right;>;":raise RuntimeError("Original complete class header changed")
        if facts["Lfixture/Core;"]["methods"][entry].get("signature") or facts["Lfixture/Core;"]["methods"][target]["signature"]!="(Ljava/util/List<+Lfixture/Left;>;Ljava/util/List<+Lfixture/Right;>;JF)V":raise RuntimeError("Original raw caller or closed target Signature changed")
        excluded={"Lfixture/Parent;","Lfixture/Pair;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for owner in ("Parent","Pair"):jar.writestr("fixture/"+owner+".class",(originals/("fixture/"+owner+".class")).read_bytes())
        provider_hash=sha(provider);runtime_deps=[provider] if excluded else []
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for mode in range(45):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],events=[0-9]+,wide=[0-9a-f]{{1,16}},scale=[0-9a-f]{{1,8}}"
                    if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen guarded closed observer changed: "+text)
                    result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline")
        core=(fixture/"java/fixture/Core.java").read_text();parent=(fixture/"java/fixture/Parent.java").read_text();trace=(fixture/"java/fixture/Trace.java").read_text()
        early_fields=core.replace("Trace.step(6);\n        this.first=first;this.second=second;this.wide=wide;this.scale=scale;","this.first=first;this.second=second;this.wide=wide;this.scale=scale;\n        Trace.step(6);")
        early_publication=core.replace("Trace.step(7);\n        Trace.corePartial=this;","Trace.corePartial=this;\n        Trace.step(7);")
        wrong=(
            ("reversed-first-mask","Core",core,"(mask&1)!=0","(mask&1)==0",0),
            ("lost-second-default","Core",core,"second=Trace.second();","second=first;",15),
            ("duplicate-first-default","Core",core,"first=Trace.first();","first=Trace.first();Trace.first();",1),
            ("lost-wide-default","Core",core,"wide=Trace.wide();","wide=0;Trace.wide();",15),
            ("narrowed-wide-default","Core",core,"wide=Trace.wide();","wide=(int)Trace.wide();",15),
            ("lost-float-default","Core",core,"scale=Trace.scale();","scale=0f;Trace.scale();",15),
            ("wrong-first-argument","Core",core,"this((List<? extends Left>)first,","this((List<? extends Left>)second,",0),
            ("wrong-noarg-mask","Core",core,"this(null,null,0L,0f,15,null);","this(null,null,0L,0f,0,null);",44),
            ("lost-parent-publication","Parent",parent,"Trace.parentPartial=this;","Trace.parentPartial=null;",0),
            ("early-child-fields","Core",core,core,early_fields,21),
            ("early-core-publication","Core",core,core,early_publication,22),
            ("lost-first-read","Trace",trace,"firstRead=value;","firstRead=null;",16),
            ("wrong-child-wide","Core",core,"this.wide=wide;","this.wide=wide^1L;",0),
            ("lost-step","Trace",trace,"events=events*10+value;","events=0;",0),
            ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",16),
            ("lost-failure","Trace",trace,"if(fail==value)","if(false)",16),
        )
        for label,owner,body,old,new,mode in wrong:
            if body.count(old)!=1 or old==new:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+".java");source.parent.mkdir();source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            v.compile_local([source],compiled,classpath=str(originals),release=25)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned guarded closed delegation" not in r.stderr:raise RuntimeError("Frozen observer missed wrong guarded closed program: "+label)
        classpath=["--classpath",args.android_jar.resolve()]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir()
        paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex"
        if list(dexdir.glob("classes*.dex"))!=[dex]:raise RuntimeError("DEX count changed")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original guarded closed definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        annotations=v.run([v.dexdump,"-a",dex],"sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or "VISIBILITY_RUNTIME" in annotations or len(re.findall(r"VISIBILITY_SYSTEM Ldalvik/annotation/Signature;",annotations))!=(8 if excluded else 9):raise RuntimeError("D8 closed Signature inventory changed")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or coverage["status"]!="recovered" or coverage.get("minimum_java_release",8)!=25:raise RuntimeError("Guarded closed definitions or source contract changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or rows[entry].get("metadata_differences"):raise RuntimeError("Guarded closed delegation invented a declaration projection")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)) if excluded else None,release=25)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Guarded closed source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            wanted=facts[owner];actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Guarded closed metadata changed: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Guarded closed method metadata/Exceptions changed: "+identity)
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Guarded closed masks, references, bits, partial fields or failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(provider)!=provider_hash or sha(args.android_jar.resolve())!=sdk_hash:raise RuntimeError("Frozen input/observer/provider changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":180,"attempts_per_case":2,"wrong_programs":16,"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
