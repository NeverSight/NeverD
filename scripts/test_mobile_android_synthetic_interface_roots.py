#!/usr/bin/env python3
"""Verify complete owned Kotlin synthetic interface naming contracts."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,shutil,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
    from scripts.mobile_real_apps_android import dex_header,parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from scripts.test_mobile_android_synthetic_method_names import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
    if error.name!="scripts":raise
    from mobile_real_apps_android import dex_header,parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from test_mobile_android_synthetic_method_names import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-synthetic-interface-roots-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"synthetic_interface_roots";k=args.kotlin_lib.resolve()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))]
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        input_hashes={str(p):sha(p) for p in inputs}
        std=k/"kotlin-stdlib.jar";runtime_deps=[std]
        declaration=w/"kotlin-metadata-declaration.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declaration,"x") as dest:dest.writestr("kotlin/Metadata.class",source.read("kotlin/Metadata.class"))
        java=w/"original-java";originals=w/"originals";harness=w/"harness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),java,release=8);originals.mkdir()
        v.run([v.java,"-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(std,java))),"-jvm-target","1.8","-Xjvm-default=all","-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        for f in java.rglob("*.class"):
            to=originals/f.relative_to(java);to.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,to)
        names=("SyntheticInterfaceRootCaller","SyntheticInterfaceRootHarness")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(6,13):raise RuntimeError("Original interface definition inventory changed")
        excluded={"Lfixture/Trace;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records=compiler_facts(originals);exceptions=exception_inventory(originals)
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
        classpath=["--classpath",args.android_jar.resolve(),"--classpath",declaration]
        if excluded:classpath += ["--classpath",provider];runtime_deps.append(provider)
        dependency_hashes={str(f):sha(f) for f in (declaration,provider)}
        proto="(Ljava/lang/Object;JJJJFI)V"
        family={"Lfixture/"+n+";->write-slot"+proto for n in ("Api","Alpha","Beta")}
        default="Lfixture/Api;->default-slot"+proto;projected=family|{default}
        if any(not records[m.split("->",1)[0]]["methods"][m]["access"]&0x1000 for m in projected):raise RuntimeError("Original synthetic contract flag changed")
        def execute(runtime,write,default_name,label):
            out=[]
            for route in range(4):
                for receiver in range(2):
                    for mode in range(12):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(receiver),str(mode),write,default_name],f"{label}-{route}-{receiver}-{mode}")
                        if text!="attempts=2,synthetic-interface-dispatch-values-identities-failures=matched\n":raise RuntimeError("Frozen interface observer changed")
                        out.append(text)
            return out
        baseline=execute((originals,harness,*runtime_deps),"write-slot","default-slot","baseline")
        def make_dex(selected,destination):
            destination.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in selected]
            v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",destination,*paths],"d8-"+destination.name)
            dex=destination/"classes.dex"
            if list(destination.glob("classes*.dex"))!=[dex]:raise RuntimeError("DEX count changed")
            return dex
        dex=make_dex(classes,w/"dex")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        annotations=v.run([v.dexdump,"-a",dex],"sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations)!=["Lkotlin/Metadata;"]*5:raise RuntimeError("D8 annotation retention changed")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count")!=4 or coverage["status"]!="partial" or coverage.get("generic_signature_projections"):raise RuntimeError("Original interface recovery coverage changed")
        spellings={rows[m].get("source_name") for m in family};default_name=rows[default].get("source_name")
        if len(spellings)!=1 or None in spellings or not default_name or default_name in spellings:raise RuntimeError("Interface families lost separate complete spellings")
        write=next(iter(spellings));rename={m:m.replace("->write-slot(","->"+write+"(") for m in family};rename[default]=default.replace("->default-slot(","->"+default_name+"(")
        for m in projected:
            row=rows[m];expected=family if m in family else {default};root="Lfixture/Api;->"+("write-slot" if m in family else "default-slot")+proto
            if row.get("projection_kind")!="virtual-method-identifier" or row.get("source_name_declaration")!=root or set(row.get("source_name_family",[]))!=expected or not any("cannot reproduce the original synthetic method flag" in d for d in row.get("metadata_differences",[])):raise RuntimeError("Missing original synthetic interface receipt")
        rebuilt=w/"rebuilt";release=coverage.get("minimum_java_release",8)
        v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,(std,provider) if excluded else (std,))),release=release)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Branch;"}
        if actual_classes!=classes or set(actual_methods)!={rename.get(m,m) for m in methods}|helpers or helpers!=expected_helpers:raise RuntimeError("Interface source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={}
        for owner in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[owner].get(key)!=after[owner].get(key):raise RuntimeError("Original interface class metadata changed: "+owner+":"+key)
            for identity,before in records[owner]["methods"].items():
                wanted=copy.deepcopy(before)
                if identity in projected:wanted["name"]=rows[identity]["source_name"];wanted["access"]&=~0x1000
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=wanted.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;","Lkotlin/jvm/JvmSynthetic;"} for a in uses):raise RuntimeError("Unexpected original CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;wanted[key]=[]
                selected=rename.get(identity,identity)
                if after[owner]["methods"].get(selected)!=wanted or after_exceptions[selected]!=exceptions[identity]:raise RuntimeError("Metadata changed beyond disclosed name/flag and exact D8 CLASS loss: "+identity)
        if not dropped:raise RuntimeError("Missing genuine compiler CLASS annotation witness")
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        if execute((rebuilt,harness,*runtime_deps),write,default_name,"rebuilt")!=baseline:raise RuntimeError("Interface dispatch, inputs, effects or failures changed")
        sdk_root=w/"unchanged-api.jar"
        with zipfile.ZipFile(sdk_root,"x") as jar:jar.writestr("fixture/Api.class",(originals/"fixture/Api.class").read_bytes())
        sdk_hash=sha(sdk_root);negative=make_dex(classes-{"Lfixture/Api;"},w/"sdk-root-dex");rejected=w/"sdk-root-rejected"
        r=subprocess.run([str(args.neverd.resolve()),"mobile",str(negative),*map(str,classpath),"--classpath",str(sdk_root),"--timeout","600","-o",str(rejected)],text=True,capture_output=True,timeout=1200,env=v.environment)
        (w/"sdk-root-rejection.log").open("x").write(r.stdout+r.stderr)
        if r.returncode==0 or rejected.exists() or "identifier cannot be represented in Java" not in r.stderr:raise RuntimeError("Unchanged SDK root gained a source spelling")
        trace=(fixture/"java/fixture/Trace.java").read_text()
        payload="token=value;a=aa;b=bb;c=cc;d=dd;bits=Float.floatToRawIntBits(scale);mode=m;"
        fail_after="if(fail==2) { if(failure instanceof RuntimeException)throw (RuntimeException)failure;throw (Error)failure; }"
        if trace.count(fail_after)!=1:raise RuntimeError("Callback failure boundary changed")
        late=trace.replace(payload,"").replace(fail_after,fail_after+payload)
        mutants=(("wrong-a","a=aa;","a=bb;",0),("wrong-b","b=bb;","b=aa;",0),("wrong-c","c=cc;","c=dd;",0),("wrong-d","d=dd;","d=cc;",0),("narrow-a","a=aa;","a=(int)aa;",0),("narrow-b","b=bb;","b=(int)bb;",0),("canonical-nan","Float.floatToRawIntBits(scale)","Float.floatToIntBits(scale)",2),("wrong-sign","Float.floatToRawIntBits(scale)","Float.floatToRawIntBits(scale)^0x80000000",0),("wrong-token","token=value;","token=new Object();",0),("wrong-tag","tag=t;","tag=0;",0),("duplicate-call","calls++;","calls+=2;",0),("lost-call","calls++;","calls+=0;",0),("wrapped-failure","throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",4),("late-payload",trace,late,5))
        for label,old,new,mode in mutants:
            count=trace.count(old)
            if count!=(2 if label=="wrapped-failure" else 1):raise RuntimeError("Wrong callback anchor changed: "+label)
            source=w/label/"fixture/Trace.java";source.parent.mkdir(parents=True);source.open("x").write(trace.replace(old,new))
            compiled=w/(label+"-classes");v.compile_local([source],compiled,release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"3","1",str(mode),"write-slot","default-slot"],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned synthetic interface roots" not in r.stderr:raise RuntimeError("Frozen observer missed wrong callback: "+label)
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(f))!=h for f,h in input_hashes.items()) or any(sha(Path(f))!=h for f,h in dependency_hashes.items()) or sha(sdk_root)!=sdk_hash:raise RuntimeError("Frozen compiler/inputs/observer/dependencies changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":96,"attempts_per_case":2,"wrong_programs":14,"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
