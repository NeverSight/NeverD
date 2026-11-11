#!/usr/bin/env python3
"""Verify recursive comparable seeds and ranges retain the original result and effects."""
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
    parser.add_argument("--inherited-owner",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-recursive-range-result-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"recursive_range_result/java"
        harness_name="RecursiveRangeResult"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.inherited_owner:
            copied=work/"fixture-source";shutil.copytree(fixture,copied);fixture=copied
            provider=fixture/"fixture/Provider.java"
            provider.write_text(provider.read_text().replace("public final class Provider", "public class Provider"))
            (fixture/"fixture/Facade.java").open("x").write(
                "package fixture;\npublic final class Facade extends Provider { static { Trace.hit(8); } }\n")
            client=fixture/"fixture/Client.java"
            client.write_text(client.read_text().replace("Provider.clamp", "Facade.clamp"))
            copied_harness=work/"harness-source";copied_harness.mkdir()
            for original in harness_sources:
                (copied_harness/original.name).open("x").write(original.read_text().replace("Provider.clamp", "Facade.clamp").replace("Provider.class", "Facade.class"))
            harness_sources=sorted(copied_harness.glob("*.java"))
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(5 if args.inherited_owner else 4) or len(all_methods)!=(10 if args.inherited_owner else 8):raise RuntimeError("Original recursive range result inventory changed")
        declarations={"Lfixture/Provider;","Lfixture/Range;"}
        if args.inherited_owner:declarations.add("Lfixture/Facade;")
        classes=all_classes-(declarations if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        identity="Lfixture/Provider;->clamp(Ljava/lang/Comparable;Lfixture/Range;)Ljava/lang/Comparable;"
        expected="<T::Ljava/lang/Comparable<-TT;>;>(TT;Lfixture/Range<TT;>;)TT;"
        if records["Lfixture/Provider;"]["methods"][identity]["signature"]!=expected:
            raise RuntimeError("Original recursive range seed/result relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in (("Provider","Range","Facade") if args.inherited_owner else ("Provider","Range")):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(20):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original recursive range result matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original recursive range result DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original recursive range result entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        if args.inherited_owner:
            code=verify.run([verify.dexdump,"-d",source_dex],"inherited-call-instructions")
            if not re.search(r"invoke-static[^\n]*Lfixture/Facade;.clamp",code):
                raise RuntimeError("Compiler did not preserve the inherited static reference")
        mutations=(("reversed-comparison","seed.compareTo(low)","low.compareTo(seed)",1),
                   ("double-lower-read","T low=range.lower();","range.lower();T low=range.lower();",0),
                   ("early-upper-read","T low=range.lower();","range.upper();T low=range.lower();",1),
                   ("wrong-result","return second>0?high:seed;","return seed;",2),
                   ("lost-seed-observation","Trace.seenSeed=seed;","Trace.seenSeed=null;",0))
        for label,before,after,mode in mutations:
            text=(fixture/"fixture/Provider.java").read_text()
            if text.count(before)!=1:raise RuntimeError("Original recursive range result counterexample anchor changed")
            changed=work/label/"Provider.java";changed.parent.mkdir();changed.open("x").write(text.replace(before,after))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned recursive range result" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect recursive range result: "+label)
        if args.inherited_owner:
            changed=work/"eager-facade-initialization"/"Client.java";changed.parent.mkdir()
            changed.open("x").write((fixture/"fixture/Client.java").read_text().replace("return Facade.clamp", "new Facade();return Facade.clamp"))
            wrong=work/"eager-facade-initialization-classes"
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_name+"Harness","0","0"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/"eager-facade-initialization-counterexample.log").open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned recursive range result" not in result.stderr:
                raise RuntimeError("Independent observer missed eager facade initialization")
        evidence={"status":"baseline-only" if args.baseline_only else "success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "inherited_owner":args.inherited_owner,"independent_wrong_programs":len(mutations)+int(args.inherited_owner)}
        if not args.baseline_only:
            output=work/"recovered"
            verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
            coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                    or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                    or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                    or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
                raise RuntimeError("Recursive range result recovery changed original entries or metadata contracts")
            rebuilt=work/"rebuilt"
            verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
            actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
            helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
            expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c != "Lfixture/Range;"}
            if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
                raise RuntimeError("Recursive range result recovery invented or omitted a runtime entry")
            after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
            for c in classes:
                for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                    if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original recursive range result class metadata changed: "+c+":"+key)
                for identity,metadata in records[c]["methods"].items():
                    if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                        raise RuntimeError("Original recursive range result method metadata or Exceptions changed: "+identity)
            runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
            if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Recursive range result recovery changed evaluation order or result/failure identity")
            evidence.update({"all_original_entries_and_metadata":"unchanged","recursive_seed_range_and_result_formal":"matched",
                "ordered_range_reads_comparisons_nulls_partial_fields_and_late_casts":"matched"})
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence["original_inputs_and_immutable_callers"]="unchanged"
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
