#!/usr/bin/env python3
"""Verify constructor prefixes retain lexically owned callback singletons."""
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
except ModuleNotFoundError as error:
    if error.name!="scripts":raise
    from mobile_real_apps_android import dex_header,parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-local-static-prefix-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"local_static_prefix";k=args.kotlin_lib.resolve()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))]
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        input_hashes={str(p):sha(p) for p in inputs}
        std=k/"kotlin-stdlib.jar";runtime_deps=[std]
        declaration=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declaration,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):dest.writestr(entry.filename,source.read(entry))
        java=w/"original-java";originals=w/"originals";harness=w/"harness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),java,release=8);originals.mkdir()
        v.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(std,java))),"-jvm-target","1.8","-Xlambdas=class","-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        for f in java.rglob("*.class"):
            to=originals/f.relative_to(java);to.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,to)
        names=("LocalStaticPrefixCaller","LocalStaticPrefixHarness","LocalStaticPrefixProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(5,13):raise RuntimeError("Original interface definition inventory changed")
        excluded={"Lfixture/Trace;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records=compiler_facts(originals);exceptions=exception_inventory(originals)
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
        classpath=["--classpath",args.android_jar.resolve(),"--classpath",declaration]
        if excluded:classpath += ["--classpath",provider];runtime_deps.append(provider)
        dependency_hashes={str(f):sha(f) for f in (declaration,provider)}
        entry="Lfixture/Core;-><init>(IILkotlin/jvm/functions/Function1;Lkotlin/jvm/functions/Function1;ILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        def execute(runtime,label):
            out=[]
            for route in range(4):
                for mode in range(32):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen callback singleton observer changed: "+text)
                    out.append(text)
            return out
        baseline=execute((originals,harness,*runtime_deps),"baseline")
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
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations)!=["Lkotlin/Metadata;"]*4:raise RuntimeError("D8 annotation retention changed")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or coverage.get("minimum_java_release")!=25:raise RuntimeError("Original singleton definitions or release changed")
        bindings=coverage.get("class_source_bindings",[])
        locals={"Lfixture/Core$1;","Lfixture/Core$2;"}
        if len(bindings)!=2 or {b["class"] for b in bindings}!=locals or any(b["binding_kind"]!="anonymous-to-shared-member" or b["source_enclosing_class"]!="Lfixture/Core;" or b["enclosing_method"]["identity"]!=entry or not b.get("metadata_differences") for b in bindings):raise RuntimeError("Missing exact shared callback owner receipt")
        renames={b["class"]:b["source_enclosing_class"][:-1]+"$"+b["source_name"]+";" for b in bindings}
        def mapped(value):
            for old,new in renames.items():value=value.replace(old,new)
            return value
        projections=coverage.get("generic_signature_projections",[])
        if len(projections)!=2 or {r["declaration"] for r in projections}!=locals or any(r["declaration_kind"]!="class" or len(r["changes"])!=1 or r["changes"][0]["kind"]!="raw-marker-superclass-argument" or r["changes"][0]["source_type"]!="Lkotlin/jvm/internal/Lambda<Ljava/lang/Object;>;" or not r.get("metadata_differences") for r in projections):raise RuntimeError("Unexpected callback Signature projection")
        if not any("synthetic" in d for d in rows[entry].get("metadata_differences",[])):raise RuntimeError("Synthetic default flag loss not reported")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,(std,provider) if excluded else (std,))),release=25)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={mapped(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={mapped(c)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!={mapped(c) for c in classes} or set(actual_methods)!={mapped(m) for m in methods}|expected_helpers or helpers!=expected_helpers:raise RuntimeError("Singleton source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={}
        by_binding={b["class"]:b for b in bindings}
        for owner in classes:
            wanted=copy.deepcopy(records[owner])
            wanted["fields"]={mapped(i):r for i,r in wanted["fields"].items()}
            for r in wanted["fields"].values():
                r["descriptor"]=mapped(r["descriptor"])
                if r.get("signature"):r["signature"]=mapped(r["signature"])
            if owner in locals:
                b=by_binding[owner]
                wanted["inner_class"]={"inner":mapped(owner),"outer":"Lfixture/Core;","name":b["source_name"],"access":24}
                wanted["enclosing_method"]=None
                if not wanted["signature"].startswith("Lkotlin/jvm/internal/Lambda;"):raise RuntimeError("Original raw lambda parent changed")
                wanted["signature"]=wanted["signature"].replace("Lkotlin/jvm/internal/Lambda;","Lkotlin/jvm/internal/Lambda<Ljava/lang/Object;>;",1)
            for identity,row in wanted["methods"].items():
                if identity==entry:row["access"]&=~0x1000
            for identity,row in [(owner,wanted),*wanted["methods"].items(),*wanted["fields"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;","Lkotlin/jvm/JvmField;"} for a in uses):raise RuntimeError("Unexpected genuine CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            actual=after[mapped(owner)]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Callback metadata changed beyond exact receipts: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(mapped(identity))!=row or after_exceptions[mapped(identity)]!=exceptions[identity]:raise RuntimeError("Callback method metadata/Exceptions changed: "+identity)
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Singleton selection, identity, effects or failures changed")
        trace=(fixture/"java/fixture/Trace.java").read_text();core=(fixture/"kotlin/fixture/Core.kt").read_text()
        early=trace.replace("seen=value;","").replace("step(5);","seen=value;step(5);")
        fresh=core.replace("@JvmField val first:","first:").replace("    init {","    @JvmField val first: (Any?)->Any?={first(it)}\n    init {")
        wrong=(("wrong-first-tag","Core",core,"Trace.accept(1,it)","Trace.accept(2,it)",7),
               ("wrong-second-tag","Core",core,"Trace.accept(2,it)","Trace.accept(1,it)",7),
               ("wrong-capacity","Core",core,"Int=7","Int=8",7),
               ("lost-first-input","Core",core,"Trace.accept(1,it)","Trace.accept(1,null)",31),
               ("fresh-first-callback","Core",core,core,fresh,7),
               ("early-observation","Core",core,"Trace.step(3)","Trace.firstSeen=first;Trace.step(3)",16),
               ("wrong-partial","Core",core,"Trace.partial=this","Trace.partial=null",0),
               ("wrong-result","Trace",trace,"return result;","return value;",0),
               ("wrong-input","Trace",trace,"seen=value;","seen=new Object();",0),
               ("lost-tag","Trace",trace,"tags=tags*10+tag;","tags=0;",0),
               ("lost-step","Trace",trace,"step(6);","",0),
               ("duplicate-step","Trace",trace,"step(5);","step(5);step(5);",0),
               ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",24),
               ("lost-failure","Trace",trace,"if(fail==value)","if(false)",24),
               ("early-payload","Trace",trace,trace,early,24))
        for label,owner,body,old,new,mode in wrong:
            if body.count(old)!=1:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+(".kt" if owner=="Core" else ".java"));source.parent.mkdir();source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            if owner=="Core":v.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(std,originals))),"-jvm-target","1.8","-Xlambdas=class","-d",compiled,source],"kotlinc-"+label)
            else:v.compile_local([source],compiled,release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned local static prefix" not in r.stderr:raise RuntimeError("Frozen observer missed wrong singleton program: "+label)
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(f))!=h for f,h in input_hashes.items()) or any(sha(Path(f))!=h for f,h in dependency_hashes.items()):raise RuntimeError("Frozen compiler/inputs/observer/dependencies changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":128,"attempts_per_case":2,"wrong_programs":15,"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
