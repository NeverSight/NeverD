#!/usr/bin/env python3
"""Verify projected class formals stay separate from a suspend callback result."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,shutil,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
    from scripts.mobile_real_apps_android import dex_header,parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from scripts.test_mobile_android_interface_default_access import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name!="scripts":raise
    from mobile_real_apps_android import dex_header,parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags
OWNER="Lfixture/Carrier;"
PROTO="(Ljava/util/concurrent/atomic/AtomicReference;Lkotlin/jvm/functions/Function1;Lkotlin/jvm/functions/Function2;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;"
ENTRY=OWNER+"->process"+PROTO
SOURCE=("<R:Ljava/lang/Object;>(Ljava/util/concurrent/atomic/AtomicReference<Lfixture/Carrier$Slot<Ljava/lang/Object;>;>;"
        "Lkotlin/jvm/functions/Function1<-Lfixture/Context;+Ljava/lang/Object;>;"
        "Lkotlin/jvm/functions/Function2<-Ljava/lang/Object;-Lkotlin/coroutines/Continuation<-TR;>;+Ljava/lang/Object;>;"
        "Lkotlin/coroutines/Continuation<-TR;>;)Ljava/lang/Object;")
INPUT=SOURCE.replace("Carrier$Slot<Ljava/lang/Object;>","Carrier$Slot<TT;>").replace("Function1<-Lfixture/Context;+Ljava/lang/Object;>","Function1<-Lfixture/Context;+TT;>").replace("Function2<-Ljava/lang/Object;-","Function2<-TT;-")
SYNTHETICS={"<init>(Ljava/util/concurrent/atomic/AtomicReference;)V":2,
            "wrap(Ljava/util/concurrent/atomic/AtomicReference;)Lfixture/Carrier;":0x19,
            "unwrap()Ljava/util/concurrent/atomic/AtomicReference;":0x11}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-projected-nested-session-callback-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"projected_nested_session_callback";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();std=args.kotlin_lib.resolve()/"kotlin-stdlib.jar";inputs={str(p):sha(p) for p in (args.android_jar.resolve(),std)};decl=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(decl,"x") as dest:
            for e in source.infolist():
                if e.filename.endswith(".class") and not e.filename.startswith("META-INF/"):
                    if ".." in Path(e.filename).parts or e.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(e.filename,source.read(e))
        authored=w/"authored";originals=w/"originals";harness=w/"harness";name="ProjectedNestedSessionCallbackHarness";v.compile_local(sorted((fixture/"java").rglob("*.java")),authored,classpath=std,release=8);v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=os.pathsep.join(map(str,(authored,std))),release=8)
        authored_hashes,harness_hashes=hashes(authored),hashes(harness);all_classes,all_methods=compiler_inventory(v,authored);before=compiler_facts(authored)
        if (len(all_classes),len(all_methods))!=(5,11) or before[OWNER]["methods"][ENTRY]["signature"]!=SOURCE:raise RuntimeError("Original projected session inventory or source Signature changed")
        def execute(runtime,form,label):
            out=[]
            for route in range(3):
                for kind in range(2):
                    for mode in range(14):
                        value=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(kind),str(mode),form],f"{label}-{route}-{kind}-{mode}")
                        if value!="attempts=2,projected-nested-session-callback=matched\n":raise RuntimeError("Frozen projected session observer changed")
                        out.append(value)
            return out
        baseline=execute((authored,harness,std),"source","authored");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n");shutil.copytree(authored,originals);target=originals/"fixture/Carrier.class";receipt={"signature":change_signature(target,SOURCE.encode(),INPUT.encode()),"flags":[]}
        for slot,flags in SYNTHETICS.items():
            member,proto=slot.split("(",1);receipt["flags"].append(change_method_flags(target,("("+proto).encode(),flags,flags|0x1000,name=member.encode()))
        (w/"owned-input-encoding.json").open("x").write(json.dumps(receipt,indent=2)+"\n");original_hashes=hashes(originals)
        if execute((originals,harness,std),"input","input")!=baseline:raise RuntimeError("Owned Signature encoding changed execution")
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"authored":authored_hashes,"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        wanted=copy.deepcopy(before);wanted[OWNER]["methods"][ENTRY]["signature"]=INPUT
        for slot in SYNTHETICS:wanted[OWNER]["methods"][OWNER+"->"+slot]["access"]|=0x1000
        if facts!=wanted:raise RuntimeError("Encoding changed metadata beyond exact one Signature and three wrapper flags")
        subject=fixture/"java/fixture/Carrier.java";text=subject.read_text();body="""        Trace.step(1,state,frame);
        Object element=factory.invoke(Trace.context);
        Trace.produced=element;
        Trace.step(2,state,frame);
        Object result=action.invoke(element,frame);
        Trace.returned=result;
        Trace.step(3,state,frame);
        return result;"""
        if text.count(body)!=1:raise RuntimeError("Projected session mutation anchor changed")
        wrong=[("lost-producer",body.replace("factory.invoke(Trace.context)","null"),0),
               ("duplicate-producer",body.replace("Trace.produced=element;","factory.invoke(Trace.context); Trace.produced=element;"),0),
               ("changed-context",body.replace("factory.invoke(Trace.context)","factory.invoke(new Context())"),0),
               ("changed-element",body.replace("action.invoke(element,frame)","action.invoke(null,frame)"),0),
               ("changed-frame",body.replace("action.invoke(element,frame)","action.invoke(element,null)"),0),
               ("discarded-result",body.replace("return result;","return null;"),0),
               ("changed-sentinel",body.replace("return result;","return result==kotlin.coroutines.intrinsics.IntrinsicsKt.getCOROUTINE_SUSPENDED()?new Object():result;"),8),
               ("swallowed-failure","try {\n"+body+"\n} catch(Throwable error) { return null; }",5),
               ("wrapped-failure","try {\n"+body+"\n} catch(Throwable error) { throw new IllegalStateException(error); }",6),
               ("runtime-content-guess","if(state!=null && state.get()!=null) ((Slot<Object>)(Object)state.get()).value();\n"+body,13)]
        for label,changed,mode in wrong:
            source=w/label/"fixture/Carrier.java";source.parent.mkdir(parents=True);source.open("x").write(text.replace(body,changed));compiled=w/(label+"-classes");v.compile_local([source],compiled,classpath=os.pathsep.join(map(str,(authored,std))),release=8);r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,authored,harness,std))),name,"0","0",str(mode),"source"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned projected nested session callback" not in r.stderr:raise RuntimeError("Observer missed wrong projected session: "+label)
        excluded={"Lfixture/Trace;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
        deps=[std,*([provider] if excluded else [])];dependency_hashes={str(p):sha(p) for p in (decl,provider)};cp=["--classpath",args.android_jar.resolve(),"--classpath",decl]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n");dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a projected session definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");c=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in c["methods"]}
        if set(rows)!=methods or c["class_count"]!=len(classes) or c["method_count"]!=len(methods) or c["unrecovered_method_count"] or c.get("minimum_java_release",8)!=8 or c["status"]!="partial" or c.get("class_source_bindings"):raise RuntimeError("Projected session coverage or scope changed")
        projected=c.get("generic_signature_projections",[]);expected=[{"kind":"static-carrier-type-variable","original_type":OWNER+"::T","scope_origin":OWNER,"source_type":"Ljava/lang/Object;","offset":m.start()} for m in re.finditer("TT;",INPUT)]
        if len(projected)!=1 or projected[0].get("declaration")!=ENTRY or projected[0].get("original_signature")!=INPUT or projected[0].get("changes")!=expected or not projected[0].get("metadata_differences"):raise RuntimeError("Missing exact three class-formal projections or changed method R")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in c.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes};rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)),release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Projected session source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers};wanted=copy.deepcopy(facts[owner])
            if owner==OWNER:
                wanted["methods"][ENTRY]["signature"]=SOURCE
                for slot in SYNTHETICS:wanted["methods"][OWNER+"->"+slot]["access"] &= ~0x1000
            if actual!=wanted or any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Projected session metadata changed beyond exact receipt/helpers/three flags: "+owner)
        if execute((rebuilt,harness,*deps),"source","rebuilt")!=baseline:raise RuntimeError("Projected session effects, bridge checks, result/frame identity or failure changed")
        if hashes(authored)!=authored_hashes or hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or any(sha(Path(p))!=h for p,h in {**inputs,**dependency_hashes}.items()):raise RuntimeError("Frozen projected session inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":84,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"class_formal_projections":3,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
