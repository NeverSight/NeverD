#!/usr/bin/env python3
"""Verify opposite callback slots and a shared fixed nominal result bound."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def nested_contextual_sources(fixture: Path, harness: list[Path]) -> None:
    """Author a paired List payload contract without changing the raw call ABI."""
    def change(path, before, after):
        text=path.read_text()
        if before not in text:raise RuntimeError("Nested bidirectional source anchor changed: "+path.name)
        path.write_text(text.replace(before,after))
    provider=fixture/"fixture/Provider.java"
    change(provider,"Carrier<T,V>","Carrier<T,Object>")
    change(provider,"? super T,? extends V>","? super T,? extends java.util.List<? extends V>>")
    change(provider,"? super V,? extends T>","? super java.util.List<? extends V>,? extends T>")
    trace=fixture/"fixture/Trace.java"
    change(trace,"Object middle=forward.apply(context,input);",
           "java.util.List middle=(java.util.List)forward.apply(context,input);")
    caller=next(p for p in harness if p.stem=="BidirectionalFactoryCaller")
    change(caller,"ContextCallback<Context,Object,Object> first,Callback<Object,Object> second",
           "ContextCallback<Context,Object,java.util.List<? extends Object>> first,Callback<java.util.List<? extends Object>,Object> second")
    observer=next(p for p in harness if p.stem=="BidirectionalFactoryHarness")
    change(observer,"result[1].equals(formals[1])","result[1]==Object.class")
    change(observer,"require(((WildcardType)slots[offset]).getLowerBounds()[0].equals(formals[direction]));\n            require(((WildcardType)slots[offset+1]).getUpperBounds()[0].equals(formals[1-direction]));",
           "Type input=((WildcardType)slots[offset]).getLowerBounds()[0];\n"
           "            Type output=((WildcardType)slots[offset+1]).getUpperBounds()[0];\n"
           "            require((direction==0?input:output).equals(formals[0]));\n"
           "            ParameterizedType nested=(ParameterizedType)(direction==0?output:input);\n"
           "            require(nested.getRawType()==java.util.List.class);\n"
           "            Type[] values=nested.getActualTypeArguments();\n"
           "            require(values.length==1 && ((WildcardType)values[0]).getUpperBounds()[0].equals(formals[1]));")
    change(observer,"Object middle=mode==5?null:new Object();",
           "java.util.List<Object> middle=mode==5?null:new java.util.ArrayList<Object>();")
    change(observer,"ContextCallback<Context,Object,Object> forward=",
           "ContextCallback<Context,Object,java.util.List<? extends Object>> forward=")
    change(observer,"Callback<Object,Object> reverse=",
           "Callback<java.util.List<? extends Object>,Object> reverse=")
    change(observer,"if(mode==4)throw original;input[0]++;return returned;",
           "if(mode==4)throw original;input[0]++;if(middle!=null)middle.add(returned);return returned;")
    change(observer,"ContextCallback first=mode==1?null:forward;Callback second=mode==2?null:reverse;",
           "ContextCallback bad=(context,value)->{require(context==scope && value==argument);counts[0]++;Trace.hit(7);input[0]++;return new Object();};\n"
           "        ContextCallback first=mode==1?null:mode==17?bad:forward;Callback second=mode==2?null:reverse;")
    change(observer,"Object expectedResult=mode==6?null:mode==7?alternate:existing;",
           "Object expectedResult=mode==6?null:mode==7?alternate:mode==18?new Object():existing;")
    change(observer,"mode==4?8:traceFailure;","mode==4?8:mode==17?7:traceFailure;")
    change(observer,"int before=input[0],left=counts[0],right=counts[1];",
           "int before=input[0],left=counts[0],right=counts[1],size=middle==null?0:middle.size();")
    change(observer,"Trace.intermediate==(leftFinished?middle:null)",
           "Trace.intermediate==(leftFinished && mode!=17?middle:null)")
    change(observer,"if(mode==1||mode==2)require(failure instanceof NullPointerException);",
           "if(middle!=null)require(middle.size()==size+(rightFinished?1:0) && (!rightFinished || middle.get(size)==returned));\n"
           "            if(mode==17||mode==18)require(failure instanceof ClassCastException && actual==null);\n"
           "            else if(mode==1||mode==2)require(failure instanceof NullPointerException);")


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--contextual",action="store_true")
    parser.add_argument("--nested-contextual",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    args=parser.parse_args()
    if args.nested_contextual:args.contextual=True
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bidirectional-factory-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"bidirectional_factory/java"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("BidirectionalFactoryCaller","BidirectionalFactoryHarness")]
        if args.contextual:
            copied=work/"fixture-source";shutil.copytree(fixture,copied);fixture=copied
            def change(path,before,after):
                text=path.read_text()
                if before not in text:raise RuntimeError("Contextual bidirectional source anchor changed")
                path.write_text(text.replace(before,after))
            (fixture/"fixture/Context.java").open("x").write("package fixture; public interface Context {}\n")
            (fixture/"fixture/ContextCallback.java").open("x").write("package fixture; public interface ContextCallback<C,A,B> { B apply(C context,A value); }\n")
            for name in ("Carrier","Provider"):change(fixture/("fixture/"+name+".java"),"V extends Bound","V")
            change(fixture/"fixture/Provider.java","Callback<? super T,? extends V> forward","ContextCallback<? super Context,? super T,? extends V> forward")
            change(fixture/"fixture/Client.java","Callback forward,Callback reverse","ContextCallback forward,Callback reverse")
            trace=fixture/"fixture/Trace.java"
            change(trace,"public static long events;","public static Context context; public static long events;")
            change(trace,"Callback forward, Callback reverse","ContextCallback forward, Callback reverse")
            change(trace,"Bound middle=(Bound)forward.apply(input);","Object middle=forward.apply(context,input);")
            copied_harness=work/"harness-source";copied_harness.mkdir()
            for original in harness_sources:shutil.copyfile(original,copied_harness/original.name)
            caller=copied_harness/"BidirectionalFactoryCaller.java"
            change(caller,"Carrier<Object,Bound>","Carrier<Object,Object>")
            change(caller,"Callback<Object,Bound> first,Callback<Bound,Object> second","ContextCallback<Context,Object,Object> first,Callback<Object,Object> second")
            change(caller,"<Object,Bound>create","<Object,Object>create")
            observer=copied_harness/"BidirectionalFactoryHarness.java"
            change(observer,"Callback first,Callback second","ContextCallback first,Callback second")
            change(observer,"Callback.class,Callback.class","ContextCallback.class,Callback.class")
            change(observer,"formals[1].getBounds()[0]==Bound.class","formals[1].getBounds()[0]==Object.class")
            change(observer,"require(parameter.getRawType()==Callback.class && slots.length==2);", "int offset=direction==0?1:0;\n            require(parameter.getRawType()==(direction==0?ContextCallback.class:Callback.class) && slots.length==2+offset);\n            if(offset==1)require(((WildcardType)slots[0]).getLowerBounds()[0]==Context.class);")
            change(observer,"slots[0]).getLowerBounds()[0].equals(formals[direction])","slots[offset]).getLowerBounds()[0].equals(formals[direction])")
            change(observer,"slots[1]).getUpperBounds()[0].equals(formals[1-direction])","slots[offset+1]).getUpperBounds()[0].equals(formals[1-direction])")
            change(observer,"Bound middle=mode==5?null:new Bound();", "Object middle=mode==5?null:new Object();\n        Context scope=mode==16?null:new Context(){};")
            change(observer,"Callback<Object,Bound> forward=value->{require(value==argument);", "ContextCallback<Context,Object,Object> forward=(context,value)->{require(context==scope && value==argument);")
            change(observer,"Callback<Bound,Object> reverse", "Callback<Object,Object> reverse")
            change(observer,"Callback first=mode==1?null:forward,second=mode==2?null:reverse;", "ContextCallback first=mode==1?null:forward;Callback second=mode==2?null:reverse;")
            change(observer,"new Carrier<Object,Bound>", "new Carrier<Object,Object>")
            change(observer,"Trace.input=argument;", "Trace.input=argument;Trace.context=scope;")
            harness_sources=sorted(copied_harness.glob("*.java"))
            if args.nested_contextual:nested_contextual_sources(fixture,harness_sources)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(8 if args.contextual else 6) or len(all_methods)!=(11 if args.contextual else 10):raise RuntimeError("Original bidirectional factory inventory changed")
        classes=all_classes-({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Provider;->create(Lfixture/Callback;Lfixture/Callback;)Lfixture/Carrier;"
        signature="<T:Ljava/lang/Object;V:Lfixture/Bound;>(Lfixture/Callback<-TT;+TV;>;Lfixture/Callback<-TV;+TT;>;)Lfixture/Carrier<TT;TV;>;"
        if args.contextual:
            factory="Lfixture/Provider;->create(Lfixture/ContextCallback;Lfixture/Callback;)Lfixture/Carrier;"
            signature="<T:Ljava/lang/Object;V:Ljava/lang/Object;>(Lfixture/ContextCallback<-Lfixture/Context;-TT;+TV;>;Lfixture/Callback<-TV;+TT;>;)Lfixture/Carrier<TT;TV;>;"
        if args.nested_contextual:
            signature="<T:Ljava/lang/Object;V:Ljava/lang/Object;>(Lfixture/ContextCallback<-Lfixture/Context;-TT;+Ljava/util/List<+TV;>;>;Lfixture/Callback<-Ljava/util/List<+TV;>;+TT;>;)Lfixture/Carrier<TT;Ljava/lang/Object;>;"
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original opposite callback and result bound relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Provider.class",(originals/"fixture/Provider.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(19 if args.nested_contextual else 17 if args.contextual else 16):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"BidirectionalFactoryHarness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original bidirectional factory matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original bidirectional factory DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original bidirectional factory entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        if not args.baseline_only:
            output=work/"recovered"
            verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
            coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                    or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                    or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                    or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
                raise RuntimeError("Bidirectional factory recovery changed original entries or metadata contracts")
            rebuilt=work/"rebuilt"
            verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
                classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
            actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
            helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
            expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Callback;","Lfixture/Context;","Lfixture/ContextCallback;"}}
            if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
                raise RuntimeError("Bidirectional factory recovery invented or omitted a runtime entry")
            after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
            for c in classes:
                for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                            "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                    if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original bidirectional factory class metadata changed: "+c+":"+key)
                for identity,metadata in records[c]["methods"].items():
                    if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                        raise RuntimeError("Original bidirectional factory method metadata or Exceptions changed: "+identity)
            runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
            if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Bidirectional factory recovery changed callback direction, partial fields, mutations, result identity or failures")
        mutations=(("swapped-callbacks","Trace.observe(forward,reverse);","Trace.observe(reverse,forward);",0),
                   ("replaced-result","return (Carrier<T,V>)Trace.result;","return new Carrier<T,V>(forward,reverse);",7),
                   ("early-effect","Trace.hit(1);","Trace.hit(6); Trace.hit(1);",1))
        if args.contextual:
            mutations=(("changed-context","Trace.observe(forward,reverse);","Trace.context=null;Trace.observe(forward,reverse);",0),
                       *mutations[1:],
                       ("skipped-observation","Trace.observe(forward,reverse);","Trace.hit(3);",0),
                       ("lost-forward-identity","Trace.forward=forward;","Trace.forward=null;",0))
        if args.nested_contextual:
            mutations=tuple((label,before.replace("Carrier<T,V>","Carrier<T,Object>"),
                             after.replace("Carrier<T,V>","Carrier<T,Object>"),mode)
                            for label,before,after,mode in mutations)
        original=(fixture/"fixture/Provider.java").read_text()
        for label,before,after,mode in mutations:
            if original.count(before)!=1:raise RuntimeError("Original bidirectional counterexample anchor changed")
            changed=work/label/"Provider.java";changed.parent.mkdir();changed.write_text(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                "BidirectionalFactoryHarness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned bidirectional factory" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect provider behavior: "+label)
        if args.nested_contextual:
            wrong=work/"signature-drift/fixture/Provider.class";wrong.parent.mkdir(parents=True)
            shutil.copy2(originals/"fixture/Provider.class",wrong)
            receipt=change_signature(wrong,signature.encode(),signature.replace("List<+TV;>","List<+Ljava/lang/Object;>",1).encode())
            (work/"signature-drift-encoding.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
            changed=work/"removed-nested-cast/Trace.java";changed.parent.mkdir()
            text=(fixture/"fixture/Trace.java").read_text()
            before="java.util.List middle=(java.util.List)forward.apply(context,input);"
            if text.count(before)!=1:raise RuntimeError("Missing independently observed nested cast")
            changed.open("x").write(text.replace(before,"Object middle=forward.apply(context,input);"))
            verify.compile_local([changed],work/"removed-nested-cast-classes",classpath=originals,release=8)
            for label,directory,mode in (("signature-drift",wrong.parents[1],0),
                                         ("removed-nested-cast",work/"removed-nested-cast-classes",17)):
                result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(directory,originals,harness))),
                    "BidirectionalFactoryHarness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
                (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
                if result.returncode==0 or "owned bidirectional factory" not in result.stderr:
                    raise RuntimeError("Frozen observer missed nested contract drift: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"baseline-only" if args.baseline_only else "success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "contextual":args.contextual,
            "nested_contextual":args.nested_contextual,
            "independent_wrong_programs":len(mutations)+(2 if args.nested_contextual else 0),"original_inputs_and_immutable_callers":"unchanged"}
        if not args.baseline_only:
            evidence.update({"opposite_callbacks_and_exact_result_formals":"matched","all_original_entries_and_metadata":"unchanged",
                "mutable_input_callback_identity_nulls_partial_writes_result_identity_and_failures":"matched"})
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
