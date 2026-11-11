#!/usr/bin/env python3
"""Verify bounded class formals retain inherited superclass dispatch and original input words and exact effects."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-formal-super-call-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"bounded_formal_super_call/java"
        harness_prefix="BoundedFormalSuperCall"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        cli_hash=hashlib.sha256(args.neverd.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=7 or len(all_methods)!=12:raise RuntimeError("Original bounded formal super call inventory changed")
        declarations={"Lfixture/Base;","Lfixture/Parent;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=compiler_facts(originals),exception_inventory(originals)
        signature="(Lfixture/Context;TV;I)Z"
        identity="Lfixture/Base;->run(Lfixture/Context;Lfixture/Bound;I)Z"
        if records["Lfixture/Base;"]["methods"][identity]["signature"]!=signature:
            raise RuntimeError("Original bounded formal super Signature changed")
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
                for future in range(2):
                    for mode in range(12):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_prefix+"Harness",
                            str(route),str(future),str(mode)],f"{label}-{route}-{future}-{mode}")
                        pattern=f"route={route},future={future},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original bounded formal super matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(
            ("wrong-receiver", "Leaf.java", "super.run(context, value, bits)", "new Base<T>().run(context, value, bits)", 0, 0),
            ("virtual-dispatch", "Leaf.java", "super.run(context, value, bits)", "run(context, value, bits)", 0, 0),
            ("lost-callback", "Base.java", "Trace.action.run();", ";", 0, 0),
            ("replaced-runtime", "Base.java", "Trace.action.run();", "try { Trace.action.run(); } catch(RuntimeException failure) { throw new IllegalArgumentException(failure); }", 0, 3),
            ("replaced-error", "Base.java", "Trace.action.run();", "try { Trace.action.run(); } catch(Error failure) { throw new AssertionError(failure); }", 0, 4),
            ("replaced-checked", "Base.java", "Trace.action.run();", "try { Trace.action.run(); } catch(Throwable failure) { throw new RuntimeException(failure); }", 0, 5),
            ("early-result", "Base.java", "Trace.hit(2); Trace.action.run(); Trace.hit(4);", "boolean saved = Trace.result; Trace.hit(2); Trace.action.run(); Trace.hit(4); Trace.result = saved;", 0, 0),
            ("negated-result", "Leaf.java", "return result;", "return !result;", 0, 0),
            ("lost-child-write", "Leaf.java", "Trace.after = value;", "Trace.after = null;", 0, 0),
            ("lost-value", "Leaf.java", "super.run(context, value, bits)", "super.run(context, (T)null, bits)", 0, 0),
        )
        for label,filename,before,after,future,mode in mutations:
            original=(fixture/"fixture"/filename).read_text()
            if original.count(before)!=1:raise RuntimeError("Original superclass call counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1",str(future),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned bounded formal super call" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect superclass call behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        dex_hash=hashlib.sha256(source_dex.read_bytes()).hexdigest()
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original bounded formal super call DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original bounded formal super call entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        bridge="Lfixture/Leaf$Child;->run(Lfixture/Context;Lfixture/Bound;I)Z"
        projected={row["identity"] for row in coverage["methods"] if row["status"]=="source-projected"}
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count")!=1 or projected!={bridge}
                or coverage["status"]!="partial" or any(row["status"]!="recovered" for row in coverage["methods"] if row["identity"]!=bridge)
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Bounded formal super call recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=expected_helpers:
            raise RuntimeError("Bounded formal super call recovery invented or omitted a runtime entry")
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            wanted,actual=copy.deepcopy(records[c]),copy.deepcopy(after[c])
            actual["methods"]={m:row for m,row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major")!=52 or actual.pop("major")!=44+coverage.get("minimum_java_release",8):
                raise RuntimeError("Bounded formal super call compiler release changed")
            if actual!=wanted or any(after_exceptions[m]!=exceptions[m] for m in records[c]["methods"]):
                raise RuntimeError("Original bounded formal super call Code, roles, Signature, Exceptions or parameters changed: "+c)
        def bridge_code(root,label):
            javap=Path(verify.java).with_name("javap.exe" if os.name=="nt" else "javap")
            dump=verify.run([javap,"-classpath",root,"-c","-s","-p","fixture.Leaf$Child"],label)
            marker="descriptor: (Lfixture/Context;Lfixture/Bound;I)Z"
            if dump.count(marker)!=1:raise RuntimeError("Original erased bridge descriptor changed")
            instructions=[]
            for line in dump.split(marker,1)[1].splitlines():
                match=re.fullmatch(r"\s*([0-9]+):\s+(.*)",line)
                if match:
                    text=re.sub(r"#\d+","#",match.group(2))
                    text=" ".join(text.split()).replace("fixture/Leaf$Child.run:","run:")
                    instructions.append(text)
                elif instructions:break
            expected=["aload_0","aload_1","aload_2","checkcast # // class fixture/Leaf",
                "iload_3","invokevirtual # // Method run:(Lfixture/Context;Lfixture/Leaf;I)Z","ireturn"]
            if instructions!=expected:raise RuntimeError("Original checked bridge bytecodes changed: "+repr(instructions))
            return {"instructions":instructions,"javap_sha256":hashlib.sha256(dump.encode()).hexdigest()}
        bridge_witness={"status":"verified-original-and-rebuilt-bridge","identity":bridge,
            "reported_status":"source-projected","original":bridge_code(originals,"original-bridge-code"),
            "rebuilt":bridge_code(rebuilt,"rebuilt-bridge-code")}
        (work/"disclosed-metadata.json").open("x").write(json.dumps(bridge_witness,indent=2)+"\n")
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Bounded formal super call recovery changed input identities, configuration, opaque results or effects")
        original_signature=signature.encode()
        child_signature=records["Lfixture/Leaf$Child;"]["signature"].encode()
        parent_signature=records["Lfixture/Parent;"]["signature"].encode()
        base_signature=records["Lfixture/Base;"]["signature"].encode()
        negatives=(
            ("missing-method-slot", "Base", original_signature, original_signature.replace(b"TV;", b"TMissing;")),
            ("shadowed-method-slot", "Base", original_signature, b"<V:Lfixture/Bound;>"+original_signature),
            ("changed-result-erasure", "Base", original_signature, original_signature.replace(b")Z", b")I")),
            ("missing-child-bound", "Leaf$Child", child_signature, child_signature.replace(b"T:Lfixture/Leaf;", b"T:Lfixture/Missing;")),
            ("open-child-bound", "Leaf$Child", child_signature, child_signature.replace(b"T:Lfixture/Leaf;", b"T:Ljava/lang/Object;")),
            ("missing-parent-edge", "Parent", parent_signature, parent_signature.replace(b"Base<TV;>", b"Base")),
            ("variant-parent-edge", "Parent", parent_signature, parent_signature.replace(b"Base<TV;>", b"Base<+TV;>")),
            ("wrong-base-bound", "Base", base_signature, base_signature.replace(b"V:Lfixture/Bound;", b"V:Ljava/lang/Number;")),
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
            if result.returncode==0 or rejected.exists() or "generic" not in result.stderr:raise RuntimeError("Unproved bounded formal super metadata accepted: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash
                or hashlib.sha256(source_dex.read_bytes()).hexdigest()!=dex_hash
                or hashlib.sha256(args.neverd.read_bytes()).hexdigest()!=cli_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "all_java_units":len(list((output/"sources").rglob("*.java"))),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "bounded_class_formals_and_inherited_superclass_substitution":"matched","compiler_regenerated_bridge":bridge,"normalized_original_and_rebuilt_bridge_code":"matched","all_original_entries_and_metadata":"unchanged",
            "original_dispatch_bridge_casts_future_overrides_partial_state_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
