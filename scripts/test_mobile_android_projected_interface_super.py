#!/usr/bin/env python3
"""Verify a proven interface projection leaves the original superclass constructor intact."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
from copy import deepcopy
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-projected-interface-super-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"projected_interface_super/java"
        harness_name="ProjectedInterfaceSuper"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")],
            harness,classpath=originals,release=8)
        receiver=originals/"fixture/Receiver.class";before=receiver.read_bytes()
        input_signature="<S:Ljava/lang/Object;>Lfixture/Base;Lfixture/Function<Lfixture/CarrierA;Lfixture/CarrierB;Lfixture/CarrierB;>;"
        edits={"signature":change_signature(receiver,b"<S:Ljava/lang/Object;>Lfixture/Base;Lfixture/Function;",input_signature.encode()),
               "flags":bridge_flag(receiver,"invoke","(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",original_flags=0x1,input_flags=0x1041)}
        if method_code(before)!=method_code(receiver.read_bytes()):raise RuntimeError("Interface receipt encoding changed original Code")
        (work/"input-encoding.json").write_text(json.dumps(edits,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=8 or len(all_methods)!=15:raise RuntimeError("Original projected interface super inventory changed")
        declarations={"Lfixture/Base;","Lfixture/Sink;","Lfixture/Function;","Lfixture/CarrierA;","Lfixture/CarrierB;"}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Receiver;-><init>(Lfixture/Sink;)V"
        signature="(Lfixture/Sink<-Lfixture/Receiver;>;)V"
        if records["Lfixture/Receiver;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete invariant key relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Base","Sink","Function","CarrierA","CarrierB"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(9):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original projected interface super matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original projected interface super DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original projected interface super entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0)!=1 or coverage["status"]!="partial"
                or coverage.get("class_source_bindings") or len(coverage.get("generic_signature_projections",[]))!=1):
            raise RuntimeError("Projected interface super recovery changed original entries or metadata contracts")
        projection=coverage["generic_signature_projections"][0]
        bridge="Lfixture/Receiver;->invoke(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
        change=projection["changes"]
        if (projection["declaration"]!="Lfixture/Receiver;" or projection["original_signature"]!=input_signature
                or len(change)!=1 or change[0]["kind"]!="checked-value-carrier-interface"
                or change[0]["scope_origin"]!=bridge or change[0]["source_type"]!="Lfixture/Function;"):
            raise RuntimeError("Missing exact interface projection receipt")
        row=next(r for r in coverage["methods"] if r["identity"]==bridge)
        if row.get("projection_kind")!="value-carrier-interface-bridge" or not row.get("metadata_differences"):
            raise RuntimeError("Original bridge metadata differences were not disclosed")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Sink;","Lfixture/Function;"} }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Projected interface super recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                wanted=records[c].get(key)
                if c=="Lfixture/Receiver;" and key=="signature":wanted="<S:Ljava/lang/Object;>Lfixture/Base;Lfixture/Function;"
                if wanted!=after[c].get(key):raise RuntimeError("Original projected interface super class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                wanted=deepcopy(metadata)
                if identity=="Lfixture/Receiver;->invoke(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;":wanted["access"] &= ~0x1040
                if after[c]["methods"].get(identity)!=wanted or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original projected interface super method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Projected interface super recovery changed lookup identity, bound casts or failure order")
        mutations=(("changed-label","Receiver","super(2,","super(7,",0),
                   ("double-callback","Base","sink.accept(this);","sink.accept(this);sink.accept(this);",0),
                   ("late-publication","Base","Trace.partial=this;Trace.hit(1);","Trace.hit(1);",0),
                   ("lost-wide","Receiver","return b;","return (int)b;",0))
        for label,name,before,after,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original projected interface super counterexample anchor changed")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            marker="owned projected interface super"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect projected interface super: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
            "unchanged_superclass_and_constructor_parameter_subtrees":"matched","all_original_entries_and_metadata":"matched except recorded interface Signature and bridge flags",
            "publication_original_self_sink_virtual_dispatch_wide_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
