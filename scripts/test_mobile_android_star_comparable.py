#!/usr/bin/env python3
"""Verify an exact Comparable<?> comparison preserves identity, nulls and static facade initialization."""
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
    if error.name!="scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"): parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"): parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-star-comparable-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"star_comparable/java"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in ("StarComparableCaller","StarComparableHarness")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=5 or len(all_methods)!=12: raise RuntimeError("Original star comparison inventory changed: "+repr((len(all_classes),len(all_methods))))
        classes=all_classes-({"Lfixture/ComparisonBase;","Lfixture/Comparisons;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        target="Lfixture/ComparisonBase;->compareValues(Ljava/lang/Comparable;Ljava/lang/Comparable;)I"
        signature="<T::Ljava/lang/Comparable<*>;>(TT;TT;)I"
        metadata=records["Lfixture/ComparisonBase;"]["methods"][target]
        if metadata["signature"]!=signature or metadata["access"]!=0x9: raise RuntimeError("Original star comparison signature or access changed")
        dependency=work/"owned-carrier.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("ComparisonBase","Comparisons"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider: classpath += ["--classpath",dependency]
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for mode in range(11):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"StarComparableHarness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in output.splitlines()):
                        raise RuntimeError("Original private anchor observer changed: "+output)
                    result.append(output)
            return result
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]: raise RuntimeError("Star comparison changed DEX inventory")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original star comparison runtime entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        code=verify.run([verify.dexdump,"-d",source_dex],"original-private-call")
        if "compareValues:" not in code or "invoke-static" not in code: raise RuntimeError("Missing original static comparison call witness")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=1 or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Star comparison changed original scope, contracts or inventory")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=coverage.get("minimum_java_release",8),classpath=dependency if args.dependency_provider else None)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or helpers!=expected_helpers: raise RuntimeError("Star comparison lost a runtime entry or helper accounting")
        actual_records,actual_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for owner in classes:
            before,after=records[owner],actual_records[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations"):
                if before.get(key)!=after.get(key): raise RuntimeError("Star comparison class metadata changed: "+owner+":"+key)
            for identity,row in before["methods"].items():
                if after["methods"].get(identity)!=row or actual_exceptions.get(identity)!=exceptions[identity]: raise RuntimeError("Private original method metadata/Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline: raise RuntimeError("Comparison receiver, null/identity shortcut or facade initialization changed")
        # Independently authored wrong programs keep all public runtime entries;
        # the immutable caller/observer must catch each semantic difference.
        provider=(fixture/"fixture/ComparisonBase.java").read_text()
        mutations={"lost-identity":("if(first==second)return 0;","if(false)return 0;",9),
                   "reversed-null":("if(first==null)return -1;","if(first==null)return 1;",7),
                   "swapped-receiver":("((Comparable)first).compareTo(second)","((Comparable)second).compareTo(first)",0)}
        for label,(old,new,mode) in mutations.items():
            if provider.count(old)!=1: raise RuntimeError("Missing exact owned provider counterexample anchor")
            authored=work/("wrong-"+label);authored.mkdir();inputs=[]
            for original in sorted(fixture.rglob("*.java")):
                dest=authored/original.relative_to(fixture);dest.parent.mkdir(parents=True,exist_ok=True)
                text=original.read_text();dest.write_text(text.replace(old,new,1) if original.name=="ComparisonBase.java" else text);inputs.append(dest)
            wrong=work/("wrong-"+label+"-classes");verify.compile_local(inputs,wrong,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,harness))),"StarComparableHarness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/("wrong-"+label+"-observer.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned star comparison" not in result.stderr: raise RuntimeError("Frozen caller accepted an altered star comparison: "+label)
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash:
            raise RuntimeError("Original inputs, declaration carrier or frozen callers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,"original_metadata_and_exceptions":"unchanged","star_bound_identity_null_order_and_static_facade":"matched","counterexamples":len(mutations),"immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0
if __name__=="__main__": raise SystemExit(main())
