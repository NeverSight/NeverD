#!/usr/bin/env python3
"""Verify seeded factory carriers preserve closed inputs and complete invocation effects."""
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
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    parser.add_argument("--nested-result",action="store_true")
    parser.add_argument("--instance-provider",action="store_true")
    args=parser.parse_args()
    if args.instance_provider and not args.nested_result:parser.error("--instance-provider requires --nested-result")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-seeded-closed-inputs-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"seeded_closed_inputs/java"
        harness_name="SeededClosedInputs"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.nested_result:
            authored=work/"authored-fixture";shutil.copytree(fixture,authored);fixture=authored
            provider=fixture/"fixture/api/Provider.java"
            text=provider.read_text().replace("Box<T> make(T seed, Box<T> input", "Pair<Box<T>,T> make(Object seed, Box<Label> input")
            text=text.replace("callback.apply(seed)","callback.apply((T)seed)")
            text=text.replace("return (Box<T>)Trace.box;","return (Pair<Box<T>,T>)Trace.box;")
            if args.instance_provider:
                text=text.replace("class Provider {","public class Provider {").replace("public static <T>","public <T>")
            provider.write_text(text)
            pair=fixture/"fixture/Pair.java"
            pair.open("x").write("package fixture;\npublic final class Pair<A,B> { public A first; public B second; }\n")
            trace=fixture/"fixture/Trace.java"
            trace.write_text(trace.read_text().replace("Box<?> box;","Pair<?,?> box;\n    public static boolean nullOwner;"))
            client=fixture/"fixture/Client.java"
            text=client.read_text().replace("Box<?> call", "Pair<?,?> call")
            if args.instance_provider:
                text=text.replace("import fixture.api.Facade;","import fixture.api.Provider;")
                text=text.replace("return Facade.make(","Provider provider=Trace.nullOwner?null:new Provider();\n        return provider.make(")
            client.write_text(text)
            adapted=work/"authored-harness";adapted.mkdir();changed=[]
            for original in harness_sources:
                text=original.read_text().replace("Box<?> call", "Pair<?,?> call").replace("return (Box<?>)", "return (Pair<?,?>)")
                text=text.replace("methodType(Box.class,Object.class", "methodType(Pair.class,Object.class")
                text=text.replace("Throwable failure=null;Box<?> result=null;", "Throwable failure=null;Pair<?,?> result=null;")
                if original.stem.endswith("Harness"):
                    text=text.replace("Trace.box=mode==18?null:mode==27?inputBox:output;",
                        "Pair<Box<?>,Object> pair=new Pair<Box<?>,Object>();pair.first=mode==27?inputBox:output;pair.second=seed;\n"
                        "        Trace.box=mode==18?null:pair;Trace.nullOwner=mode==28;")
                    text=text.replace("result.value==(mode==27?inputBox.value:output.value)",
                        "result.first==pair.first&&result.second==seed&&((Box<?>)result.first).value==(mode==27?inputBox.value:output.value)")
                    if args.instance_provider:
                        text=text.replace("boolean entered=mode!=1;", "boolean entered=mode!=1&&mode!=28;")
                        text=text.replace('String expected=attempt==0?"1":"";', 'String expected=attempt==0&&mode!=28?"1":"";')
                        text=text.replace("else if(mode>=9&&mode<=11)", "else if(mode==28||mode>=9&&mode<=11)")
                output=adapted/original.name;output.write_text(text);changed.append(output)
            harness_sources=changed
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(10 if args.nested_result else 9) or len(all_methods)!=(16 if args.nested_result else 15):raise RuntimeError("Original seeded closed-input inventory changed")
        declarations={"Lfixture/api/Provider;","Lfixture/api/Middle;","Lfixture/api/Facade;"}
        classes=all_classes-(declarations if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        identity="Lfixture/api/Provider;->make(Ljava/lang/Object;Lfixture/Box;Lfixture/Fn;Lfixture/Fn;Lfixture/Producer;)Lfixture/Box;"
        expected="<T:Ljava/lang/Object;>(TT;Lfixture/Box<TT;>;Lfixture/Fn<-Lfixture/Label;Lfixture/Label;>;Lfixture/Fn<-TT;Lfixture/Label;>;Lfixture/Producer<Lfixture/Label;>;)Lfixture/Box<TT;>;"
        if args.nested_result:
            identity=identity.replace(")Lfixture/Box;",")Lfixture/Pair;")
            expected=expected.replace("(TT;Lfixture/Box<TT;>;","(Ljava/lang/Object;Lfixture/Box<Lfixture/Label;>;")
            expected=expected.replace(")Lfixture/Box<TT;>;",")Lfixture/Pair<Lfixture/Box<TT;>;TT;>;")
        if records["Lfixture/api/Provider;"]["methods"][identity]["signature"]!=expected:
            raise RuntimeError("Original complete seed, closed inputs and result carrier changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("api/Provider","api/Middle","api/Facade"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(29 if args.instance_provider else 28):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original seeded closed-input callback matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original seeded closed-input DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original seeded closed-input entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-argument-disassembly")
        reference="Lfixture/api/Provider;.make:" if args.instance_provider else "Lfixture/api/Facade;.make:"
        if reference not in disassembly:
            raise RuntimeError("D8 lost the inherited static facade witness")
        if args.nested_result:
            calls=[op for op,tail in re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",disassembly) if reference in tail]
            if len(calls)!=1 or not calls[0].startswith("invoke-virtual" if args.instance_provider else "invoke-static"):
                raise RuntimeError("D8 changed the original nested-result invocation style")
            (work/"nested-result-witness.json").open("x").write(json.dumps({"identity":identity,"signature":expected,"original_code":"unchanged","invocation":calls[0],"direct_formal_seeds":0},indent=2)+"\n")
        mutations=(("wrong-seed","callback.apply(seed)","callback.apply(null)",0),
                   ("wrong-closed-argument","closed.apply(produced)","closed.apply(null)",0),
                   ("duplicate-producer","Label produced=producer.get();","producer.get(); Label produced=producer.get();",0),
                   ("lost-mixed-callback","callback.apply(seed)","first",0),
                   ("wrong-result","return (Box<T>)Trace.box;","return input;",0),
                   ("late-produced-field","Trace.produced=produced; Trace.hit(4);","Trace.hit(4); Trace.produced=produced;",4),
                   ("early-mixed-effect","Label first=closed.apply(produced); Trace.first=first; Trace.hit(6);","Trace.hit(6); Label first=closed.apply(produced); Trace.first=first;",0))
        if args.nested_result:
            mutations=tuple((label,before.replace("callback.apply(seed)","callback.apply((T)seed)").replace("return (Box<T>)Trace.box;","return (Pair<Box<T>,T>)Trace.box;"),
                             "return null;" if label=="wrong-result" else after,mode) for label,before,after,mode in mutations)
        for label,before,after,mode in mutations:
            text=(fixture/"fixture/api/Provider.java").read_text()
            if text.count(before)!=1:raise RuntimeError("Original seeded closed-input counterexample anchor changed")
            changed=work/label/"Provider.java";changed.parent.mkdir();changed.open("x").write(text.replace(before,after))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned seeded closed-input observer" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect seeded closed inputs: "+label)
        evidence={"status":"baseline-only" if args.baseline_only else "success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "nested_result":args.nested_result,"instance_provider":args.instance_provider,
            "independent_wrong_programs":len(mutations)}
        if not args.baseline_only:
            output=work/"recovered"
            verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
            coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                    or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                    or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                    or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
                raise RuntimeError("Seeded closed-input recovery changed original entries or metadata contracts")
            rebuilt=work/"rebuilt"
            verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
            actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
            helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
            expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Fn;","Lfixture/Producer;"}}
            if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
                raise RuntimeError("Seeded closed-input recovery invented or omitted a runtime entry")
            after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
            for c in classes:
                for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                    if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original seeded closed-input class metadata changed: "+c+":"+key)
                for identity,metadata in records[c]["methods"].items():
                    if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                        raise RuntimeError("Original seeded closed-input method metadata or Exceptions changed: "+identity)
            runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
            if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Seeded closed-input recovery changed callback order or result/failure identity")
            evidence.update({"all_original_entries_and_metadata":"unchanged","complete_seed_closed_inputs_and_carrier_result":"matched",
                "inherited_facade_producer_two_callbacks_nulls_partial_fields_and_late_casts":"matched"})
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence["original_inputs_and_immutable_callers"]="unchanged"
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
