#!/usr/bin/env python3
"""Verify closed receiver/scalar prefixes on genuine suspend interface bodies."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-suspend-forwarder-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"scoped_suspend_forwarder";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar"
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
            v.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(std,java,originals))),"-jvm-target","1.8","-Xjvm-default=all","-d",output,*sources],label)
        kotlin(sorted((fixture/"kotlin").rglob("*.kt")),originals,"kotlinc-original")
        for f in java.rglob("*.class"):
            target=originals/f.relative_to(java);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,target)
        names=("ScopedSuspendForwarderCaller","ScopedSuspendForwarderHarness","ScopedSuspendForwarderProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(3,9):raise RuntimeError("Original suspend forwarder definition inventory changed")
        excluded={"Lfixture/Context;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        scope=facts["Lfixture/Scope;"]
        if scope["signature"] is not None or len(scope["methods"])!=4:raise RuntimeError("Genuine closed interface/helper scope changed")
        synthetic={identity for identity,row in scope["methods"].items() if row["access"]&0x1000}
        if len(synthetic)!=2 or any(not scope["methods"][m]["access"]&8 for m in synthetic):raise RuntimeError("Missing genuine public static synthetic bodies")
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Context.class",(originals/"fixture/Context.class").read_bytes())
        runtime_deps=[std,*([provider] if excluded else [])]
        dependency_hashes={str(f):sha(f) for f in (declarations,provider)}
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for kind in range(2):
                    for mode in range(21):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(kind),str(mode)],f"{label}-{route}-{kind}-{mode}")
                        pattern=f"route={route},kind={kind},mode={mode},attempt=[01],events=[0-9]+,bits=[0-9a-f]{{1,16}}"
                        if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen suspend forwarder observer changed: "+text)
                        result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline")
        (w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        scope_source=(fixture/"kotlin/fixture/Scope.kt").read_text();trace=(fixture/"java/fixture/Trace.java").read_text()
        wrong=(("wrong-long","Scope",scope_source,"captureWindow(this, token(), tick)","captureWindow(this, token(), tick+1)",0,2),
               ("wrong-double","Scope",scope_source,"captureGauge(this, token(), tick)","captureGauge(this, token(), -tick)",1,2),
               ("missing-token","Scope",scope_source,"captureWindow(this, token(), tick)","captureWindow(this, null, tick)",0,1),
               ("duplicate-token","Scope",scope_source,"captureGauge(this, token(), tick)","captureGauge(this, token().also { token() }, tick)",1,1),
               ("missing-self","Scope",scope_source,"captureWindow(this, token(), tick)","captureWindow(null, token(), tick)",0,0),
               ("wrong-context","Trace",trace,"ownerSeen=owner;contextSeen=context;bits=value;","ownerSeen=owner;contextSeen=new Object();bits=value;",0,1),
               ("wrong-return","Scope",scope_source,"return block()","block(); return null as T",0,0),
               ("duplicate-callback","Scope",scope_source,"return block()","block(); return block()",0,0),
               ("late-observations","Trace",trace,"ownerSeen=owner;contextSeen=context;bits=value;step(2);step(3);","step(2);ownerSeen=owner;contextSeen=context;bits=value;step(3);",0,5),
               ("wrong-bits","Trace",trace,"bits=value;","bits=value+1;",0,0),
               ("canonical-nan","Trace",trace,"Double.doubleToRawLongBits(value)","Double.doubleToLongBits(value)",1,0),
               ("missing-precallback","Trace",trace,"bits=Double.doubleToRawLongBits(value);step(2);step(3);","bits=Double.doubleToRawLongBits(value);step(2);",1,0),
               ("lost-step","Trace",trace,"events=events*10+value;","events=value;",0,0),
               ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",0,4),
               ("lost-error","Trace",trace,"throw (Error)failure;","return;",0,10),
               ("lost-failure","Trace",trace,"if(fail==value)","if(false)",0,7))
        for label,owner,body,old,new,kind,mode in wrong:
            expected=2 if old=="return block()" else 1
            if body.count(old)!=expected:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+(".kt" if owner=="Scope" else ".java"));source.parent.mkdir();source.open("x").write(body.replace(old,new,1));compiled=w/(label+"-classes")
            if owner=="Scope":kotlin([source],compiled,"kotlinc-"+label)
            else:v.compile_local([source],compiled,classpath=str(originals),release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(kind),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned scoped suspend forwarder" not in r.stderr:raise RuntimeError("Frozen observer missed wrong suspend program: "+label)
        classpath=["--classpath",args.android_jar.resolve(),"--classpath",declarations]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir()
        paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex"
        frozen_dex={str(dex):sha(dex)}
        (w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original suspend definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        annotations=v.run([v.dexdump,"-a",dex],"sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations)!=["Lkotlin/Metadata;"]:raise RuntimeError("D8 annotation retention changed")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or coverage.get("minimum_java_release",8)!=9:raise RuntimeError("Original suspend definitions or release changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):raise RuntimeError("Unnecessary suspend class or Signature projection")
        if any(not any("synthetic" in d for d in rows[m].get("metadata_differences",[])) for m in synthetic):raise RuntimeError("Synthetic helper flag loss not reported")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)),release=9)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Suspend source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner])
            for identity,row in wanted["methods"].items():
                if identity in synthetic:row["access"]&=~0x1000
            for identity,row in [(owner,wanted),*wanted["methods"].items(),*wanted["fields"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;"} for a in uses):raise RuntimeError("Unexpected genuine CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Suspend metadata changed beyond reported flag loss: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Suspend method metadata/Exceptions changed: "+identity)
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Suspend identities, raw bits, order, partial effects or failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(f))!=h for f,h in input_hashes.items()) or any(sha(Path(f))!=h for f,h in {**dependency_hashes,**frozen_dex}.items()):raise RuntimeError("Frozen compiler/inputs/observer/declarations changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":168,"attempts_per_case":2,"wrong_programs":len(wrong),"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
