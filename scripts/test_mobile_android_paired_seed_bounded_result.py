#!/usr/bin/env python3
"""Verify direct seeds and both bounded result slots retain original provider behavior."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-paired-seed-bounded-result-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"paired_seed_bounded_result/java"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in ("PairedSeedBoundedResultCaller","PairedSeedBoundedResultHarness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=7 or len(all_methods)!=14:raise RuntimeError("Original paired seed bounded result inventory changed")
        classes=all_classes-({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Provider;->create(Lfixture/Carrier;Lfixture/Link;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;I)Lfixture/Result;"
        signature="<T:Ljava/lang/Object;V:Lfixture/Bound;>(Lfixture/Carrier<TT;>;Lfixture/Link<TT;TV;>;TT;TT;TT;I)Lfixture/Result<TT;TV;>;"
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete paired bounded result relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Provider.class",(originals/"fixture/Provider.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(14):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"PairedSeedBoundedResultHarness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original paired seed bounded result matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original paired seed bounded result DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original paired seed bounded result entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Paired seed bounded result recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Paired seed bounded result recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original paired seed bounded result class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original paired seed bounded result method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Paired seed bounded result recovery changed seed identities, converter and callback order, return identity or failures")
        mutations=(("swapped-seeds","Provider","new Result<T,V>(first,second,third,converter)","new Result<T,V>(second,first,third,converter)",0),
                   ("replaced-result","Provider","return (Result<T,V>)Trace.other;","return new Result<T,V>(first,second,third,converter);",11),
                   ("lost-converter","Provider","Trace.converter=converter;","Trace.converter=null;",0),
                   ("early-first-write","Result","Trace.partial=this;Trace.hit(3);","Trace.partial=this;this.first=first;Trace.hit(3);",7))
        for label,name,before,after,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original paired result counterexample anchor changed")
            if label=="early-first-write":text=text.replace("this.first=first;Trace.hit(4);","Trace.hit(4);")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                "PairedSeedBoundedResultHarness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned paired seed bounded result" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect paired result: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "both_original_result_formals":"matched","all_original_entries_and_metadata":"unchanged",
            "seeds_converter_input_reads_arbitrary_results_partial_fields_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
