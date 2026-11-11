#!/usr/bin/env python3
"""Verify nominally inapplicable generic constructor siblings with genuine masks."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-nominal-overload-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"closed_nominal_overload";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar"
        sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(f):sha(f) for f in inputs}
        declarations=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declarations,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                    if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(entry.filename,source.read(entry))
        java=w/"original-java";originals=w/"originals";harness=w/"harness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),java,release=8);originals.mkdir()
        def kotlin(sources,output,label):
            v.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(std,java,originals))),"-jvm-target","1.8","-d",output,*sources],label)
        kotlin(sorted((fixture/"kotlin").rglob("*.kt")),originals,"kotlinc-original")
        for f in java.rglob("*.class"):
            target=originals/f.relative_to(java);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,target)
        external=w/"kotlin-harness";kotlin(sorted((fixture/"harness_kotlin").rglob("*.kt")),external,"kotlinc-external-invoker")
        names=("ClosedNominalOverloadCaller","ClosedNominalOverloadHarness","ClosedNominalOverloadProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=os.pathsep.join(map(str,(originals,external,std))),release=8)
        for f in external.rglob("*"):
            if f.is_file():
                target=harness/f.relative_to(external);target.parent.mkdir(parents=True,exist_ok=True)
                if target.exists():raise RuntimeError("External observer collision")
                shutil.copy2(f,target)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(5,9):raise RuntimeError("Original nominal overload definition inventory changed")
        excluded={"Lfixture/Context;","Lfixture/Handler;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);core=facts["Lfixture/Core;"]
        suffix="(Ljava/lang/Object;Ljava/lang/Object;[Ljava/lang/Object;Lfixture/Handler;"
        target="Lfixture/Core;-><init>"+suffix+")V";entry="Lfixture/Core;-><init>"+suffix+"ILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        if core["signature"] is not None or len(core["methods"])!=3 or any(core["methods"][m]["signature"] is not None for m in (target,entry)):raise RuntimeError("Genuine raw primary/default constructor signature changed")
        synthetic={m for m,r in core["methods"].items() if r["access"]&0x1000}
        if synthetic!={entry} or sum(r["signature"] is not None for r in core["methods"].values())!=1:raise RuntimeError("Missing genuine closed generic sibling")
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for name in ("Handler","Context"):jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        runtime_deps=[std,*([provider] if excluded else [])]
        dependency_hashes={str(f):sha(f) for f in (declarations,provider,*external.rglob("*.class"),*external.rglob("*.kotlin_module"))}
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for kind in range(2):
                    for mode in range(30):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(kind),str(mode)],f"{label}-{route}-{kind}-{mode}")
                        pattern=f"route={route},kind={kind},mode={mode},attempt=[01],events=[0-9]+,mask=-?[0-9]+"
                        if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen nominal overload observer changed: "+text)
                        result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline")
        (w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        code=(fixture/"kotlin/fixture/Core.kt").read_text();trace=(fixture/"java/fixture/Trace.java").read_text();factory=(fixture/"java/fixture/Provider.java").read_text()
        wrong=(("wrong-first-default","Core",code,"val first: Any? = null","val first: Any? = 17",0,1),
               ("wrong-second-default","Core",code,"val second: Any? = null","val second: Any? = first",0,2),
               ("wrong-array-default","Core",code,"val rest: Array<out Any?>? = null","val rest: Array<out Any?>? = emptyArray()",0,4),
               ("wrong-high-mask","Core",code,"val handler: Handler?","val handler: Handler? = null",0,8),
               ("missing-factory","Core",code,"Provider.select()","Trace.selected",1,1),
               ("duplicate-factory","Core",code,"Provider.select()","Provider.select().also { Provider.select() }",1,1),
               ("wrong-factory-snapshot","Provider",factory,"return Trace.selected;","return Trace.replacement;",1,1),
               ("late-factory-counter","Provider",factory,"Trace.factoryCalls++;Trace.step(4);","Trace.step(4);Trace.factoryCalls++;",1,13),
               ("lost-partial","Core",code,"Trace.partial=this","Trace.partial=null",0,0),
               ("lost-legacy","Core",code,"Trace.legacy=this","Trace.legacy=null",1,1),
               ("late-legacy-store","Core",code,"legacyBlock=block\n        Trace.step(5)","Trace.step(5)\n        legacyBlock=block",1,14),
               ("wrong-key-observation","Trace",trace,"firstSeen=first;secondSeen=second;","firstSeen=second;secondSeen=first;",0,0),
               ("lost-array-identity","Trace",trace,"restSeen=rest;","restSeen=rest==null?null:rest.clone();",0,1),
               ("late-observations","Trace",trace,"firstSeen=first;secondSeen=second;restSeen=rest;handlerSeen=handler;\n        selected=replacement;step(2);","step(2);firstSeen=first;secondSeen=second;restSeen=rest;handlerSeen=handler;selected=replacement;",0,11),
               ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",0,10),
               ("lost-error","Trace",trace,"throw (Error)failure;","return;",1,20),
               ("unexpected-handler","Trace",trace,"selected=replacement;step(2);","if(handler!=null)handler.apply(null);selected=replacement;step(2);",0,1),
               ("unexpected-callback","Provider",factory,"return Trace.selected;","((kotlin.jvm.functions.Function2)Trace.selected).invoke(null,null);return Trace.selected;",1,1))
        for label,owner,body,old,new,kind,mode in wrong:
            if body.count(old)!=1:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+(".kt" if owner=="Core" else ".java"));source.parent.mkdir();source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            if owner=="Core":kotlin([source],compiled,"kotlinc-"+label)
            else:v.compile_local([source],compiled,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(kind),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned closed nominal overload" not in r.stderr:raise RuntimeError("Frozen observer missed wrong nominal overload program: "+label)
        classpath=["--classpath",args.android_jar.resolve(),"--classpath",declarations]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir()
        paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex"
        frozen_dex={str(dex):sha(dex)};(w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original nominal overload definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        annotations=v.run([v.dexdump,"-a",dex],"sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations)!=["Lkotlin/Metadata;"]:raise RuntimeError("D8 annotation retention changed")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]};release=coverage.get("minimum_java_release",8)
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or not 8<=release<=25:raise RuntimeError("Original nominal overload definitions or release changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):raise RuntimeError("Unnecessary nominal overload class or Signature projection")
        if not any("synthetic" in d for d in rows[entry].get("metadata_differences",[])):raise RuntimeError("Synthetic mask flag loss not reported")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)),release=release)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Nominal overload source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner])
            for identity,row in wanted["methods"].items():
                if identity in synthetic:row["access"]&=~0x1000
            for identity,row in [(owner,wanted),*wanted["methods"].items(),*wanted["fields"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;","Lkotlin/jvm/JvmField;"} for a in uses):raise RuntimeError("Unexpected genuine CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Nominal overload metadata changed beyond reported flag loss: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Nominal overload method metadata/Exceptions changed: "+identity)
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Nominal views, masks, identities, snapshots, partial fields or failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(f))!=h for f,h in input_hashes.items()) or any(sha(Path(f))!=h for f,h in {**dependency_hashes,**frozen_dex}.items()):raise RuntimeError("Frozen compiler/inputs/observer/declarations changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":240,"attempts_per_case":2,"wrong_programs":len(wrong),"java_release":release,"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
