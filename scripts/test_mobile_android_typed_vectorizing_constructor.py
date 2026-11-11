#!/usr/bin/env python3
"""Verify typed interface conversion retains exact constructor binding and ordered effects."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-contracts",action="store_true")
    parser.add_argument("--callback-input",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-typed-vectorizing-constructor-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"typed_vectorizing_constructor/java"
        harness_name="TypedCallbackConstructor" if args.callback_input else "TypedVectorizingConstructor"
        if args.callback_input:
            import shutil
            authored=work/"authored-fixture";shutil.copytree(fixture,authored);fixture=authored
            (fixture/"fixture/Callback.java").open("x").write("package fixture;\npublic interface Callback<I,O> { O apply(I input); }\n")
            link=fixture/"fixture/Link.java";text=link.read_text();at=text.rfind("}")
            text=text[:at]+" @SuppressWarnings(\"unchecked\") public Callback<T,V> get() { Trace.hit(6);return (Callback<T,V>)Trace.callback; } "+text[at:];link.write_text(text)
            trace=fixture/"fixture/Trace.java";text=trace.read_text().replace("public static Core partial;", "public static Core partial;\n    public static Object callback;");trace.write_text(text)
            core=fixture/"fixture/Core.java";text=core.read_text()
            text=text.replace("Core(Spec<T> spec,Link<T,V> converter,T first,V velocity)", "Core(Spec<T> spec,Link<T,V> converter,T first,T input)")
            text=text.replace("this(spec.vectorize(converter),converter,first,velocity);", "this(spec.vectorize(converter),converter,first,converter.get().apply(input));")
            core.write_text(text)
            client=fixture/"fixture/Client.java";text=client.read_text().replace("Object first,Bound velocity", "Object first,Object velocity");client.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(9 if args.callback_input else 8) or len(all_methods)!=(16 if args.callback_input else 14):raise RuntimeError("Original typed vectorizing constructor inventory changed")
        classes=all_classes-({"Lfixture/Spec;","Lfixture/View;","Lfixture/Vector;","Lfixture/Link;","Lfixture/Bound;"}|({"Lfixture/Callback;"} if args.callback_input else set()) if args.dependency_contracts else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Core;-><init>(Lfixture/Spec;Lfixture/Link;Ljava/lang/Object;Lfixture/Bound;)V"
        signature="(Lfixture/Spec<TT;>;Lfixture/Link<TT;TV;>;TT;TV;)V"
        if args.callback_input:
            factory="Lfixture/Core;-><init>(Lfixture/Spec;Lfixture/Link;Ljava/lang/Object;Ljava/lang/Object;)V"
            signature="(Lfixture/Spec<TT;>;Lfixture/Link<TT;TV;>;TT;TT;)V"
        if records["Lfixture/Core;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete paired bounded result relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in (["Spec","View","Vector","Link","Bound","Callback"] if args.callback_input else ["Spec","View","Vector","Link","Bound"]):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_contracts:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(17 if args.callback_input else 11):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9,]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original typed vectorizing constructor matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original typed vectorizing constructor DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original typed vectorizing constructor entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Typed vectorizing constructor recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_contracts else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Spec;", "Lfixture/View;", "Lfixture/Callback;"} }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Typed vectorizing constructor recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original typed vectorizing constructor class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original typed vectorizing constructor method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_contracts else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Typed vectorizing constructor recovery changed input identities, virtual dispatch, partial fields, wide values or failure order")
        mutations=(("wrong-converter","Core","spec.vectorize(converter)","spec.vectorize(null)",0),
                   ("lost-vector","Core","this.vector=vector;","this.vector=null;",0),
                   ("early-first","Core","Trace.partial=this;Trace.hit(2);","Trace.partial=this;this.first=first;Trace.hit(2);",7))
        if args.callback_input:
            mutations+=(("changed-callback-input","Core","converter.get().apply(input)","converter.get().apply(first)",0),)
        for label,name,before,after,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original bounded consumer counterexample anchor changed")
            if label=="early-first":text=text.replace("this.converter=converter;this.first=first;", "this.converter=converter;")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or ("owned typed callback constructor" if args.callback_input else "owned typed vectorizing constructor") not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect bounded consumer: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_contracts":args.dependency_contracts,"callback_input":args.callback_input,
            "declared_receiver_method_formal_and_constructor_result_relation":"matched","all_original_entries_and_metadata":"unchanged",
            "conversion_count_inputs_result_identity_partial_fields_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
