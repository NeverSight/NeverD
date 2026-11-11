#!/usr/bin/env python3
"""Verify protected conditional arguments retain allocation order and exception frames."""
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
    parser.add_argument("--dependency-inputs",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-conditional-allocation-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"protected_conditional_allocation/java"
        harness_name="ProtectedConditionalAllocation"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=5 or len(all_methods)!=11:raise RuntimeError("Original protected conditional allocation inventory changed")
        declarations={"Lfixture/Provider;","Lfixture/Checked;","Lfixture/Target;"}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Provider","Checked","Target"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            modes=((0,1,2,3,4,5,6,7,8),(0,1,2,5,6,7,8),(0,1,2,5,6,7,8),
                   (0,1,2,3,7,8),(0,1,2,3,7,8),(0,1,2,3,4,5,6,7,8),(0,1,7,8))
            for route in range(4):
                for variant,selected in enumerate(modes):
                    for mode in selected:
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                            str(route),str(variant),str(mode)],f"{label}-{route}-{variant}-{mode}")
                        pattern=f"route={route},variant={variant},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original protected conditional allocation matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original protected conditional allocation DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original protected conditional allocation entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-protected-conditional-disassembly")
        witnessed=False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            name=re.search(r"(?m)^\s+name\s+: '([^']+)'",block[2])
            if block[1]!="Lfixture/Maker;" or not name or name[1]!="make":continue
            ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)",block[2])
            if (ops.count("new-instance")!=1 or ops.count("instance-of")!=1
                    or ops.count("check-cast")!=1 or block[2].count("Lfixture/Provider;.current:")!=2
                    or "move-exception" not in ops or "catches" not in block[2]
                    or ops.index("new-instance")>ops.index("instance-of")):
                raise RuntimeError("Original protected conditional DEX witness changed")
            witnessed=True
        if not witnessed:raise RuntimeError("Missing original protected conditional DEX witness")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Protected conditional allocation recovery changed original entries or metadata contracts")
        if coverage.get("minimum_java_release")!=14:raise RuntimeError("Protected conditional argument source release changed")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Protected conditional allocation recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original protected conditional allocation class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original protected conditional allocation method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Protected conditional allocation recovery changed seed/null branch, primitive raw bits, actual result identity or failures")
        mutations=(
            ("cached-getter","provider.current() instanceof Checked\n                    ? ((Checked)provider.current()).value()","provider.first instanceof Checked\n                    ? ((Checked)provider.first).value()",0,0),
            ("early-argument","Object current=seed;","Object early=provider.current();Object current=seed;",0,1),
            ("lost-type-test","provider.current() instanceof Checked","provider.current()!=null",2,0),
            ("lost-incoming-copy","Object current=seed;","Object current=null;",0,2),
            ("late-commit","new Target(current=provider.current()","new Target(provider.current()",0,5),
            ("lost-handler-input","Trace.observed=current;","Trace.observed=null;",0,2),
            ("replaced-exception","throw failure;","throw new IllegalStateException(failure);",0,2),
            ("lost-finally","Trace.event(8);",";",0,0),
        )
        for label,before,after,variant,mode in mutations:
            text=(fixture/"fixture/Maker.java").read_text()
            if text.count(before)!=1:raise RuntimeError("Original protected conditional counterexample anchor changed: "+label)
            changed=work/label/"Maker.java";changed.parent.mkdir();changed.open("x").write(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_name+"Harness","1",str(variant),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned protected conditional allocation" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect protected conditional allocation: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
            "allocation_type_test_repeated_getter_and_wide_argument":"matched","all_original_entries_and_metadata":"unchanged",
            "catch_input_identity_finally_replacement_and_exact_failure":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
