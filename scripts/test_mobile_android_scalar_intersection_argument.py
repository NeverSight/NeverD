#!/usr/bin/env python3
"""Verify a non-first intersection argument retains its witness, scalar words and dispatch."""
from __future__ import annotations
import argparse
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_visibility_generic_bridge import compiler_facts


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scalar-intersection-argument-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"scalar_intersection_argument/java"
        harness_prefix="ScalarIntersectionArgument"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Future","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        cli_hash=hashlib.sha256(args.neverd.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=8 or len(all_methods)!=11:raise RuntimeError("Original scalar intersection arguments inventory changed")
        declarations={"Lfixture/Bound;","Lfixture/Flag;","Lfixture/Provider;","Lfixture/Child;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=compiler_facts(originals),exception_inventory(originals)
        signature="<T:Lfixture/Bound;:Lfixture/Flag;>(JITT;ZD)V"
        identity="Lfixture/Provider;->accept(JILfixture/Bound;ZD)V"
        if records["Lfixture/Provider;"]["methods"][identity]["signature"]!=signature:
            raise RuntimeError("Original scalar intersection Signature changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for owner in sorted(declarations):
                name=owner[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if declarations:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for future in ("false","true"):
                    for mode in range(16):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_prefix+"Harness",
                            str(route),future,str(mode),"false"],f"{label}-{route}-{future}-{mode}")
                        pattern=f"route={route},future={future},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original scalar intersection matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        call="provider.accept(number, label, value, enabled, fraction);"
        mutations=(
            ("lost-number","Provider.java","Trace.number = number;","Trace.number = 0;",0),
            ("wrong-label","Provider.java","Trace.label = label;","Trace.label = label + 1;",0),
            ("wrong-boolean","Provider.java","Trace.enabled = enabled;","Trace.enabled = !enabled;",0),
            ("lost-fraction","Provider.java","Trace.fraction = fraction;","Trace.fraction = 0;",0),
            ("lost-value","Provider.java","Trace.value = value;","Trace.value = null;",0),
            ("lost-receiver","Provider.java","Trace.receiver = this;","Trace.receiver = null;",0),
            ("duplicate-callback","Provider.java","Trace.action.run();","Trace.action.run(); Trace.action.run();",0),
            ("wrapped-checked","Provider.java","Trace.action.run();","try { Trace.action.run(); } catch(Throwable error) { throw new IllegalStateException(error); }",6),
            ("lost-touch","Provider.java","if (value != null) value.touch();",";",0),
            ("lost-completion","Provider.java","Trace.hit(8);",";",0),
            ("lost-dispatch","Core.java",call,"new Child().accept(number, label, value, enabled, fraction);",0),
            ("narrowed-wide-input","Core.java",call,"provider.accept((int)number, label, value, enabled, fraction);",0),
        )
        for label,filename,before,after,mode in mutations:
            original=(fixture/"fixture"/filename).read_text()
            if original.count(before)!=1:raise RuntimeError("Original scalar intersection counterexample anchor changed: "+label)
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1","true",str(mode),"false"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned scalar intersection argument invariant" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect intersection argument behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        dex_hash=hashlib.sha256(source_dex.read_bytes()).hexdigest()
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original scalar intersection arguments DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original scalar intersection arguments entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        expected_status={m:("recovered" if records[m.split("->",1)[0]]["methods"][m]["code"] else "declaration-only") for m in methods}
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") or coverage["status"]!="recovered"
                or any(row["status"]!=expected_status[row["identity"]] for row in coverage["methods"])
                or coverage["recovered_method_count"]!=sum(status=="recovered" for status in expected_status.values())
                or coverage.get("declaration_only_method_count",0)!=sum(status=="declaration-only" for status in expected_status.values())
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Scalar intersection arguments changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=expected_helpers:
            raise RuntimeError("Scalar intersection arguments recovery invented or omitted a runtime entry")
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            wanted,actual=copy.deepcopy(records[c]),copy.deepcopy(after[c])
            actual["methods"]={m:row for m,row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major")!=52 or actual.pop("major")!=44+coverage.get("minimum_java_release",8):
                raise RuntimeError("Scalar intersection arguments compiler release changed")
            if actual!=wanted or any(after_exceptions[m]!=exceptions[m] for m in records[c]["methods"]):
                raise RuntimeError("Original scalar intersection arguments Code, roles, Signature, Exceptions or parameters changed: "+c)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Scalar intersection arguments recovery changed scalar words, witnesses, dispatch or effects")
        original_signature=signature.encode()
        negatives=(
            ("missing-method-slot","Provider",original_signature,original_signature.replace(b"TT;",b"TMissing;")),
            ("open-class-bound","Provider",original_signature,original_signature.replace(b"T:Lfixture/Bound;",b"T:Ljava/lang/Object;")),
            ("missing-interface-bound","Provider",original_signature,original_signature.replace(b"Lfixture/Flag;",b"Lfixture/Missing;")),
            ("duplicate-interface-bound","Provider",original_signature,original_signature.replace(b":Lfixture/Flag;>",b":Lfixture/Flag;:Lfixture/Flag;>")),
            ("changed-wide-erasure","Provider",original_signature,original_signature.replace(b">(JI",b">(II")),
            ("changed-result-erasure","Provider",original_signature,original_signature.replace(b")V",b")Ljava/lang/Object;")),
            ("extra-method-formal","Provider",original_signature,original_signature.replace(b">(",b"U:Ljava/lang/Object;>(")),
            ("changed-formal-position","Provider",original_signature,original_signature.replace(b"(JITT;ZD)",b"(TT;IJZD)")),
        )
        for label,owner,original_signature,changed in negatives:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            if original_signature==changed:raise RuntimeError("Signature control did not change metadata: "+label)
            target=negative/"fixture"/(owner+".class");code_before=method_code(target.read_bytes())
            receipt=change_signature(target,original_signature,changed)
            if method_code(target.read_bytes())!=code_before:raise RuntimeError("Signature control changed Code")
            negative_classpath=classpath
            if declarations:
                changed_dependency=work/(label+"-provider.jar")
                with zipfile.ZipFile(changed_dependency,"x") as jar:
                    for owner in sorted(declarations):
                        entry=owner[1:-1]+".class";jar.writestr(entry,(negative/entry).read_bytes())
                negative_classpath=["--classpath",args.android_jar.resolve(),"--classpath",changed_dependency]
            (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
            negative_dex=work/(label+"-dex");negative_dex.mkdir()
            verify.run([*verify.d8,"--debug","--min-api","24",*negative_classpath,"--output",negative_dex,*[negative/path.relative_to(originals) for path in paths]],label+"-d8")
            rejected=work/(label+"-rejected")
            result=subprocess.run([args.neverd.resolve(),"mobile",negative_dex/"classes.dex",*negative_classpath,"-o",rejected],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or rejected.exists() or "generic" not in result.stderr:raise RuntimeError("Unproved scalar intersection metadata accepted: "+label)
        negative=work/"missing-witness-originals";shutil.copytree(originals,negative)
        target=negative/"fixture/Value.class";before=target.read_bytes()
        old=b"fixture/Flag";new=b"fixture/Else"
        pattern=b"\x01"+len(old).to_bytes(2,"big")+old
        replacement=b"\x01"+len(new).to_bytes(2,"big")+new
        if before.count(pattern)!=1:raise RuntimeError("Original witness interface constant changed")
        after_bytes=before.replace(pattern,replacement,1)
        if method_code(before)!=method_code(after_bytes):raise RuntimeError("Witness control changed Code")
        target.write_bytes(after_bytes)
        for route in range(4):
            result=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(negative,harness))),
                harness_prefix+"Harness",str(route),"false","0","true"],"late-bound-failure-"+str(route))
            expected=[f"route={route},future=false,mode=0,attempt={attempt},events=456" for attempt in range(2)]
            if result.splitlines()!=expected:raise RuntimeError("Original missing bound failed before provider effects")
        negative_dex=work/"missing-witness-dex";negative_dex.mkdir()
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",negative_dex,
            *[negative/path.relative_to(originals) for path in paths]],"missing-witness-d8")
        rejected=work/"missing-witness-rejected"
        result=subprocess.run([args.neverd.resolve(),"mobile",negative_dex/"classes.dex",*classpath,"-o",rejected],
            text=True,capture_output=True,timeout=300,env=verify.environment)
        (work/"missing-witness-recovery.log").open("x").write(result.stdout+result.stderr)
        if result.returncode==0 or rejected.exists() or "intersection" not in result.stderr:
            raise RuntimeError("Call-site witness accepted a missing original interface bound")
        (work/"witness-control.json").open("x").write(json.dumps({"status":"verified","all_Code":"unchanged",
            "original_failure":"ClassCastException after provider callback and partial effects",
            "before_sha256":hashlib.sha256(before).hexdigest(),"after_sha256":hashlib.sha256(after_bytes).hexdigest()},indent=2)+"\n")
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash
                or hashlib.sha256(source_dex.read_bytes()).hexdigest()!=dex_hash
                or hashlib.sha256(args.neverd.read_bytes()).hexdigest()!=cli_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "all_java_units":len(list((output/"sources").rglob("*.java"))),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "nonfirst_intersection_witness_and_scalar_word_positions":"matched","all_original_entries_and_metadata":"unchanged",
            "original_dispatch_wide_input_snapshots_future_overrides_partial_effects_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_witness_control":"rejected-after-original-late-failure-was-qualified","same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
