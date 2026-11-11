#!/usr/bin/env python3
"""Verify closed generic superclass calls reuse their original guarded prefix and frame."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-super-arguments-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"closed_super_arguments/java"
        harness_name="ClosedSuperArguments"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=25)
        compatible=work/"java8-declarations"
        verify.compile_local([p for p in sorted(fixture.rglob("*.java")) if p.name!="Child.java"],compatible,classpath=originals,release=8)
        for p in compatible.rglob("*.class"):shutil.copy2(p,originals/p.relative_to(compatible))
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=7 or len(all_methods)!=16:raise RuntimeError("Original closed super arguments inventory changed")
        declarations={"Lfixture/Fn;","Lfixture/Unit;","Lfixture/Provider;","Lfixture/Parent;","Lfixture/Trace;"}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        target="Lfixture/Parent;-><init>(JLjava/lang/Object;Lfixture/Fn;Lfixture/Fn;)V"
        signature="(JLjava/lang/Object;Lfixture/Fn<Ljava/lang/Object;Lfixture/Unit;>;Lfixture/Fn<Ljava/lang/Object;Lfixture/Unit;>;)V"
        if records["Lfixture/Parent;"]["methods"][target]["signature"]!=signature:
            raise RuntimeError("Original closed superclass constructor contract changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Fn","Unit","Provider","Parent","Trace"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for variant in range(16):
                    for mode in (range(14) if variant in (0,4,8,12) else (0,10)):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                            str(route),str(variant),str(mode)],f"{label}-{route}-{variant}-{mode}")
                        pattern=f"route={route},variant={variant},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original closed superclass matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original closed super arguments DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original closed super arguments entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-closed-super-disassembly")
        witnessed=False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            name=re.search(r"(?m)^\s+name\s+: '([^']+)'",block[2])
            if block[1]!="Lfixture/Child;" or not name or name[1]!="<init>":continue
            ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)",block[2])
            if ("move-result-wide" not in ops or sum(op.startswith("if-") for op in ops)<4
                    or "Lfixture/Parent;.<init>:(JLjava/lang/Object;Lfixture/Fn;Lfixture/Fn;)V" not in block[2]
                    or ops.index("move-result-wide")>=next(i for i,op in enumerate(ops) if op.startswith("invoke-direct"))):
                raise RuntimeError("Original guarded superclass prefix witness changed")
            witnessed=True
        if not witnessed:raise RuntimeError("Missing original guarded superclass prefix")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Closed super arguments recovery changed original entries or metadata contracts")
        if coverage.get("minimum_java_release")!=25:raise RuntimeError("Closed superclass prologue source release changed")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c != "Lfixture/Fn;" }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Closed super arguments recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original closed super arguments class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original closed super arguments method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Closed super arguments recovery changed seed/null branch, primitive raw bits, actual result identity or failures")
        same_code=(
            ("unused-formal","<T:Ljava/lang/Object;>"+signature),
            ("open-input","<T:Ljava/lang/Object;>"+signature.replace("<Ljava/lang/Object;Lfixture/Unit;>","<TT;Lfixture/Unit;>",1)),
            ("open-result","<T:Ljava/lang/Object;>"+signature.replace("<Ljava/lang/Object;Lfixture/Unit;>","<Ljava/lang/Object;TT;>",1)),
            ("unscoped-formal",signature.replace("<Ljava/lang/Object;Lfixture/Unit;>","<TT;Lfixture/Unit;>",1)),
            ("wrong-erasure",signature.replace("Lfixture/Fn<Ljava/lang/Object;Lfixture/Unit;>;","Ljava/lang/Object;",1)),
            ("wrong-result",signature[:-1]+"Ljava/lang/Object;"),
        )
        for label,changed in same_code:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            receipt=change_signature(negative/"fixture/Parent.class",signature.encode(),changed.encode())
            (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
            negative_dex=work/(label+"-dex");negative_dex.mkdir()
            verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",negative_dex,
                *[negative/p.relative_to(originals) for p in paths]],label+"-d8")
            if args.dependency_inputs:
                changed_jar=work/(label+"-provider.jar")
                with zipfile.ZipFile(changed_jar,"x") as jar:
                    for owner in sorted(declarations):
                        name=owner[1:-1]+".class";jar.writestr(name,(negative/name).read_bytes())
                negative_classpath=classpath[:-1]+[changed_jar]
            else:negative_classpath=classpath
            rejected=work/(label+"-rejected")
            result=subprocess.run([str(args.neverd.resolve()),"mobile",str(negative_dex/"classes.dex"),*map(str,negative_classpath),"-o",str(rejected)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or rejected.exists() or not any(t in result.stderr.lower() for t in ("generic","signature","constructor","erasure")):
                raise RuntimeError("Closed super arguments accepted an unproved signature: "+label)
        mutations=(
            ("lost-read-selection","Child","first!=null ? first : ","",1,0),
            ("lost-write-selection","Child","second!=null ? second : ","",10,0),
            ("lost-wide-word","Child","long bits=Trace.wide();","long bits=0;Trace.wide();",0,0),
            ("narrowed-wide-word","Child","long bits=Trace.wide();","long bits=(int)Trace.wide();",0,0),
            ("lost-token","Child","super(bits,token,read,write);","super(bits,null,read,write);",0,0),
            ("early-final-store","Child","Trace.hit(7);\n        marker=token;","marker=token;\n        Trace.hit(7);",0,7),
            ("lost-callback","Parent","return (write ? second : first).apply(input);","return null;",0,0),
            ("changed-failure","Child","long bits=Trace.wide();","long bits;try {bits=Trace.wide();} catch(RuntimeException e) {throw new IllegalStateException(e);}",0,1),
        )
        for label,name,before,after,variant,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original closed super counterexample anchor changed: "+label)
            changed=work/label/(name+".java");changed.parent.mkdir();changed.open("x").write(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=25)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_name+"Harness","1",str(variant),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned closed super arguments" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect closed superclass arguments: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
            "guarded_prefix_closed_callback_arguments_and_wide_words":"matched","all_original_entries_and_metadata":"unchanged",
            "partial_construction_final_store_exact_failure_and_callback_cast":"matched",
            "same_code_signature_mutations":len(same_code),"independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
