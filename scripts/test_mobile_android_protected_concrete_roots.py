#!/usr/bin/env python3
"""Verify independent concrete protected roots preserve complete virtual families."""
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


OWNERS=("Lfixture/Parent;","Lfixture/OverrideChild;","Lfixture/PublicPeer;","Lfixture/PublicChild;")
METHODS=tuple(o+"->write-value(J)V" for o in OWNERS)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-concrete-roots-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"protected_concrete_roots";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(p):sha(p) for p in inputs};declarations=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declarations,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                    if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(entry.filename,source.read(entry))
        originals=w/"originals";harness=w/"harness";name="ProtectedConcreteRootsHarness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        compiler=[v.java,"-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-jvm-target","1.8","-Xno-param-assertions","-Xno-call-assertions","-Xlambdas=class"]
        source_file=fixture/"kotlin/Core.kt";source_text=source_file.read_text();v.run([*compiler,"-classpath",os.pathsep.join(map(str,(std,originals))),"-d",originals,source_file],"kotlinc-original")
        v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(7,19):raise RuntimeError("Original concrete-root definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded={"Lfixture/Trace;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
        deps=[std,*([provider] if excluded else [])];dependency_hashes={str(f):sha(f) for f in (declarations,provider)}
        def execute(runtime,label,names):
            out=[]
            for kind in range(4):
                for route in range(2):
                    for mode in range(13):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(kind),str(route),str(mode),*names],f"{label}-{kind}-{route}-{mode}")
                        if text!="attempts=2,protected-concrete-roots=matched\n":raise RuntimeError("Frozen concrete-root observer changed")
                        out.append(text)
            return out
        baseline=execute((originals,harness,std),"baseline",["write-value"]*4);(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        parent="Trace.before(this,value,1)\n        stored=value\n        Trace.after()";override="Trace.before(this,value,2)\n        chosen=value\n        Trace.after()";public="Trace.before(this,value,3)\n        stored=value\n        Trace.after()";send="fun send(value: Long) { `write-value`(value) }"
        if any(source_text.count(x)!=count for x,count in ((parent,1),(override,1),(public,1),(send,2))):raise RuntimeError("Concrete-root source mutation anchors changed")
        wrong=[("lost-root-body",source_text.replace(parent,"return"),0,0,0),
               ("changed-bits",source_text.replace(parent,parent.replace("stored=value","stored=0")),0,0,5),
               ("duplicate-before",source_text.replace(parent,"Trace.before(this,value,1)\n        "+parent),0,0,0),
               ("early-store",source_text.replace(parent,"stored=value\n        Trace.before(this,value,1)\n        Trace.after()"),0,0,8),
               ("lost-override",source_text.replace(override,"super.`write-value`(value)"),1,0,0),
               ("direct-root-dispatch",source_text.replace(send,"fun send(value: Long) { "+parent+" }",1),1,0,0),
               ("wrong-public-component",source_text.replace(public,public.replace(",3)",",1)")),2,1,0),
               ("narrow-long-store",source_text.replace(parent,parent.replace("stored=value","stored=value.toInt().toLong()")),0,0,4),
               ("wrapped-failure",source_text.replace(parent,"try { "+parent+" } catch(error: Throwable) { throw IllegalStateException(error) }"),0,0,8),
               ("swallowed-failure",source_text.replace(parent,"try { "+parent+" } catch(error: Throwable) { return }"),0,0,9)]
        for label,text,kind,route,mode in wrong:
            file=w/label/"Core.kt";file.parent.mkdir(parents=True);file.open("x").write(text);compiled=w/(label+"-classes");v.run([*compiler,"-classpath",os.pathsep.join(map(str,(std,originals))),"-d",compiled,file],"kotlinc-"+label)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,std))),name,str(kind),str(route),str(mode),*(["write-value"]*4)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned protected concrete roots" not in r.stderr:raise RuntimeError("Observer missed wrong concrete-root program: "+label)
        cp=["--classpath",args.android_jar.resolve(),"--classpath",declarations]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a concrete-root definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Concrete-root coverage or complete Signature changed")
        aliases=[rows[m].get("source_name") for m in METHODS]
        if not all(aliases) or aliases[0]!=aliases[1] or aliases[2]!=aliases[3]:raise RuntimeError("Concrete-root overrides lost their source spelling")
        for index,identity in enumerate(METHODS):
            root=METHODS[0 if index<2 else 2];wanted={METHODS[0],METHODS[1]} if index<2 else {METHODS[2],METHODS[3]};family=set(rows[identity].get("source_name_family",[]))
            if rows[identity].get("source_name_declaration")!=root or not wanted<=family<=set(METHODS) or any(rows[m].get("source_name")!=aliases[index] for m in family):raise RuntimeError("Concrete root borrowed another declaration or incomplete family")
        mapped={m:m.split("->",1)[0]+"->"+r["source_name"]+m[m.index("("):] for m,r in rows.items() if r.get("source_name")};helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)),release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!={mapped.get(m,m) for m in methods}|helpers or not helpers<=allowed:raise RuntimeError("Concrete-root source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);debug={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);adjusted={}
            for m,row in wanted["methods"].items():
                if rows[m].get("source_name"):row["name"]=rows[m]["source_name"]
                adjusted[mapped.get(m,m)]=row
            wanted["methods"]=adjusted;actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers};original_file="Trace.java" if owner=="Lfixture/Trace;" else "Core.kt";source_file=owner.rsplit("/",1)[1][:-1]+".java"
            if wanted.get("source_file")!=original_file or actual.get("source_file")!=source_file:raise RuntimeError("Unexpected concrete-root source filename")
            debug[owner]={"original":original_file,"rebuilt":source_file};wanted["source_file"]=source_file
            if actual!=wanted:raise RuntimeError("Concrete-root metadata changed beyond exact identifier and source file differences: "+owner)
            if any(after_exceptions[mapped.get(m,m)]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Concrete-root Exceptions changed")
        (w/"source-file-differences.json").open("x").write(json.dumps(debug,indent=2)+"\n")
        if execute((rebuilt,harness,*deps),"rebuilt",aliases)!=baseline:raise RuntimeError("Concrete-root virtual dispatch, wide values or partial failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or any(sha(Path(f))!=h for f,h in {**input_hashes,**dependency_hashes}.items()):raise RuntimeError("Frozen concrete-root inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":104,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
