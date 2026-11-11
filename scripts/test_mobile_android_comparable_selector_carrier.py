#!/usr/bin/env python3
"""Verify independent element and recursive Comparable selector bounds."""
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


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--boolean-result",action="store_true")
    parser.add_argument("--void-result",action="store_true")
    args=parser.parse_args()
    if args.boolean_result and args.void_result:parser.error("choose one original result profile")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-comparable-selector-carrier-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"comparable_selector_carrier"
        if args.boolean_result:
            authored=work/"boolean-fixture";shutil.copytree(fixture,authored);fixture=authored
            provider=fixture/"java/fixture/Provider.java";text=provider.read_text()
            if text.count("Box<T> choose(")!=1 or text.count("return box;")!=1:
                raise RuntimeError("Original comparable selector authoring anchors changed")
            provider.write_text(text.replace("Box<T> choose(","boolean choose(").replace("return box;","return box != null && result <= 0;"))
            for relative in ("java/fixture/Client.java","harness/ComparableSelectorCaller.java"):
                p=fixture/relative;text=p.read_text()
                if text.count("static Box call(")!=1:raise RuntimeError("Original selector caller descriptor anchor changed")
                p.write_text(text.replace("static Box call(","static boolean call("))
            p=fixture/"harness/ComparableSelectorHarness.java";text=p.read_text()
            replacements=(
                ("static Box call(","static boolean call("),
                ("return (Box)Client.class", "return (Boolean)Client.class"),
                ("return (Box)MethodHandles", "return (boolean)MethodHandles"),
                ("MethodType.methodType(Box.class,Box.class,Callback.class)","MethodType.methodType(boolean.class,Box.class,Callback.class)"),
                ("ParameterizedType output = (ParameterizedType)method.getGenericReturnType();","Type output = method.getGenericReturnType();"),
                ("input.getRawType() == Box.class && output.getRawType() == Box.class && input.getActualTypeArguments()[0].equals(vars[0]) && output.getActualTypeArguments()[0].equals(vars[0])",
                 "input.getRawType() == Box.class && output == boolean.class && input.getActualTypeArguments()[0].equals(vars[0])"),
                ("new Key(-17 - attempt)","new Key(mode == 0 && attempt == 1 ? 17 : -17 - attempt)"),
                ("Box result = null; Throwable caught = null;","boolean result = false; Throwable caught = null;"),
                ("caught == null && result == lateBox","caught == null && result == (lateBox != null && (!(suppliedKey instanceof Key) || ((Key)suppliedKey).value <= 0))"),
            )
            for before,after in replacements:
                if text.count(before)!=1:raise RuntimeError("Original boolean selector observer anchor changed")
                text=text.replace(before,after)
            p.write_text(text)
        if args.void_result:
            authored=work/"void-fixture";shutil.copytree(fixture,authored);fixture=authored
            provider=fixture/"java/fixture/Provider.java";text=provider.read_text()
            if text.count("Box<T> choose(")!=1 or text.count("return box;")!=1:
                raise RuntimeError("Original void selector provider anchors changed")
            provider.write_text(text.replace("Box<T> choose(","void choose(").replace("return box;","if (box != null) box.value = value;"))
            for relative in ("java/fixture/Client.java","harness/ComparableSelectorCaller.java"):
                p=fixture/relative;text=p.read_text()
                if text.count("static Box call(")!=1 or text.count("return ")!=1:
                    raise RuntimeError("Original void selector caller anchors changed")
                p.write_text(text.replace("static Box call(","static void call(").replace("return ",""))
            p=fixture/"harness/ComparableSelectorHarness.java";text=p.read_text()
            a=text.index("    static Box call(");b=text.index("    public static void main",a)
            text=text[:a]+'''    static void call(int route,Box seed,Callback selector) throws Throwable {
        if (route == 0) { Client.call(seed,selector); return; }
        if (route == 1) { ComparableSelectorCaller.call(seed,selector); return; }
        if (route == 2) try {
            Client.class.getMethod("call",Box.class,Callback.class).invoke(null,seed,selector); return;
        } catch (InvocationTargetException failure) { throw failure.getCause(); }
        MethodHandles.publicLookup().findStatic(Client.class,"call",
            MethodType.methodType(void.class,Box.class,Callback.class)).invokeExact(seed,selector);
    }
'''+text[b:]
            replacements=(
                ("ParameterizedType output = (ParameterizedType)method.getGenericReturnType();","Type output = method.getGenericReturnType();"),
                ("input.getRawType() == Box.class && output.getRawType() == Box.class && input.getActualTypeArguments()[0].equals(vars[0]) && output.getActualTypeArguments()[0].equals(vars[0])",
                 "input.getRawType() == Box.class && output == void.class && input.getActualTypeArguments()[0].equals(vars[0])"),
                ("Object lateInput = new Object(), sentinel = new Object();",
                 "Object lateInput = new Object(), sentinel = new Object();\n            Object suppliedResultValue = suppliedBox.value, lateResultValue = lateBox == null ? null : lateBox.value;"),
                ("Box result = null; Throwable caught = null;","Throwable caught = null;"),
                ("try { result = call(route,seed,callback); }","try { call(route,seed,callback); }"),
                ("else require(caught == null && result == lateBox);","else require(caught == null);\n            require(suppliedBox.value == suppliedResultValue);\n            if (lateBox != null) require(lateBox.value == (caught == null ? suppliedInput : lateResultValue));"),
            )
            for before,after in replacements:
                if text.count(before)!=1:raise RuntimeError("Original void selector observer anchor changed")
                text=text.replace(before,after)
            p.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        verify.compile_local(sorted((fixture/"harness").glob("*.java")),harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=6 or len(all_methods)!=11:raise RuntimeError("Original comparable selector inventory changed")
        providers={"Lfixture/Provider;","Lfixture/Facade;"}
        classes=all_classes-(providers if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        target="Lfixture/Provider;->choose(Lfixture/Box;Lfixture/Callback;)Lfixture/Box;"
        signature="<T:Ljava/lang/Object;K::Ljava/lang/Comparable<-TK;>;>(Lfixture/Box<TT;>;Lfixture/Callback<-TT;+TK;>;)Lfixture/Box<TT;>;"
        if args.boolean_result:
            target=target.replace(")Lfixture/Box;",")Z")
            signature=signature.replace(")Lfixture/Box<TT;>;",")Z")
        if args.void_result:
            target=target.replace(")Lfixture/Box;",")V")
            signature=signature.replace(")Lfixture/Box<TT;>;",")V")
        if records["Lfixture/Provider;"]["methods"][target]["signature"]!=signature:
            raise RuntimeError("Original element/selector/recursive key relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Provider","Facade"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(17):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"ComparableSelectorHarness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original comparable selector oracle changed")
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original comparable selector DEX count changed")
        dex_hash=hashlib.sha256(source_dex.read_bytes()).hexdigest()
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original comparable selector entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-selector-call")
        call_pattern=r"invoke-static[^\n]*Lfixture/Facade;.choose:\(Lfixture/Box;Lfixture/Callback;\)"+("Z" if args.boolean_result else "V" if args.void_result else "Lfixture/Box;")
        if len(re.findall(call_pattern,disassembly))!=1:
            raise RuntimeError("Original inherited selector call was lost from the DEX")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or
            coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or
            coverage.get("projected_method_count",0) or coverage["status"]!="recovered" or
            coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8):
            raise RuntimeError("Comparable selector recovery changed original contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=8,
            classpath=dependency if args.dependency_provider else None)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Callback;"}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Comparable selector invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original comparable selector class metadata changed: "+c+":"+key)
            for m,meta in records[c]["methods"].items():
                if after[c]["methods"].get(m)!=meta or after_exceptions.get(m)!=exceptions[m]:
                    raise RuntimeError("Original comparable selector method metadata or Exceptions changed: "+m)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Comparable selector effects, snapshots, actual carriers or failures changed")
        signature_mutations=[]
        if args.boolean_result or args.void_result:
            original_signature=signature.encode()
            signature_mutations=[
                ("covariant-recursive-bound",original_signature.replace(b"Comparable<-TK;>",b"Comparable<+TK;>")),
                ("wrong-recursive-formal",original_signature.replace(b"Comparable<-TK;>",b"Comparable<-TT;>")),
                ("reversed-selector-slots",original_signature.replace(b"Callback<-TT;+TK;>",b"Callback<-TK;+TT;>")),
                ("variant-input-carrier",original_signature.replace(b"Box<TT;>",b"Box<+TT;>")),
                ("extra-formal",original_signature.replace(b"K::",b"U:Ljava/lang/Object;K::")),
                ("changed-plain-result",original_signature.replace(b")V" if args.void_result else b")Z",b")I")),
            ]
            for label,changed in signature_mutations:
                negative=work/(label+"-originals");shutil.copytree(originals,negative)
                receipt=change_signature(negative/"fixture/Provider.class",original_signature,changed)
                (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
                negative_classpath=list(classpath)
                if args.dependency_provider:
                    jar_path=work/(label+"-provider.jar")
                    with zipfile.ZipFile(jar_path,"x") as jar:
                        for name in ("Provider","Facade"):
                            jar.writestr("fixture/"+name+".class",(negative/("fixture/"+name+".class")).read_bytes())
                    negative_classpath[-1]=jar_path
                negative_dex=work/(label+"-dex");negative_dex.mkdir()
                negative_paths=[negative/p.relative_to(originals) for p in paths]
                verify.run([*verify.d8,"--debug","--min-api","24",*negative_classpath,"--output",negative_dex,*negative_paths],label+"-d8")
                rejected=work/(label+"-rejected")
                run=subprocess.run([str(args.neverd.resolve()),"mobile",str(negative_dex/"classes.dex"),
                    *map(str,negative_classpath),"-o",str(rejected)],text=True,capture_output=True,timeout=600,env=verify.environment)
                (work/(label+"-recovery.log")).open("x").write(run.stdout+run.stderr)
                if run.returncode==0 or rejected.exists() or "generic" not in run.stderr.lower():
                    raise RuntimeError("Boolean selector accepted an unproved complete relation: "+label)
        mutations=(
            ("lost-input-snapshot","T value = seed.value; Trace.seenInput = value; Trace.hit(2);",
             "Trace.hit(2); T value = seed.value; Trace.seenInput = value;",0),
            ("lost-key","K key = selector.apply(value);","selector.apply(value); K key = null;",0),
            ("lost-comparison","key.compareTo(key)","0",0),
            ("wrong-comparison-argument","key.compareTo(key)","key.compareTo(null)",0),
            ("lost-result-snapshot","return box;","return (Box<T>)Trace.result;",0),
            ("inferred-input-result","return box;","return seed;",0),
        )
        if args.boolean_result:
            result="return box != null && result <= 0;"
            mutations=mutations[:4]+(
                ("lost-result-snapshot",result,"return Trace.result != null && result <= 0;",8),
                ("inferred-input-result",result,"return seed != null && result <= 0;",8),
                ("reversed-comparison-result",result,"return box != null && result >= 0;",0),
                ("inferred-true-result",result,"return true;",0),
            )
        if args.void_result:
            write="if (box != null) box.value = value;"
            mutations=mutations[:4]+(
                ("lost-result-snapshot",write,"if (Trace.result != null) ((Box<T>)Trace.result).value = value;",8),
                ("inferred-input-write",write,"seed.value = value;",0),
                ("omitted-original-write",write,"",0),
                ("changed-written-value",write,"if (box != null) box.value = null;",0),
            )
        original_source=(fixture/"java/fixture/Provider.java").read_text()
        for label,before,changed,mode in mutations:
            if original_source.count(before)!=1:raise RuntimeError("Owned comparable selector counterexample anchor changed")
            wrong=work/label/"Provider.java";wrong.parent.mkdir()
            wrong.open("x").write(original_source.replace(before,changed,1))
            compiled=work/(label+"-classes");verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),
                "ComparableSelectorHarness","1",str(mode)],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned comparable selector carrier" not in result.stderr:
                raise RuntimeError("Frozen observer missed comparable selector change: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or
            hashlib.sha256(source_dex.read_bytes()).hexdigest()!=dex_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash or
            hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash):raise RuntimeError("Original comparable selector input, SDK or frozen observer changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"dependency_provider":args.dependency_provider,
            "boolean_result":args.boolean_result,"void_result":args.void_result,"same_code_signature_mutations":len(signature_mutations),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"independent_wrong_programs":len(mutations),
            "recursive_key_bound_and_invariant_element_contracts":"unchanged","original_definitions_metadata_and_exceptions":"unchanged",
            "selector_comparison_cast_timing_snapshots_carrier_identity_and_exact_failures":"matched",
            "original_inputs_and_frozen_observers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
