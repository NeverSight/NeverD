#!/usr/bin/env python3
"""Verify a generic Nothing target and its raw void bridge independently."""
from __future__ import annotations
import argparse,copy,hashlib,json,os,re,subprocess,tempfile,zipfile
from contextlib import nullcontext
from pathlib import Path
try:
    from scripts.mobile_real_apps_android import dex_header,parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from scripts.test_mobile_android_interface_default_access import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name!="scripts":raise
    from mobile_real_apps_android import dex_header,parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
    from test_mobile_android_static_defaults import method_code


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-generic-terminal-void-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"generic_terminal_void";k=args.kotlin_lib.resolve();std=k/"kotlin-stdlib.jar";sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(k.glob("*.jar"))];input_hashes={str(f):sha(f) for f in inputs};declarations=w/"kotlin-declarations.jar"
        with zipfile.ZipFile(std) as source,zipfile.ZipFile(declarations,"x") as dest:
            for entry in source.infolist():
                if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                    if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe Kotlin declaration entry")
                    dest.writestr(entry.filename,source.read(entry))
        originals=w/"originals";harness=w/"harness"
        v.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",k/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",std,"-jvm-target","1.8","-Xno-param-assertions","-Xno-call-assertions","-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        names=("GenericTerminalVoidCaller","GenericTerminalVoidHarness","GenericTerminalVoidProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(3,7):raise RuntimeError("Genuine terminal definition inventory changed")
        excluded={"Lfixture/Base;","Lfixture/Key;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);typed="Lfixture/Core;->writeValue(Lfixture/Key;Ljava/lang/Object;)Ljava/lang/Void;";bridge=typed.replace(")Ljava/lang/Void;",")V")
        if facts["Lfixture/Core;"]["methods"][bridge]["access"]!=0x1041 or facts["Lfixture/Core;"]["methods"][bridge]["signature"] is not None:raise RuntimeError("Genuine raw bridge flags or Signature changed")
        if facts["Lfixture/Core;"]["methods"][typed]["signature"]!="<T:Ljava/lang/Object;>(Lfixture/Key<TT;>;TT;)Ljava/lang/Void;":raise RuntimeError("Genuine method-owned key/value relation changed")
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for name in ("Base","Key"):jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        runtime_deps=[std,*([provider] if excluded else [])];dependency_hashes={str(f):sha(f) for f in (declarations,provider)}
        def execute(runtime,label,target,projected):
            result=[]
            for route in range(4):
                for entry in range(2):
                    for mode in range(16):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(entry),str(mode),target,str(int(projected))],f"{label}-{route}-{entry}-{mode}")
                        pattern=f"route={route},entry={entry},mode={mode},attempt=[01],frames={2 if entry==0 else 1}"
                        if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen terminal observer changed")
                        result.append(text)
            return result
        baseline=execute((originals,harness,*runtime_deps),"baseline","writeValue",False);(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        terminal='throw new IllegalStateException("owned terminal message");'
        forward='typedBridgeSlot(key,value);'
        wrong=(("normal-return","return null;",forward,1,1),
               ("wrong-message",'throw new IllegalStateException("owned wrong message");',forward,1,1),
               ("wrong-type",'throw new IllegalArgumentException("owned terminal message");',forward,1,1),
               ("extra-cause",'throw new IllegalStateException("owned terminal message",new RuntimeException("owned cause"));',forward,1,1),
               ("wrapped-target",'throw new RuntimeException(new IllegalStateException("owned terminal message"));',forward,1,1),
               ("extra-null-key-check","key.toString();"+terminal,forward,1,0),
               ("extra-value-string-cast","((String)value).length();"+terminal,forward,1,2),
               ("extra-value-key-cast","Key other=(Key)(Object)value;"+terminal,forward,1,2),
               ("extra-value-unbox","((Integer)value).intValue();"+terminal,forward,1,4),
               ("lost-forward",terminal,"return;",0,1),
               ("swallowed-target",terminal,'try{typedBridgeSlot(key,value);}catch(IllegalStateException error){return;}',0,1),
               ("wrapped-forward",terminal,'try{typedBridgeSlot(key,value);}catch(IllegalStateException error){throw new IllegalStateException("owned terminal message",error);}',0,1),
               ("fabricated-forward",terminal,terminal,0,1),
               ("wrong-self",terminal,'((Core)null).typedBridgeSlot(key,value);',0,1),
               ("new-forward-effect",terminal,'value.toString();'+forward,0,15),
               ("new-target-effect","value.hashCode();"+terminal,forward,1,15),
               ("cached-exception",'if(saved==null)saved=new IllegalStateException("owned terminal message");throw saved;',forward,1,1))
        for label,target_body,bridge_body,entry,mode in wrong:
            source=w/label/"Core.java";source.parent.mkdir();source.open("x").write('package fixture; public final class Core extends Base { public static final Core INSTANCE=new Core(); private static IllegalStateException saved; private Core(){} public <T> Void typedBridgeSlot(Key<T> key,T value){'+target_body+'} public void writeValue(Key key,Object value){'+bridge_body+'} }\n')
            compiled=w/(label+"-classes");v.compile_local([source],compiled,classpath=os.pathsep.join(map(str,(originals,std))),release=8)
            file=compiled/"fixture/Core.class";data=file.read_bytes();old=b"\x01"+len(b"typedBridgeSlot").to_bytes(2,"big")+b"typedBridgeSlot";new=b"\x01"+len(b"writeValue").to_bytes(2,"big")+b"writeValue"
            if data.count(old)!=1:raise RuntimeError("Wrong terminal name anchor changed")
            offset=data.index(old);changed=data.replace(old,new)
            if changed[:offset]!=data[:offset] or changed[offset+len(new):]!=data[offset+len(old):]:raise RuntimeError("Name encoding changed another byte")
            file.write_bytes(changed)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],"0",str(entry),str(mode),"writeValue","1"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned generic terminal void" not in r.stderr:raise RuntimeError("Frozen observer missed wrong terminal program: "+label)
        data=(originals/"fixture/Core.class").read_bytes();signature=facts["Lfixture/Core;"]["methods"][typed]["signature"].encode();replacement=signature.replace(b"Key<TT;>",b"Key<Ljava/lang/Object;>");old=b"\x01"+len(signature).to_bytes(2,"big")+signature;new=b"\x01"+len(replacement).to_bytes(2,"big")+replacement
        if data.count(old)!=1:raise RuntimeError("Terminal Signature anchor changed")
        changed=data.replace(old,new)
        if method_code(data)!=method_code(changed):raise RuntimeError("Signature drift changed Code")
        file=w/"signature-drift/fixture/Core.class";file.parent.mkdir(parents=True);file.open("xb").write(changed)
        r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(file.parent.parent,originals,harness,*runtime_deps))),names[1],"0","1","1","writeValue","0"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/"signature-drift-counterexample.log").open("x").write(r.stdout+r.stderr)
        if r.returncode==0 or "owned generic terminal void" not in r.stderr:raise RuntimeError("Frozen observer missed terminal Signature drift")
        classpath=["--classpath",args.android_jar.resolve(),"--classpath",declarations]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex={str(dex):sha(dex)};(w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a terminal definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or coverage.get("minimum_java_release",8)!=8 or coverage.get("projected_method_count")!=2:raise RuntimeError("Original terminal definitions or release changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):raise RuntimeError("Unnecessary terminal class or Signature projection")
        target=rows[typed].get("source_name")
        if not target or target=="writeValue" or rows[typed].get("projection_kind")!="nonreturning-void-target-alias" or rows[bridge].get("projection_kind")!="nonreturning-void-forwarding-bridge" or rows[bridge].get("source_generic_signature"):raise RuntimeError("Missing exact raw forwarding/target alias receipt")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,runtime_deps)),release=8)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt);mapped={typed:typed.replace("->writeValue(","->"+target+"(")};helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!={mapped.get(m,m) for m in methods}|helpers or not helpers<=allowed:raise RuntimeError("Terminal source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt);dropped={};source_files={}
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);adjusted={}
            for identity,row in wanted["methods"].items():
                if identity==typed:row["name"]=target
                if identity==bridge:row["access"]&=~0x1040
                adjusted[mapped.get(identity,identity)]=row
            wanted["methods"]=adjusted
            for identity,row in [(owner,wanted),*wanted["methods"].items(),*wanted["fields"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    value=row.get(key,[]);uses=value if key=="runtime_invisible_annotations" else [a for slot in value for a in slot]
                    if uses:
                        if any(a["type"] not in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;"} for a in uses):raise RuntimeError("Unexpected terminal CLASS annotation")
                        dropped.setdefault(identity,{})[key]=value;row[key]=[]
            actual=copy.deepcopy(after[owner]);actual["methods"]={m:row for m,row in actual["methods"].items() if m not in helpers}
            source_file=owner.rsplit("/",1)[-1][:-1]+".java"
            if wanted.get("source_file")!="Core.kt" or actual.get("source_file")!=source_file:raise RuntimeError("Unexpected terminal debug source filename")
            source_files[owner]={"original":wanted["source_file"],"rebuilt":source_file};wanted["source_file"]=source_file
            if wanted!=actual:raise RuntimeError("Terminal metadata changed beyond named alias/flags: "+owner)
            if any(after_exceptions[mapped.get(m,m)]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Terminal Exceptions changed")
        (w/"d8-annotation-differences.json").open("x").write(json.dumps(dropped,indent=2)+"\n")
        (w/"source-file-differences.json").open("x").write(json.dumps(source_files,indent=2)+"\n")
        if execute((rebuilt,harness,*runtime_deps),"rebuilt",target,True)!=baseline:raise RuntimeError("Terminal identity/message/cause/frames or unused operands changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(sha(Path(p))!=h for p,h in input_hashes.items()) or any(sha(Path(p))!=h for p,h in {**dependency_hashes,**frozen_dex}.items()):raise RuntimeError("Frozen terminal inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":128,"attempts_per_case":2,"wrong_programs":len(wrong)+1,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
