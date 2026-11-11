#!/usr/bin/env python3
"""Verify a final generic instance entry owns its disclosed Java spelling."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,subprocess,tempfile,zipfile
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
    for name in ("neverd","android-jar","kotlin-lib"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-public-final-generic-names-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"public_final_generic_names";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(p):sha(p) for p in inputs};decl=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(decl,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                    if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(entry.filename,source.read(entry))
        originals=w/"originals";harness=w/"harness";name="PublicFinalGenericNamesHarness";compiler=[v.java,"-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-jvm-target","1.8","-Xno-param-assertions","-Xno-call-assertions","-Xlambdas=class","-Xno-source-debug-extension"]
        file=fixture/"kotlin/Core.kt";source_text=file.read_text();v.run([*compiler,"-classpath",std,"-d",originals,file],"kotlinc-original");v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(4,10):raise RuntimeError("Original public final generic names definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded={"Lfixture/Foundation;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for owner in ("fixture/Foundation",):jar.writestr(owner+".class",(originals/(owner+".class")).read_bytes())
        deps=[std,*([provider] if excluded else [])];dependency_hashes={str(f):sha(f) for f in (decl,provider)}
        original="Lfixture/Root;->walk-stage(ILkotlin/jvm/functions/Function1;)V"
        signature="<T:Ljava/lang/Object;>(ILkotlin/jvm/functions/Function1<-TT;Lkotlin/Unit;>;)V"
        if facts["Lfixture/Root;"]["methods"][original]["access"]!=0x1011 or facts["Lfixture/Root;"]["methods"][original]["signature"]!=signature:raise RuntimeError("Original public final synthetic generic contract changed")
        def execute(runtime,spelling,label):
            out=[]
            for route in range(4):
                for kind in range(2):
                    for mode in range(10):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(kind),str(mode),spelling],f"{label}-{route}-{kind}-{mode}")
                        if text!="attempts=2,public-final-generic-names=matched\n":raise RuntimeError("Frozen public final generic observer changed")
                        out.append(text)
            return out
        baseline=execute((originals,harness,std),"walk-stage","baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        body="""        if (bits == 0) return
        before(bits)
        action(seed as T)
        after()"""
        if source_text.count(body)!=1:raise RuntimeError("Public final generic mutation anchor changed")
        wrong=[("lost-callback",body.replace("action(seed as T)","seed as T"),0),
               ("duplicate-callback",body.replace("action(seed as T)","action(seed as T); action(seed as T)"),0),
               ("changed-input",body.replace("action(seed as T)","action(null as T)"),0),
               ("callback-before-hook",body.replace("before(bits)\n        action(seed as T)","action(seed as T)\n        before(bits)"),0),
               ("lost-after",body.replace("after()",""),0),
               ("swallowed-failure","try {\n"+body+"\n} catch(error: Throwable) { return }",6),
               ("wrapped-failure","try {\n"+body+"\n} catch(error: Throwable) { throw IllegalStateException(error) }",6),
               ("early-callback-check","action.hashCode()\n"+body,0),
               ("changed-scalar",body.replace("before(bits)","before(0)"),0),
               ("lost-short-circuit",body.replace("if (bits == 0) return",""),1)]
        for label,replacement,mode in wrong:
            file=w/label/"Core.kt";file.parent.mkdir(parents=True);file.open("x").write(source_text.replace(body,replacement));compiled=w/(label+"-classes");v.run([*compiler,"-classpath",std,"-d",compiled,file],"kotlinc-"+label)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,harness,std))),name,"0","0",str(mode),"walk-stage"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned public final generic names" not in r.stderr:raise RuntimeError("Observer missed wrong public final generic entry: "+label)
        cp=["--classpath",args.android_jar.resolve(),"--classpath",decl]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a public final generic names definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");c=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in c["methods"]}
        row=rows.get(original,{});spelling=row.get("source_name");renamed=original.replace("->walk-stage(","->"+str(spelling)+"(")
        if set(rows)!=methods or c["class_count"]!=len(classes) or c["method_count"]!=len(methods) or c["unrecovered_method_count"] or c.get("generic_signature_projections") or c.get("minimum_java_release",8)!=8 or c.get("projected_method_count")!=1 or c["status"]!="partial":raise RuntimeError("Public final generic scope, coverage or Signature changed")
        if not spelling or row.get("source_identity")!=renamed or row.get("source_name_declaration")!=original or row.get("source_name_family")!=[original] or row.get("projection_kind")!="virtual-method-identifier" or row.get("generic_signature")!=signature:raise RuntimeError("Missing exact public final generic declaring receipt")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in c.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}|{"Lfixture/CoreKt;-><init>()V"};default={"class":"Lfixture/CoreKt;","kind":"default-constructor","name":"<init>","prototype":"()V","source_unit":"fixture/CoreKt.java","static":False}
        if default not in c.get("generated_source_helpers",[]):raise RuntimeError("Missing exact reported facade default constructor")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)),release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!=(methods-{original})|{renamed}|helpers or not helpers<=allowed:raise RuntimeError("Public final generic names source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);debug={};dropped={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            if original in wanted["methods"]:
                member=wanted["methods"].pop(original);member["name"]=spelling;member["access"] &= ~0x1000;wanted["methods"][renamed]=member
            for identity,row in [(owner,wanted),*wanted["fields"].items(),*wanted["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;","Lkotlin/jvm/JvmField;"} for a in uses):raise RuntimeError("Unexpected public final generic names CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            source_file=owner.rsplit("/",1)[1][:-1]+".java"
            if wanted.get("source_file")!="Core.kt" or actual.get("source_file")!=source_file:raise RuntimeError("Unexpected public final generic names source filename")
            debug[owner]={"original":"Core.kt","rebuilt":source_file};wanted["source_file"]=source_file
            if actual!=wanted:raise RuntimeError("Public final generic names metadata changed beyond reported differences: "+owner)
            if any(after_exceptions[renamed if m==original else m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Public final generic names Exceptions changed")
        (w/"source-file-differences.json").open("x").write(json.dumps(debug,indent=2)+"\n");(w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        if execute((rebuilt,harness,*deps),spelling,"rebuilt")!=baseline:raise RuntimeError("Public final generic names input, bridge casts, exceptions or partial effects changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or any(sha(Path(f))!=h for f,h in {**input_hashes,**dependency_hashes}.items()):raise RuntimeError("Frozen public final generic names inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":80,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
