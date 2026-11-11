#!/usr/bin/env python3
"""Verify independent key/value method scopes retain erased calls and effects."""
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
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true");args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-key-value-carrier-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"key_value_carrier";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(p):sha(p) for p in inputs}
        declarations=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declarations,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                    if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(entry.filename,source.read(entry))
        originals=w/"originals";harness=w/"harness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,classpath=std,release=8)
        compiler=[v.java,"-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-jvm-target","1.8","-Xno-param-assertions","-Xno-call-assertions"]
        v.run([*compiler,"-classpath",os.pathsep.join(map(str,(std,originals))),"-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        name="KeyValueCarrierHarness";v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(2,6):raise RuntimeError("Original key/value definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        excluded={"Lfixture/Values;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Values.class",(originals/"fixture/Values.class").read_bytes())
        deps=[std,*([provider] if excluded else [])];dependency_hashes={str(f):sha(f) for f in (declarations,provider)}
        def execute(runtime,label):
            out=[]
            for route in range(4):
                for mode in range(22):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
                    if text!="attempts=2,key-value-carrier=matched\n":raise RuntimeError("Frozen key/value observer changed")
                    out.append(text)
            return out
        baseline=execute((originals,harness,std),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        correct="return kotlin.collections.MapsKt.getValue(map,key);"
        wrong=[("lost-missing-key","return map.get(key);",4),
               ("lost-default-map","return map.get(key);",5),
               ("early-contains","map.containsKey(key);"+correct,0),
               ("duplicate-call","kotlin.collections.MapsKt.getValue(map,key);"+correct,0),
               ("new-key-string-effect","key.toString();"+correct,0),
               ("cast-value-as-key","return (V)key.getClass().cast(kotlin.collections.MapsKt.getValue(map,key));",0),
               ("wrapped-error","try{"+correct+"}catch(RuntimeException error){throw new RuntimeException(error);}",7),
               ("swallowed-error","try{"+correct+"}catch(RuntimeException error){return null;}",8)]
        def reject(compiled,label,route,mode):
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,std))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned key value carrier" not in r.stderr:raise RuntimeError("Observer missed wrong key/value program: "+label)
        for label,body,mode in wrong:
            file=w/label/"fixture/Values.java";file.parent.mkdir(parents=True);file.open("x").write("package fixture;import java.util.Map;public final class Values{public static <K,V> V pick(Map<K,? extends V> map,K key){"+body+"}}\n");compiled=w/(label+"-classes");v.compile_local([file],compiled,classpath=std,release=8);reject(compiled,label,0,mode)
        label="integer-cast-instead-of-number";file=w/label/"fixture/CoreKt.java";file.parent.mkdir(parents=True)
        file.open("x").write("package fixture;import java.util.Map;public final class CoreKt{public static final <K,V> V lookup(Map<K,? extends V> m,K k){return Values.pick(m,k);}public static final <K,V> V direct(Map<K,? extends V> m,K k){return kotlin.collections.MapsKt.getValue(m,k);}public static final int read(Map<Object,Integer> m,Object k){return ((Integer)Values.pick((Map)m,k)).intValue();}public static final int directRead(Map<Object,Integer> m,Object k){return kotlin.collections.MapsKt.getValue(m,k).intValue();}}\n")
        compiled=w/(label+"-classes");v.compile_local([file],compiled,classpath=os.pathsep.join(map(str,(originals,std))),release=8);reject(compiled,label,1,13)
        cp=["--classpath",args.android_jar.resolve(),"--classpath",declarations]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex={str(dex):sha(dex)};(w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a key/value definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("generic_signature_projections"):raise RuntimeError("Original key/value coverage/Signature changed")
        release=coverage.get("minimum_java_release",8);rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)),release=release)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt);mapped={m:m.split("->",1)[0]+"->"+r["source_name"]+m[m.index("("):] for m,r in rows.items() if r.get("source_name")};helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        default={"class":"Lfixture/CoreKt;","kind":"default-constructor","name":"<init>","prototype":"()V","source_unit":"fixture/CoreKt.java","static":False}
        if default not in coverage.get("generated_source_helpers",[]):raise RuntimeError("Missing exact reported top-level default constructor")
        allowed.add("Lfixture/CoreKt;-><init>()V")
        if actual_classes!=classes or set(actual_methods)!={mapped.get(m,m) for m in methods}|helpers or not helpers<=allowed:raise RuntimeError("Key/value source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={};debug={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);wanted["methods"]={mapped.get(m,m):r for m,r in wanted["methods"].items()}
            for identity,row in [(owner,wanted),*wanted["methods"].items(),*wanted["fields"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [ann for slot in value for ann in slot]
                    if uses:
                        if any(ann["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;"} for ann in uses):raise RuntimeError("Unexpected key/value CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            original_file="Core.kt" if owner=="Lfixture/CoreKt;" else "Values.java";source_file="CoreKt.java" if owner=="Lfixture/CoreKt;" else original_file
            if wanted.get("source_file")!=original_file or actual.get("source_file")!=source_file:raise RuntimeError("Unexpected key/value source filename")
            debug[owner]={"original":original_file,"rebuilt":source_file};wanted["source_file"]=source_file
            if actual!=wanted:raise RuntimeError("Key/value metadata changed beyond exact reported differences: "+owner)
            if any(after_exceptions[mapped.get(m,m)]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Key/value Exceptions changed")
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n");(w/"source-file-differences.json").open("x").write(json.dumps(debug,indent=2)+"\n")
        if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Key/value aliases, errors, effects or Number conversion changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(f))!=h for f,h in {**input_hashes,**dependency_hashes,**frozen_dex}.items()):raise RuntimeError("Frozen key/value inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":88,"attempts_per_case":2,"wrong_programs":9,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":release,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
