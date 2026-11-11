#!/usr/bin/env python3
"""Verify a generic compatibility child retains its closed boolean default."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,shutil,subprocess,tempfile,zipfile
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-closed-default-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"inherited_closed_default";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(p):sha(p) for p in inputs}
        declarations=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declarations,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                    if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(entry.filename,source.read(entry))
        java=w/"java";parent=w/"parent";child=w/"child";originals=w/"originals";harness=w/"harness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),java,release=8)
        compiler=[v.java,"-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-jvm-target","1.8","-Xno-param-assertions","-Xno-call-assertions"]
        v.run([*compiler,"-classpath",os.pathsep.join(map(str,(std,java))),"-Xjvm-default=all","-d",parent,*sorted((fixture/"parent").rglob("*.kt"))],"kotlinc-parent")
        v.run([*compiler,"-classpath",os.pathsep.join(map(str,(std,java,parent))),"-Xjvm-default=all-compatibility","-d",child,*sorted((fixture/"child").rglob("*.kt"))],"kotlinc-child")
        originals.mkdir()
        for directory in (java,parent,child):
            for file in directory.rglob("*.class"):
                dest=originals/file.relative_to(directory);dest.parent.mkdir(parents=True,exist_ok=True)
                if dest.exists():raise RuntimeError("Duplicate original definition")
                shutil.copy2(file,dest)
        names=("InheritedClosedDefaultCaller","InheritedClosedDefaultHarness")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=9)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(4,5):raise RuntimeError("Original default definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        access="Lfixture/Child;->access$matches$jd(Lfixture/Child;Lkotlin/jvm/functions/Function1;)Z";forward="Lfixture/Child$DefaultImpls;->matches(Lfixture/Child;Lkotlin/jvm/functions/Function1;)Z"
        if facts["Lfixture/Child;"]["methods"][access]["access"]!=0x1009 or facts["Lfixture/Child;"]["methods"][access]["signature"] is not None or facts["Lfixture/Child;"]["signature"]!="<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Parent;":raise RuntimeError("Original generic owner/accessor shape changed")
        if set(facts["Lfixture/Parent;"]["methods"])!={"Lfixture/Parent;->matches(Lkotlin/jvm/functions/Function1;)Z"}:raise RuntimeError("Unexpected ancestor compatibility accessor")
        excluded={"Lfixture/Trace;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
        deps=[std,*([provider] if excluded else [])];dependency_hashes={str(f):sha(f) for f in (declarations,provider)}
        def execute(runtime,label,accessor,forwarded,projected):
            out=[]
            for route in range(6):
                for alias in range(2):
                    for mode in range(9):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(alias),str(mode),accessor,forwarded,str(projected).lower()],f"{label}-{route}-{alias}-{mode}")
                        if text!="attempts=2,inherited-closed-default=matched\n":raise RuntimeError("Frozen default observer changed")
                        out.append(text)
            return out
        baseline=execute((originals,harness,*deps),"baseline","access$matches$jd","matches",False);(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        prefix="Trace.enter(this,predicate);";invoke="return predicate.invoke(this);"
        wrong=[("lost-callback",prefix+"return true;",0),
               ("negated-result",prefix+"return !predicate.invoke(this);",0),
               ("lost-trace",invoke,0),
               ("late-trace","boolean result=predicate.invoke(this);"+prefix+"return result;",0),
               ("duplicate-trace",prefix+prefix+invoke,0),
               ("duplicate-callback",prefix+"predicate.invoke(this);"+invoke,0),
               ("null-callback-argument",prefix+"return predicate.invoke(null);",0),
               ("early-null-check","predicate.getClass();"+prefix+invoke,5),
               ("wrapped-failure",prefix+"try{"+invoke+"}catch(RuntimeException error){throw new RuntimeException(error);}",3),
               ("swallowed-failure",prefix+"try{"+invoke+"}catch(RuntimeException error){return false;}",3),
               ("null-result-default",prefix+"Boolean value=predicate.invoke(this);return value!=null && value;",2),
               ("receiver-effect",prefix+"this.toString();"+invoke,0)]
        for label,body,mode in wrong:
            file=w/label/"fixture/Parent.java";file.parent.mkdir(parents=True);file.open("x").write("package fixture;import kotlin.jvm.functions.Function1;public interface Parent {default boolean matches(Function1<? super Parent,Boolean> predicate){"+body+"}}\n")
            compiled=w/(label+"-classes");v.compile_local([file],compiled,classpath=os.pathsep.join(map(str,(originals,std))),release=9)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*deps))),names[1],"0","0",str(mode),"access$matches$jd","matches","false"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned inherited closed default" not in r.stderr:raise RuntimeError("Observer missed wrong default: "+label)
        child_prefix="package fixture;import kotlin.jvm.functions.Function1;public interface Child<T> extends Parent {private boolean exact(Function1 predicate){return Parent.super.matches(predicate);}static boolean access$matches$jd(Child receiver,Function1 predicate){"
        child_suffix="}public static final class DefaultImpls{public static <T> boolean matches(Child<T> receiver,Function1<? super Parent,Boolean> predicate){return "
        for label,body,tail,route in [("virtual-redispatch","return receiver.matches(predicate);","Child.access$matches$jd(receiver,predicate);",0),("null-dispatch-receiver","return ((Child)null).exact(predicate);","Child.access$matches$jd(receiver,predicate);",0),("wrong-forward-result","return receiver.exact(predicate);","!Child.access$matches$jd(receiver,predicate);",3),("forward-virtual-redispatch","return receiver.exact(predicate);","receiver.matches(predicate);",3)]:
            file=w/label/"fixture/Child.java";file.parent.mkdir(parents=True);file.open("x").write(child_prefix+body+child_suffix+tail+"}}}\n")
            compiled=w/(label+"-classes");v.compile_local([file],compiled,classpath=os.pathsep.join(map(str,(originals,std))),release=9)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*deps))),names[1],str(route),"0","0","access$matches$jd","matches","true"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned inherited closed default" not in r.stderr:raise RuntimeError("Observer missed wrong default forwarding: "+label)
        cp=["--classpath",args.android_jar.resolve(),"--classpath",declarations]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex={str(dex):sha(dex)};(w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a default definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or coverage.get("minimum_java_release")!=9 or coverage.get("generic_signature_projections"):raise RuntimeError("Original default coverage/Signature changed")
        a=rows[access];accessor=a.get("source_name","access$matches$jd");forwarded=rows[forward].get("source_name","matches")
        if a.get("body_projection_kind",a.get("projection_kind"))!="static-super-accessor" or not a.get("super_dispatch_helper"):raise RuntimeError("Missing exact scoped super helper receipt")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)),release=9)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt);mapped={m:m.split("->",1)[0]+"->"+r["source_name"]+m[m.index("("):] for m,r in rows.items() if r.get("source_name")};helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}|{a["super_dispatch_helper"],"Lfixture/Child$DefaultImpls;-><init>()V"}
        if actual_classes!=classes or set(actual_methods)!={mapped.get(m,m) for m in methods}|helpers or not helpers<=allowed or a["super_dispatch_helper"] not in helpers:raise RuntimeError("Default source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={};debug={};versions={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);adjusted={}
            for m,row in wanted["methods"].items():
                if rows[m].get("source_name"):row["name"]=rows[m]["source_name"]
                if m==access:row["access"]&=~0x1000
                adjusted[mapped.get(m,m)]=row
            wanted["methods"]=adjusted
            for identity,row in [(owner,wanted),*wanted["methods"].items(),*wanted["fields"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [ann for slot in value for ann in slot]
                    if uses:
                        if any(ann["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;"} for ann in uses):raise RuntimeError("Unexpected default CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers};original_file="Trace.java" if owner=="Lfixture/Trace;" else "Parent.kt" if owner=="Lfixture/Parent;" else "Child.kt";source_file=original_file.replace(".kt",".java")
            if wanted.get("source_file")!=original_file or actual.get("source_file")!=source_file:raise RuntimeError("Unexpected default source filename")
            debug[owner]={"original":original_file,"rebuilt":source_file};wanted["source_file"]=source_file
            if wanted.get("major")!=52 or actual.get("major")!=53:raise RuntimeError("Unexpected default class version beyond required Java 9 helper")
            versions[owner]={"original":52,"rebuilt":53,"minimum_java_release":9};wanted["major"]=53
            if actual!=wanted:raise RuntimeError("Default metadata changed beyond exact reported differences: "+owner)
            if any(after_exceptions[mapped.get(m,m)]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Default Exceptions changed")
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n");(w/"source-file-differences.json").open("x").write(json.dumps(debug,indent=2)+"\n")
        (w/"class-version-differences.json").open("x").write(json.dumps(versions,indent=2)+"\n")
        if execute((rebuilt,harness,*deps),"rebuilt",accessor,forwarded,True)!=baseline:raise RuntimeError("Default boolean/dispatch/alias/partial failure semantics changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(f))!=h for f,h in {**input_hashes,**dependency_hashes,**frozen_dex}.items()):raise RuntimeError("Frozen default inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":108,"attempts_per_case":2,"wrong_programs":16,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
