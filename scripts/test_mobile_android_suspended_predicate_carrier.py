#!/usr/bin/env python3
"""Verify producer carriers with a callback consuming a closed completion tail."""
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
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-suspended-predicate-carrier-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"suspended_predicate_carrier"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        verify.compile_local(sorted((fixture/"harness").glob("*.java")),harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=7 or len(all_methods)!=12:raise RuntimeError("Original suspended carrier inventory changed")
        providers={"Lfixture/Provider;","Lfixture/Facade;"}
        classes=all_classes-(providers if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        target="Lfixture/Provider;->choose(Lfixture/Carrier;Lfixture/Callback;)Lfixture/Carrier;"
        signature="<T:Ljava/lang/Object;>(Lfixture/Carrier<+TT;>;Lfixture/Callback<-TT;-Lfixture/Completion<-Ljava/lang/Boolean;>;+Ljava/lang/Object;>;)Lfixture/Carrier<TT;>;"
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
            cases=[(variant,0,0) for variant in range(13)]
            cases += [(0,fail,kind) for fail in range(1,10) for kind in (0,1)]
            for route in range(4):
                for variant,fail,kind in cases:
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"SuspendedCarrierHarness",
                        str(route),str(variant),str(fail),str(kind)],f"{label}-{route}-{variant}-{fail}-{kind}")
                    pattern=f"route={route},variant={variant},fail={fail},kind={kind},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original suspended carrier oracle changed")
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original suspended carrier DEX count changed")
        dex_hash=hashlib.sha256(source_dex.read_bytes()).hexdigest()
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original suspended carrier entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-selector-call")
        if len(re.findall(r"invoke-static[^\n]*Lfixture/Facade;.choose:\(Lfixture/Carrier;Lfixture/Callback;\)Lfixture/Carrier;",disassembly))!=1:
            raise RuntimeError("Original inherited selector call was lost from the DEX")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or
            coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or
            coverage.get("projected_method_count",0) or coverage["status"]!="recovered" or
            coverage.get("class_source_bindings") or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release",8)!=8):
            raise RuntimeError("Suspended carrier recovery changed original contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=8,
            classpath=dependency if args.dependency_provider else None)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Carrier;","Lfixture/Callback;","Lfixture/Completion;"}}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Suspended carrier invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original suspended carrier class metadata changed: "+c+":"+key)
            for m,meta in records[c]["methods"].items():
                if after[c]["methods"].get(m)!=meta or after_exceptions.get(m)!=exceptions[m]:
                    raise RuntimeError("Original suspended carrier method metadata or Exceptions changed: "+m)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Suspended carrier effects, snapshots, actual carriers or failures changed")
        mutations=(
            ("lost-value-snapshot","T value = captured.value();","captured.value(); T value = null;"),
            ("lost-channel-snapshot","Object returned = callback.invoke(value, channel);","Object returned = callback.invoke(value, Trace.completion);"),
            ("lost-done-snapshot","channel.resume(done);","channel.resume(Trace.done);"),
            ("eager-boolean-result","Trace.seenReturn = returned;","((Boolean) returned).booleanValue(); Trace.seenReturn = returned;"),
            ("lost-result-snapshot","return result;","return (Carrier<T>) Trace.output;"),
            ("inferred-input-result","return result;","return (Carrier<T>) captured;"),
            ("extra-callback","Object returned = callback.invoke(value, channel);","callback.invoke(value, channel); Object returned = callback.invoke(value, channel);"),
            ("early-result","Trace.mask |= 8; Trace.hit(4);\n        Carrier<T> result = (Carrier<T>) Trace.output;",
             "Carrier<T> result = (Carrier<T>) Trace.output;\n        Trace.mask |= 8; Trace.hit(4);"),
        )
        original_source=(fixture/"java/fixture/Provider.java").read_text()
        for label,before,changed in mutations:
            if original_source.count(before)!=1:raise RuntimeError("Owned suspended carrier counterexample anchor changed")
            wrong=work/label/"Provider.java";wrong.parent.mkdir()
            wrong.open("x").write(original_source.replace(before,changed,1))
            compiled=work/(label+"-classes");verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),
                "SuspendedCarrierHarness","1","0","0","0"],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned suspended carrier contract" not in result.stderr:
                raise RuntimeError("Frozen observer missed suspended carrier change: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or
            hashlib.sha256(source_dex.read_bytes()).hexdigest()!=dex_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash or
            hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash):raise RuntimeError("Original suspended carrier input, SDK or frozen observer changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"dependency_provider":args.dependency_provider,
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"independent_wrong_programs":len(mutations),
            "producer_closed_completion_and_invariant_result_contracts":"unchanged","original_definitions_metadata_and_exceptions":"unchanged",
            "callback_completion_snapshots_carrier_identity_and_exact_failures":"matched",
            "original_inputs_and_frozen_observers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
