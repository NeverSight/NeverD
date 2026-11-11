#!/usr/bin/env python3
"""Verify nested completion/result bindings and original checked failures."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib,json,os,re,subprocess,tempfile,zipfile
from pathlib import Path
try:
    from scripts.mobile_real_apps_android import dex_header,parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES,Verify,choose_d8,choose_jdk,class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header,parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES,Verify,choose_d8,choose_jdk,class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes,exception_inventory


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"): parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"): parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--initializer",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-completion-result-")
    with context as directory:
        work=Path(directory).resolve(); verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"nested_completion_result/java"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(name+".java") for name in
            ("NestedCompletionResultCaller","NestedCompletionResultHarness")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=8 or len(all_methods)!=17: raise RuntimeError("Owned nested completion result inventory changed")
        providers={"Lfixture/Provider;"}
        classes=all_classes-(providers if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Provider.class",(originals/"fixture/Provider.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider: classpath += ["--classpath",dependency]
        initialized=str(args.initializer).lower()
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(18):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"NestedCompletionResultHarness",
                        str(route),str(mode),initialized],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},initializer={initialized},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original nested completion result matrix changed")
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]: raise RuntimeError("Owned nested result DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original nested result entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-call-disassembly")
        prototype="(Lfixture/Context;Lfixture/Callback;)Ljava/lang/Object;"
        if len(re.findall(r"invoke-static[^\n]*Lfixture/Provider;.select:"+re.escape(prototype),disassembly))!=2:
            raise RuntimeError("Original ordinary/initializer generic calls changed")
        identity="Lfixture/Provider;->select"+prototype
        expected_signature="<T:Ljava/lang/Object;>(Lfixture/Context;Lfixture/Callback<-Lfixture/Scope;-Lfixture/Completion<-TT;>;+Ljava/lang/Object;>;)TT;"
        if records["Lfixture/Provider;"]["methods"][identity]["signature"]!=expected_signature or exceptions[identity]!=["Ljava/lang/InterruptedException;"]:
            raise RuntimeError("Original nested result Signature or checked exception changed")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or
            coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0) or
            coverage["status"]!="recovered" or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or
            coverage.get("minimum_java_release",8)!=8): raise RuntimeError("Nested completion result recovery changed original contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=8,
            classpath=dependency if args.dependency_provider else None)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Callback;"}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Nested completion result invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key): raise RuntimeError("Original nested result class metadata changed: "+c+":"+key)
            for m,meta in records[c]["methods"].items():
                if after[c]["methods"].get(m)!=meta or after_exceptions.get(m)!=exceptions[m]:
                    raise RuntimeError("Original nested result method metadata or Exceptions changed: "+m)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline: raise RuntimeError("Nested completion result values, effects or failure identity changed")
        mutations=(
            ("lost-completed-result","Provider","return completion.value;","return (T) returned;","1"),
            ("lost-direct-result","Provider","return (T) returned;","return completion.value;","0"),
            ("lost-callback","Provider","block.invoke(scope, completion)","Context.SUSPENDED","1"),
            ("changed-completion","Provider","block.invoke(scope, completion)","block.invoke(scope, new Completion<T>())","1"),
            ("lost-argument-order","Initializer" if args.initializer else "Client",
                "Provider.select(Trace.contextArg(Context.installed), Trace.callbackArg(Context.block))" if args.initializer else "Provider.select(Trace.contextArg(context), Trace.callbackArg(block))",
                "Provider.select(Context.installed, Context.block)" if args.initializer else "Provider.select(context, block)","0"),
            ("changed-interruption","Provider","throw context.interruption;","throw new InterruptedException(\"owned replacement\");","4"),
            ("lost-interruption","Provider","if (context.interruption != null) throw context.interruption;","","4"),
            ("changed-completion-failure","Provider","throw completion.failure;","throw new IllegalStateException(completion.failure);","3"),
            ("changed-result-reference","Initializer" if args.initializer else "Client",
                "Trace.result = value;" if args.initializer else "Trace.result = result;","Trace.result = new Object();","0"),
        )
        if args.initializer:
            mutations += (("changed-initializer-cause","Initializer","new ExceptionInInitializerError(cause)",
                "new ExceptionInInitializerError(new InterruptedException(\"owned replacement\"))","4"),)
        for label,owner,before,after,mode in mutations:
            text=(fixture/("fixture/"+owner+".java")).read_text()
            if text.count(before)!=1: raise RuntimeError("Owned nested result counterexample anchor changed: "+label)
            wrong=work/label/(owner+".java");wrong.parent.mkdir();wrong.open("x").write(text.replace(before,after,1))
            compiled=work/(label+"-classes");verify.compile_local([wrong],compiled,classpath=originals,release=8)
            r=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),"NestedCompletionResultHarness","1",mode,initialized],
                text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned nested completion result" not in r.stderr:
                raise RuntimeError("Independent observer missed nested result change: "+label)
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash:
            raise RuntimeError("Original nested result input, frozen observer or SDK bytes changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"dependency_provider":args.dependency_provider,
            "initializer":args.initializer,"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"independent_wrong_programs":len(mutations),
            "original_definitions_metadata_and_exceptions":"unchanged","callback_completion_and_result_identity":"matched",
            "ordered_arguments_partial_delivery_checked_failures_and_initialization_cache":"matched","input_and_frozen_oracle_bytes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__": raise SystemExit(main())
