#!/usr/bin/env python3
"""Verify static callers retain their explicit cross-package protected receiver."""
from __future__ import annotations
import argparse,hashlib,json,os,subprocess,tempfile,zipfile
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
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true");args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-static-receivers-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"protected_static_receivers";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        android=args.android_jar.resolve();android_hash=sha(android);originals=w/"originals";harness=w/"harness";name="ProtectedStaticReceiversHarness"
        v.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(3,5):raise RuntimeError("Original static receiver definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded={"Lparent/Base;","Lapi/Bound;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for owner in ("parent/Base","api/Bound"):jar.writestr(owner+".class",(originals/(owner+".class")).read_bytes())
        provider_hash=sha(provider);deps=[provider] if excluded else []
        def execute(runtime,label):
            out=[]
            for route in range(2):
                for alias in range(3):
                    for mode in range(10):
                        text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(alias),str(mode)],f"{label}-{route}-{alias}-{mode}")
                        if text!="attempts=2,protected-static-receiver=matched\n":raise RuntimeError("Frozen static receiver observer changed")
                        out.append(text)
            return out
        baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        correct="return receiver.choose(value);"
        wrong=[("lost-call","return receiver.replacement;",0),
               ("new-receiver","return new Child().choose(value);",0),
               ("duplicate-call","receiver.choose(value);"+correct,0),
               ("early-store","receiver.seen=value;"+correct,4),
               ("changed-result","receiver.choose(value);return value;",0),
               ("new-argument-check","value.getClass();"+correct,3),
               ("lost-null-receiver","if(receiver==null)return null;"+correct,6),
               ("wrapped-error","try{"+correct+"}catch(RuntimeException error){throw new RuntimeException(error);}",4),
               ("swallowed-error","try{"+correct+"}catch(RuntimeException error){return null;}",5),
               ("result-class-from-input","return (api.Bound)value.getClass().cast(receiver.choose(value));",0)]
        for label,body,mode in wrong:
            file=w/label/"fixture/Child.java";file.parent.mkdir(parents=True);file.open("x").write("package fixture;import api.Bound;public class Child extends parent.Base{public static Bound apply(Child receiver,Bound value){"+body+"}public static Bound copied(Child receiver,Bound value){return apply(receiver,value);}}\n");compiled=w/(label+"-classes");v.compile_local([file],compiled,classpath=originals,release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,"0","0",str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned protected static receiver" not in r.stderr:raise RuntimeError("Observer missed wrong static receiver program: "+label)
        cp=["--classpath",android]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a static receiver definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8:raise RuntimeError("Static receiver coverage or full Signature changed")
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)) if deps else None,release=8)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt);helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])};allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Static receiver source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            actual=after[owner];actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            if actual!=facts[owner]:raise RuntimeError("Static receiver class, field or method metadata changed: "+owner)
            if any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Static receiver Exceptions changed")
        if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Static receiver identity, results, fields or partial failures changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(android)!=android_hash or sha(provider)!=provider_hash or sha(dex)!=frozen_dex:raise RuntimeError("Frozen static receiver inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":60,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
