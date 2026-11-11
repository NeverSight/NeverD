#!/usr/bin/env python3
"""Verify complete protected overload families retain fixed generic contracts."""
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
    from scripts.test_mobile_android_protected_fixed_mangled import patch_name
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name!="scripts":raise
    from mobile_real_apps_android import dex_header,parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES,Verify,choose_jdk,choose_d8
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
    from test_mobile_android_protected_fixed_mangled import patch_name
    from test_mobile_android_static_defaults import method_code


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-base",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-overload-identifiers-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"protected_overload_identifiers/java";authored=w/"authored";originals=w/"originals";harness=w/"harness"
        sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();android=args.android_jar.resolve();android_hash=sha(android)
        v.compile_local(sorted(fixture.rglob("*.java")),authored,release=8)
        names=("ProtectedOverloadIdentifiersCaller","ProtectedOverloadIdentifiersHarness","ProtectedOverloadIdentifiersProbe")
        v.compile_local([FIXTURES/"harness"/(n+".java") for n in names],harness,classpath=str(authored),release=8)
        authored_hashes=hashes(authored);shutil.copytree(authored,originals)
        witness={f.relative_to(originals).as_posix():count for f in sorted(originals.rglob("*.class")) if (count:=patch_name(f))}
        if witness!={"api/Root.class":1,"impl/Left.class":1,"impl/Right.class":1}:raise RuntimeError("Protected overload name encoding changed")
        (w/"name-encoding-witness.json").open("x").write(json.dumps(witness,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(8,15):raise RuntimeError("Original protected overload definition inventory changed")
        excluded={"Lapi/Base;"} if args.dependency_base else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);before=compiler_facts(authored)
        family={m for m in all_methods if "->choose-Name(" in m};assert len(family)==5
        callback="(JFLapi/Callback;)V";token="(JFLapi/Token;)V";root_callback="Lapi/Root;->choose-Name"+callback;root_token="Lapi/Root;->choose-Name"+token
        for owner in facts:
            wanted=copy.deepcopy(before[owner]);renamed={}
            for identity,row in wanted["methods"].items():
                if "->chooseValue(" in identity:row["name"]="choose-Name";identity=identity.replace("->chooseValue(","->choose-Name(")
                renamed[identity]=row
            wanted["methods"]=renamed
            if wanted!=facts[owner]:raise RuntimeError("Name encoding changed another class fact")
        provider=w/"owned-base.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("api/Base.class",(originals/"api/Base.class").read_bytes())
        provider_hash=sha(provider);runtime_deps=[provider] if excluded else []
        def execute(runtime,label,spelling):
            result=[]
            for route in range(4):
                for kind in range(2):
                    for shape in range(2):
                        for mode in range(23):
                            text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),names[1],str(route),str(kind),str(shape),str(mode),spelling],f"{label}-{route}-{kind}-{shape}-{mode}")
                            pattern=f"route={route},kind={kind},shape={shape},mode={mode},attempt=[01],events=[0-9]+"
                            if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):raise RuntimeError("Frozen protected overload observer changed")
                            result.append(text)
            return result
        baseline=execute((authored,harness),"authored","chooseValue")
        if execute((originals,harness),"encoded","choose-Name")!=baseline:raise RuntimeError("Method name encoding changed dispatch")
        (w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        sources={name:(fixture/(name+".java")).read_text() for name in ("api/Root","api/Trace","impl/Left","impl/Right")}
        wrong=(("lost-owner","api/Trace","ownerSeen=owner;","ownerSeen=null;",0,0,1),
               ("lost-input","api/Trace","argSeen=arg;","argSeen=null;",0,1,1),
               ("wrong-wide","api/Trace","bits=value;","bits=value+1;",0,0,0),
               ("wrong-float","api/Trace","raw=scale;","raw=scale+1;",0,0,0),
               ("wrong-kind","api/Trace","kind=selected;","kind=0;",1,0,1),
               ("lost-events","api/Trace","events=events*10+value;","events=value;",0,0,0),
               ("wrapped-failure","api/Trace","throw (RuntimeException)failure;","throw new IllegalArgumentException((RuntimeException)failure);",1,1,4),
               ("lost-error","api/Trace","throw (Error)failure;","return;",1,0,11),
               ("lost-failure","api/Trace","if(fail==value)","if(false)",0,0,6),
               ("wrong-wrapper-input","api/Root","chooseValue(bits,scale,block)","chooseValue(bits+1,scale,block)",0,0,0),
               ("wrong-tagged-kind","api/Root","Float.floatToRawIntBits(scale),1)","Float.floatToRawIntBits(scale),2)",0,1,1),
               ("lost-callback","impl/Left","block.apply(Trace.context);","",0,0,0),
               ("duplicate-callback","impl/Right","block.apply(Trace.context);","block.apply(Trace.context);block.apply(Trace.context);",1,0,1),
               ("missing-super-step","impl/Right","Trace.step(7);super.chooseValue","super.chooseValue",1,1,0),
               ("lost-super-call","impl/Right","super.chooseValue(bits,scale,token);","",1,1,0),
               ("wrong-callback-input","impl/Left","block.apply(Trace.context)","block.apply(null)",0,0,1),
               ("late-partial-fields","api/Trace","ownerSeen=owner;argSeen=arg;bits=value;raw=scale;kind=selected;step(2);step(3);","step(2);ownerSeen=owner;argSeen=arg;bits=value;raw=scale;kind=selected;step(3);",1,0,5))
        for label,owner,old,new,kind,shape,mode in wrong:
            body=sources[owner]
            if body.count(old)!=1:raise RuntimeError("Wrong-program source anchor changed: "+label)
            source=w/label/(owner+".java");source.parent.mkdir(parents=True);source.open("x").write(body.replace(old,new));compiled=w/(label+"-classes")
            v.compile_local([source],compiled,classpath=str(authored),release=8)
            if patch_name(compiled/(owner+".class"))!=(0 if owner=="api/Trace" else 1):raise RuntimeError("Wrong-program encoding changed")
            route=3 if label=="wrong-wrapper-input" else 0
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*runtime_deps))),names[1],str(route),str(kind),str(shape),str(mode),"choose-Name"],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned protected overload identifiers" not in r.stderr:raise RuntimeError("Frozen observer missed wrong protected program: "+label)
        data=(originals/"api/Root.class").read_bytes();signature=facts["Lapi/Root;"]["methods"][root_callback]["signature"].encode();changed=signature.replace(b"Context",b"Token");old=b"\x01"+len(signature).to_bytes(2,"big")+signature;new=b"\x01"+len(changed).to_bytes(2,"big")+changed
        if data.count(old)!=1:raise RuntimeError("Protected Signature anchor changed")
        drift=data.replace(old,new);assert method_code(data)==method_code(drift)
        file=w/"signature-drift/api/Root.class";file.parent.mkdir(parents=True);file.open("xb").write(drift)
        r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(file.parent.parent,originals,harness,*runtime_deps))),names[1],"0","0","0","0","choose-Name"],text=True,capture_output=True,timeout=1200,env=v.environment);(w/"signature-drift-counterexample.log").open("x").write(r.stdout+r.stderr)
        if r.returncode==0 or "owned protected overload identifiers" not in r.stderr:raise RuntimeError("Frozen observer missed Signature drift")
        classpath=["--classpath",android]
        if excluded:classpath += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*classpath,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex={str(dex):sha(dex)};(w/"frozen-dex-hashes.json").open("x").write(json.dumps(frozen_dex,indent=2)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original protected entry")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or coverage.get("minimum_java_release",8)!=8 or coverage.get("projected_method_count")!=5:raise RuntimeError("Original protected definitions or release changed")
        if coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):raise RuntimeError("Unnecessary protected class or Signature projection")
        spelling=rows[root_callback].get("source_name")
        if not spelling or not re.fullmatch(r"__neverdVirtualMethod[0-9]+",spelling):raise RuntimeError("Missing protected source spelling")
        for identity in family:
            row=rows[identity];declaration=root_callback if identity.endswith(callback) else root_token
            if row.get("source_name")!=spelling or row.get("source_name_declaration")!=declaration or set(row.get("source_name_family",[]))!=family or row.get("projection_kind")!="virtual-method-identifier" or len(row.get("metadata_differences",[]))!=3:raise RuntimeError("Incomplete per-overload declaring receipt")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=str(provider) if excluded else None,release=8)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt);renamed={m:m.replace("->choose-Name(","->"+spelling+"(") for m in family};helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!={renamed.get(m,m) for m in methods}|helpers or not helpers<=allowed:raise RuntimeError("Protected recovery invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            wanted=copy.deepcopy(facts[owner]);wanted["methods"]={renamed.get(m,m):{**row,**({"name":spelling} if m in family else {})} for m,row in wanted["methods"].items()}
            actual=copy.deepcopy(after[owner]);actual["methods"]={m:row for m,row in actual["methods"].items() if m not in helpers}
            if wanted!=actual:raise RuntimeError("Protected metadata changed beyond the declared spelling: "+owner)
            if any(after_exceptions[renamed.get(m,m)]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Protected Exceptions changed")
        if execute((rebuilt,harness,*runtime_deps),"rebuilt",spelling)!=baseline:raise RuntimeError("Protected overload dispatch, raw words, partial effects or errors changed")
        if hashes(authored)!=authored_hashes or hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(android)!=android_hash or sha(provider)!=provider_hash or any(sha(Path(p))!=h for p,h in frozen_dex.items()):raise RuntimeError("Frozen original/observer/declarations changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":368,"attempts_per_case":2,"wrong_programs":len(wrong)+1,"dependency_base":args.dependency_base};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
