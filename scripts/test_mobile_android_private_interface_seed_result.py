#!/usr/bin/env python3
"""Verify a private interface-bounded seed/result keeps effects and caller casts."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-interface-seed-result-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"private_interface_seed_result";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(p):sha(p) for p in inputs};decl=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(decl,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                    if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(entry.filename,source.read(entry))
        originals=w/"originals";java_originals=w/"original-java";harness=w/"harness";name="PrivateInterfaceSeedResultHarness";compiler=[v.java,"-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-jvm-target","1.8","-Xno-param-assertions","-Xno-call-assertions","-Xlambdas=class"]
        file=fixture/"kotlin/Core.kt";source_text=file.read_text();v.compile_local(sorted((fixture/"java").rglob("*.java")),java_originals,release=8);original_cp=os.pathsep.join(map(str,(std,java_originals)));v.run([*compiler,"-classpath",original_cp,"-d",originals,file],"kotlinc-original");
        import shutil
        for path in java_originals.rglob("*.class"):
            target=originals/path.relative_to(java_originals);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
        v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(2,6):raise RuntimeError("Original private interface seed result definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded={"Lfixture/Text;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for owner in ("fixture/Text",):jar.writestr(owner+".class",(originals/(owner+".class")).read_bytes())
        deps=[std,*([provider] if excluded else [])];dependency_hashes={str(f):sha(f) for f in (decl,provider)}
        def execute(runtime,label):
            out=[]
            for route in range(4):
                for mode in range(15):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
                    if text!="attempts=2,private-interface-seed-result=matched\n":raise RuntimeError("Frozen private interface seed observer changed")
                    out.append(text)
            return out
        baseline=execute((originals,harness,std),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        body="""        calls++
        require(size > 0)
        if (value == null || value.length <= size) return value
        return value.subSequence(0,size) as T"""
        if source_text.count(body)!=1:raise RuntimeError("Private interface mutation anchor changed")
        wrong=[("lost-call",body.replace("calls++",""),0,0),
               ("duplicate-subsequence",body.replace("return value.subSequence(0,size)","value.subSequence(0,size); return value.subSequence(0,size)"),0,0),
               ("changed-size",body.replace("subSequence(0,size)","subSequence(0,size+1)"),0,0),
               ("new-concrete-cast",body.replace("as T","as Text as T"),0,0),
               ("lost-null-result-check",body.replace("as T","as T?"),0,2),
               ("moved-null-guard",body.replace("require(size > 0)\n        if (value == null || value.length <= size) return value","if(value==null) return null\n        require(size > 0)\n        if(value.length<=size) return value"),0,14),
               ("lost-guard",body.replace("require(size > 0)",""),0,7),
               ("new-text-conversion","value?.toString()\n"+body,0,0),
               ("swallowed-failure","try {\n"+body+"\n} catch(error: Throwable) { return value }",0,5),
               ("wrapped-failure","try {\n"+body+"\n} catch(error: Throwable) { throw IllegalStateException(error) }",0,6)]
        for label,replacement,route,mode in wrong:
            file=w/label/"Core.kt";file.parent.mkdir(parents=True);file.open("x").write(source_text.replace(body,replacement));compiled=w/(label+"-classes");v.run([*compiler,"-classpath",original_cp,"-d",compiled,file],"kotlinc-"+label)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,std))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned private interface seed result" not in r.stderr:raise RuntimeError("Observer missed wrong private interface: "+label)
        cp=["--classpath",args.android_jar.resolve(),"--classpath",decl]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a private interface seed result definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");c=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in c["methods"]}
        if set(rows)!=methods or c["class_count"]!=len(classes) or c["method_count"]!=len(methods) or c["unrecovered_method_count"] or c.get("generic_signature_projections") or c.get("minimum_java_release",8)!=8:raise RuntimeError("Private interface seed result coverage, scope or full Signature changed")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in c.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)),release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Private interface seed result source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);debug={};dropped={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            for identity,row in [(owner,wanted),*wanted["fields"].items(),*wanted["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;","Lkotlin/jvm/JvmField;"} for a in uses):raise RuntimeError("Unexpected private interface seed result CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            source_file=owner.rsplit("/",1)[1][:-1]+".java"
            if wanted.get("source_file")!=("Core.kt" if owner=="Lfixture/Core;" else "Text.java") or actual.get("source_file")!=source_file:raise RuntimeError("Unexpected private interface seed result source filename")
            debug[owner]={"original":wanted["source_file"],"rebuilt":source_file};wanted["source_file"]=source_file
            if actual!=wanted:raise RuntimeError("Private interface seed result metadata changed beyond reported differences: "+owner)
            if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Private interface seed result Exceptions changed")
        (w/"source-file-differences.json").open("x").write(json.dumps(debug,indent=2)+"\n");(w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Private interface seed result input, bridge casts, exceptions or partial effects changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or any(sha(Path(f))!=h for f,h in {**input_hashes,**dependency_hashes}.items()):raise RuntimeError("Frozen private interface seed result inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":60,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
