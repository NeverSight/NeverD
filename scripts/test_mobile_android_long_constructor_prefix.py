#!/usr/bin/env python3
"""Verify typed long constructor preparation against unmodified Kotlin output."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-long-constructor-prefix-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"long_constructor_prefix";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar"
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(p):sha(p) for p in inputs}
        declarations=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declarations,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):dest.writestr(entry.filename,source.read(entry))
        java=w/"original-java";originals=w/"originals";harness=w/"harness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),java,release=8);originals.mkdir()
        def kotlin(sources,output,label):
            v.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(std,java,originals))),"-jvm-target","1.8","-d",output,*sources],label)
        kotlin(sorted((fixture/"kotlin").rglob("*.kt")),originals,"kotlinc-original")
        for f in java.rglob("*.class"):
            target=originals/f.relative_to(java);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,target)
        names=("LongConstructorPrefixCaller","LongConstructorPrefixHarness","LongConstructorPrefixProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(4,10):raise RuntimeError("Original long prefix definition inventory changed")
        excluded={"Lfixture/Shape;"} if args.dependency_provider else set();classes=all_classes-excluded
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Shape.class",(originals/"fixture/Shape.class").read_bytes())
        runtime_deps=[std,*([provider] if excluded else [])]
        dependency_hashes={str(f):sha(f) for f in (declarations,provider)}
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for mode in range(33):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],events=[0-9]+,packed=[0-9a-f]{{1,16}}"
                    if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen long prefix observer changed: "+text)
                    result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline")
        core=(fixture/"kotlin/fixture/Core.kt").read_text();trace=(fixture/"java/fixture/Trace.java").read_text()
        wrong=(("wrong-shift","Core",core,"shl 32","shl 31",2),
               ("lost-height-sign","Core",core,"0xffffffffL","0x7fffffffL",6),
               ("lost-width-sign","Core",core,"image.width().toLong()","(image.width().toLong() and 0x7fffffffL)",2),
               ("wrong-bitwise","Core",core,"shl 32) or (","shl 32) and (",2),
               ("missing-offset","Core",core,"Trace.offset()","0L",1),
               ("duplicate-width","Core",core,"image.width()","(image.width()+image.width())",2),
               ("wrong-offset-value","Trace",trace,"return offsetValue;","return offsetValue+1;",1),
               ("wrong-return","Trace",trace,"return result;","return value;",2),
               ("lost-partial","Core",core,"Trace.partial=this","Trace.partial=null",0),
               ("early-fields","Core",core,"Trace.step(5)","Trace.imageSeen=image;Trace.step(5)",16),
               ("wrong-size-observation","Core",core,"Trace.sizeSeen=size","Trace.sizeSeen=size+1",0),
               ("lost-step","Trace",trace,"events=events*10+value;","events=0;",0),
               ("duplicate-record","Trace",trace,"step(4);","step(4);step(4);",2),
               ("wrapped-failure","Trace",trace,"throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",16),
               ("lost-failure","Trace",trace,"if(fail==value)","if(false)",16),
               ("late-packed","Trace",trace,"packed=value;step(4);","step(4);packed=value;",22))
        for label,owner,body,old,new,mode in wrong:
            if body.count(old)!=1:raise RuntimeError("Frozen wrong-program anchor changed: "+label)
            source=w/label/(owner+(".kt" if owner=="Core" else ".java"));source.parent.mkdir();source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            if owner=="Core":kotlin([source],compiled,"kotlinc-"+label)
            else:v.compile_local([source],compiled,classpath=str(originals),release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned long constructor prefix" not in r.stderr:raise RuntimeError("Frozen observer missed wrong long prefix program: "+label)
        classpath=["--classpath",args.android_jar.resolve(),"--classpath",declarations]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir()
        paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex"
        if list(dexdir.glob("classes*.dex"))!=[dex]:raise RuntimeError("DEX count changed")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original long prefix definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        annotations=v.run([v.dexdump,"-a",dex],"sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations)!=["Lkotlin/Metadata;"]*2:raise RuntimeError("D8 annotation retention changed")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        entry="Lfixture/Core;-><init>(Lfixture/Shape;JJILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or coverage.get("minimum_java_release")!=25:raise RuntimeError("Original long prefix definitions or release changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):raise RuntimeError("Unnecessary long prefix class or Signature projection")
        if not any("synthetic" in d for d in rows[entry].get("metadata_differences",[])):raise RuntimeError("Synthetic default flag loss not reported")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)),release=25)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Long prefix source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner])
            for identity,row in wanted["methods"].items():
                if identity==entry:row["access"]&=~0x1000
            for identity,row in [(owner,wanted),*wanted["methods"].items(),*wanted["fields"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;","Lkotlin/jvm/JvmField;"} for a in uses):raise RuntimeError("Unexpected genuine CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if wanted.get(key)!=actual.get(key):raise RuntimeError("Long prefix metadata changed beyond reported flag loss: "+owner+":"+key)
            for identity,row in wanted["methods"].items():
                if actual["methods"].get(identity)!=row or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Long prefix method metadata/Exceptions changed: "+identity)
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        if execute((rebuilt,harness,*runtime_deps),"rebuilt")!=baseline:raise RuntimeError("Long prefix bits, order, snapshots or failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(f))!=h for f,h in input_hashes.items()) or any(sha(Path(f))!=h for f,h in dependency_hashes.items()):raise RuntimeError("Frozen compiler/inputs/observer/declarations changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":132,"attempts_per_case":2,"wrong_programs":16,"dependency_provider":args.dependency_provider}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
