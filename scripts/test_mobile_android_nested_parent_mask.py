#!/usr/bin/env python3
"""Verify raw mask delegation under a complete nested generic parent edge."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nested-parent-mask-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"nested_parent_mask/java"
        harness_name="NestedParentMask"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")],harness,classpath=originals,release=8)
        mask_prototype="(IJLfixture/Sink;ILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        mask_identity="Lfixture/Core;-><init>"+mask_prototype
        receipt=change_method_flags(originals/"fixture/Core.class",mask_prototype.encode(),0x1,0x1001,name=b"<init>")
        (work/"mask-flag-witness.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=12 or len(all_methods)!=17:raise RuntimeError("Original nested parent mask inventory changed")
        declarations={"Lfixture/"+n+";" for n in ("Parent","Bound","Middle","View","Sink")}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/"owned-inputs.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for n in ("Parent","Bound","Middle","View","Sink"):jar.writestr("fixture/"+n+".class",(originals/("fixture/"+n+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label,synthetic):
            outputs=[]
            for route in range(4):
                for mode in range(64):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"fixture."+harness_name+"Harness",str(route),str(mode),str(synthetic).lower()],f"{label}-{route}-{mode}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in output.splitlines()):
                        raise RuntimeError("Nested parent mask observer output changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline",True)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original nested parent masks DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original nested parent mask entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        mutations=(("lost-default-bits","Core","(mask&2)!=0?Defaults.defaultBits():bits","bits",1),
                   ("lost-default-callback","Core","(mask&4)!=0?Defaults.CB:callback","callback",2),
                   ("lost-wide","Core","this.bits=bits;","this.bits=(int)bits;",0),
                   ("lost-callback","Core","callback.accept((T)Trace.token);","Trace.seen=null;",0),
                   ("wrong-parent","Parent","boot=17;","boot=0;",0),
                   ("early-default","Core","Trace.partial=this;","Object ignored=Defaults.CB;Trace.partial=this;",0),
                   ("late-fields","Core","this.count=count;this.bits=bits;this.callback=callback;\n        Trace.hit(7);",
                    "Trace.hit(7);this.count=count;this.bits=bits;this.callback=callback;",28))
        for label,owner,old,new,mode in mutations:
            source=(fixture/("fixture/"+owner+".java")).read_text()
            if source.count(old)!=1:raise RuntimeError("Nested parent mask wrong-program anchor changed")
            changed=work/label/(owner+".java");changed.parent.mkdir();changed.open("x").write(source.replace(old,new))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),"fixture."+harness_name+"Harness","1",str(mode),str(owner!="Core").lower()],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned nested parent mask" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect constructor behavior: "+label)
        evidence={"status":"baseline-only" if args.baseline_only else "success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,"independent_wrong_programs":len(mutations)}
        if not args.baseline_only:
            output=work/"recovered"
            verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
            coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods)
                    or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=1 or coverage["status"]!="partial"
                    or coverage.get("class_source_bindings") or coverage.get("field_source_bindings") or coverage.get("generic_signature_projections")):
                raise RuntimeError("Nested parent mask binding changed original entries or generic contracts")
            projected=[row for row in coverage["methods"] if row["status"]=="source-projected"]
            if (len(projected)!=1 or projected[0]["identity"]!=mask_identity or projected[0].get("projection_kind")!="synthetic-constructor"
                    or projected[0].get("source_identity")!=mask_identity or len(projected[0].get("metadata_differences",[]))!=1):
                raise RuntimeError("Raw mask entry lost its exact flag receipt")
            rebuilt=work/"rebuilt"
            verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
            actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
            helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
            expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Sink;","Lfixture/View;"}}
            if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
                raise RuntimeError("Nested parent mask binding invented or omitted a runtime entry")
            after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
            for c in classes:
                for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                    if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original nested parent mask metadata changed : "+c+":"+key)
                for identity,metadata in records[c]["methods"].items():
                    wanted=deepcopy(metadata)
                    if identity==mask_identity:wanted["access"] &= ~0x1000
                    if after[c]["methods"].get(identity)!=wanted or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Original nested mask method metadata changed: "+identity)
            runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
            if execute(runtime,"rebuilt",False)!=baseline:raise RuntimeError("Nested parent mask binding changed storage, initialization or failure identity")
            evidence.update({"all_original_entries_and_metadata":"unchanged except exact reported mask synthetic flag",
                "nested_owner_bound_parent_initialization_mask_wide_callback_partial_fields_and_failures":"matched"})
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence["original_inputs_and_immutable_callers"]="unchanged"
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
