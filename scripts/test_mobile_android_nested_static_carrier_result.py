#!/usr/bin/env python3
"""Verify a static factory retains its nested private carrier and lexical formal."""
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


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):p.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):p.add_argument("--"+name,type=Path)
    p.add_argument("--dependency-provider",action="store_true");args=p.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-static-carrier-result-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200);fixture=FIXTURES/"nested_static_carrier_result";sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();sdk_hash=sha(args.android_jar)
        originals=w/"originals";harness=w/"harness";name="NestedStaticCarrierResultHarness";sources=sorted((fixture/"java").rglob("*.java"));v.compile_local(sources,originals,release=8);v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);(w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n");all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(4,10):raise RuntimeError("Original nested static carrier inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals);excluded={"Lfixture/Trace;"} if args.dependency_provider else set();classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes};provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
        deps=[provider] if excluded else [];provider_hash=sha(provider)
        original="Lfixture/Holder;->build()Ljava/util/concurrent/atomic/AtomicReference;";signature="<T:Ljava/lang/Object;>()Ljava/util/concurrent/atomic/AtomicReference<Lfixture/Holder$Slot<TT;>;>;"
        if facts["Lfixture/Holder;"]["methods"][original]["signature"]!=signature:raise RuntimeError("Original static method formal or private result carrier changed")
        def execute(runtime,label):
            out=[]
            for route in range(4):
                for mode in range(10):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode)],f"{label}-{route}-{mode}")
                    if text!="attempts=2,nested-static-carrier-result=matched\n":raise RuntimeError("Frozen nested static carrier observer changed")
                    out.append(text)
            return out
        baseline=execute((originals,harness),"baseline");(w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        subject=fixture/"java/fixture/Holder.java";text=subject.read_text();returned="        return value;";hit="        Trace.hit(2,value);"
        wrong=[("lost-before",text.replace("        Trace.hit(1,null);",""),0,0),
               ("lost-helper",text.replace("return same(value);","return value;"),0,0),
               ("changed-identity",text.replace(returned,"        return new AtomicReference<Slot<T>>(value==null?null:value.get());"),3,0),
               ("prefilled-result",text.replace("new AtomicReference<Slot<T>>(null)","new AtomicReference<Slot<T>>(new Slot<T>(null))"),0,0),
               ("reused-result",text.replace("public final class Holder<T> {","public final class Holder<T> {\n    private static final AtomicReference SINGLETON=new AtomicReference(null);").replace("new AtomicReference<Slot<T>>(null)","(AtomicReference) SINGLETON"),0,0),
               ("changed-transfer-input",text.replace("return same(value);","return same(null);"),0,0),
               ("swallowed-failure",text.replace(hit,"        try { Trace.hit(2,value); } catch(Throwable error) {}"),0,5),
               ("wrapped-failure",text.replace(hit,"        try { Trace.hit(2,value); } catch(Throwable error) { throw new IllegalStateException(error); }"),0,6),
               ("runtime-content-guess",text.replace(returned,"        if(value!=null && value.get()!=null) ((Slot<T>)(Object)value.get()).value();\n"+returned),3,2),
               ("new-input-check",text.replace(hit,"        value.hashCode();\n"+hit),3,1)]
        if text.count(returned)!=1 or text.count(hit)!=1 or len({changed for _,changed,_,_ in wrong})!=10:raise RuntimeError("Nested static carrier mutation anchors changed")
        for label,changed,route,mode in wrong:
            source=w/label/"fixture/Holder.java";source.parent.mkdir(parents=True);source.open("x").write(changed);compiled=w/(label+"-classes");v.compile_local([source],compiled,classpath=originals,release=8)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode)],text=True,capture_output=True,timeout=1200,env=v.environment);(w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned nested static carrier result" not in r.stderr:raise RuntimeError("Observer missed wrong nested static carrier: "+label)
        cp=["--classpath",args.android_jar.resolve()]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[f for f in sorted(originals.rglob("*.class")) if "L"+f.relative_to(originals).as_posix()[:-6]+";" in classes];v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original");dex=dexdir/"classes.dex";frozen_dex=sha(dex);(w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n");(w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a nested static carrier definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n");output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover");c=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in c["methods"]}
        if set(rows)!=methods or c["class_count"]!=len(classes) or c["method_count"]!=len(methods) or c["unrecovered_method_count"] or c.get("generic_signature_projections") or c.get("minimum_java_release",8)!=8 or c["status"]!="recovered":raise RuntimeError("Nested static carrier coverage, scope or Signature changed")
        if rows[original].get("generic_signature")!=signature:raise RuntimeError("Nested factory lost its exact original Signature")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in c.get("generated_source_helpers",[])};allowed={o+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,deps)),release=8);actual_classes,actual_methods=compiler_inventory(v,rebuilt)
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=allowed:raise RuntimeError("Nested static carrier source invented/omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            actual=copy.deepcopy(after[owner]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            if actual!=facts[owner] or any(after_exceptions[m]!=exceptions[m] for m in facts[owner]["methods"]):raise RuntimeError("Nested static carrier metadata or Exceptions changed: "+owner)
        if execute((rebuilt,harness,*deps),"rebuilt")!=baseline:raise RuntimeError("Nested static carrier null/default/identity/effects/throwables changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or sha(args.android_jar)!=sdk_hash or sha(provider)!=provider_hash:raise RuntimeError("Frozen nested static carrier inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":40,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"dependency_provider":args.dependency_provider};(w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
