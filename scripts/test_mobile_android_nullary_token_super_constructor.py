#!/usr/bin/env python3
"""Verify nullary constructors retain class-token resolution and closed superclass binding and exact effects."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nullary-token-super-constructor-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"nullary_token_super_constructor/java"
        harness_prefix="NullaryTokenSuperConstructor"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        cli_hash=hashlib.sha256(args.neverd.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=5 or len(all_methods)!=8:raise RuntimeError("Original nullary token super constructor inventory changed")
        declarations={"Lfixture/Parent;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=compiler_facts(originals),exception_inventory(originals)
        signature="(Ljava/lang/Class<TB;>;Ljava/lang/String;)V"
        identity="Lfixture/Parent;-><init>(Ljava/lang/Class;Ljava/lang/String;)V"
        if records["Lfixture/Parent;"]["methods"][identity]["signature"]!=signature:
            raise RuntimeError("Original nullary token superclass Signature changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for owner in sorted(declarations):
                name=owner[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if declarations:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            missing=work/(label+"-without-probe.jar")
            with zipfile.ZipFile(missing,"x") as jar:
                for path in sorted(runtime[0].rglob("*.class")):
                    entry=path.relative_to(runtime[0]).as_posix()
                    if entry!="fixture/Probe.class":jar.writestr(entry,path.read_bytes())
            outputs=[]
            for route in range(4):
                for mode in range(12):
                    paths=(missing,*runtime[1:]) if mode==11 else runtime
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,paths)),harness_prefix+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original nullary constructor matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(
            ("wrong-token", "Child.java", "super(Probe.class,", "super((Class)Marker.class,", 0),
            ("wrong-label", "Child.java", '"owned token label"', '"owned changed label"', 0),
            ("lost-callback", "Parent.java", "Trace.action.run();", ";", 0),
            ("replaced-failure", "Parent.java", "Trace.action.run();", "try { Trace.action.run(); } catch(RuntimeException failure) { throw new IllegalArgumentException(failure); }", 4),
            ("replaced-error", "Parent.java", "Trace.action.run();", "try { Trace.action.run(); } catch(Error failure) { throw new AssertionError(failure); }", 5),
            ("lost-parent-publication", "Parent.java", "Trace.receiver = this;", ";", 2),
            ("eager-token-initialization", "Child.java", "return new Child();", 'try { Class.forName("fixture.Probe"); } catch (ClassNotFoundException failure) { throw new AssertionError(failure); } return new Child();', 0),
            ("lost-result-write", "Child.java", "value = Trace.result;", "value = null;", 0),
            ("repeated-construction", "Child.java", "return new Child();", "new Child(); return new Child();", 0),
            ("late-callback", "Parent.java", "Trace.action.run(); Trace.hit(4);", "Trace.hit(4); Trace.action.run();", 0),
        )
        for label,filename,before,after,mode in mutations:
            original=(fixture/"fixture"/filename).read_text()
            if original.count(before)!=1:raise RuntimeError("Original token superclass constructor counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned nullary token super constructor" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect token superclass constructor behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        dex_hash=hashlib.sha256(source_dex.read_bytes()).hexdigest()
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original nullary token super constructor DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original nullary token super constructor entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Nullary token super constructor recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=expected_helpers:
            raise RuntimeError("Nullary token super constructor recovery invented or omitted a runtime entry")
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            wanted,actual=copy.deepcopy(records[c]),copy.deepcopy(after[c])
            actual["methods"]={m:row for m,row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major")!=52 or actual.pop("major")!=44+coverage.get("minimum_java_release",8):
                raise RuntimeError("Nullary token super constructor compiler release changed")
            if actual!=wanted or any(after_exceptions[m]!=exceptions[m] for m in records[c]["methods"]):
                raise RuntimeError("Original nullary token super constructor Code, roles, Signature, Exceptions or parameters changed: "+c)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Nullary token super constructor recovery changed input identities, configuration, opaque results or effects")
        original_signature=signature.encode()
        parent_signature=records["Lfixture/Parent;"]["signature"].encode()
        child_signature=records["Lfixture/Child;"]["signature"].encode()
        negatives=(
            ("missing-token-slot", "Parent", original_signature, original_signature.replace(b"TB;", b"TMissing;")),
            ("method-shadow", "Parent", original_signature, b"<B:Ljava/lang/Object;>"+original_signature),
            ("wrong-token-rank", "Parent", original_signature, original_signature.replace(b"(Ljava/lang/Class", b"([Ljava/lang/Class")),
            ("wrong-input-arity", "Parent", original_signature, original_signature.replace(b")V", b"Ljava/lang/Object;)V")),
            ("wrong-parent-bound", "Parent", parent_signature, parent_signature.replace(b"B:Ljava/lang/Object;", b"B:Ljava/lang/Number;")),
            ("unbound-header-slot", "Child", child_signature, child_signature.replace(b"Lfixture/Probe;", b"TMissing;")),
            ("missing-header-slots", "Child", child_signature, b"Lfixture/Parent;"),
            ("variant-header-slot", "Child", child_signature, child_signature.replace(b"Lfixture/Probe;", b"+Lfixture/Probe;")),
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
            if result.returncode==0 or rejected.exists() or "generic" not in result.stderr:raise RuntimeError("Unproved nullary token superclass metadata accepted: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash
                or hashlib.sha256(source_dex.read_bytes()).hexdigest()!=dex_hash
                or hashlib.sha256(args.neverd.read_bytes()).hexdigest()!=cli_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "all_java_units":len(list((output/"sources").rglob("*.java"))),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "nullary_constructor_and_closed_parent_slot_substitution":"matched","all_original_entries_and_metadata":"unchanged",
            "token_resolution_initialization_partial_state_callbacks_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
