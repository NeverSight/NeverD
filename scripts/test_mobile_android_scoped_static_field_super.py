#!/usr/bin/env python3
"""Verify static nominal field origins in scoped superclass constructor arguments."""
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
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-static-field-super-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"scoped_static_field_super/java"
        harness_prefix="ScopedStaticFieldSuper"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=6 or len(all_methods)!=11:raise RuntimeError("Original scoped static field super inventory changed")
        declarations={"Lfixture/Parent;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        signature="(Ljava/util/concurrent/ConcurrentMap<Ljava/lang/ref/Reference<TK;>;TV;>;Lfixture/Marker;)V"
        identity="Lfixture/Parent;-><init>(Ljava/util/concurrent/ConcurrentMap;Lfixture/Marker;)V"
        if records["Lfixture/Parent;"]["methods"][identity]["signature"]!=signature:
            raise RuntimeError("Original scoped static field super Signature changed")
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
                for operation in range(2):
                    for mode in range(12):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_prefix+"Harness",
                            str(route),str(operation),str(mode)],f"{label}-{route}-{operation}-{mode}")
                        pattern=f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original scoped static field super matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(
            ('changed-enum-value','Core.java','super(value,Marker.FIRST);','super(value,Marker.SECOND);',0,0),
            ('predicted-field-value','Core.java','super(value,Trace.selected);','super(value,Marker.FIRST);',1,0),
            ('lost-action','Parent.java','Trace.action.run();','',0,0),
            ('late-field-read','Parent.java','this.marker=marker;','this.marker=Trace.selected;',1,0),
            ('lost-completion-stage','Parent.java','Trace.parentObject=this;Trace.hit(3);','Trace.parentObject=this;',0,0),
            ('replaced-action-failure','Parent.java','Trace.action.run();','try {Trace.action.run();} catch(RuntimeException failure){throw new IllegalArgumentException(failure);}',0,4),
        )
        for label,filename,before,after,operation,mode in mutations:
            original=(fixture/"fixture"/filename).read_text()
            if original.count(before)!=1:raise RuntimeError("Original scoped static field super counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1",str(operation),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned scoped static field super" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect scoped static field super behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original scoped static field super DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original scoped static field super entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        enum="Lfixture/Marker;"
        projected={enum+"->$values()[Lfixture/Marker;":"synthetic-method",
            enum+"-><clinit>()V":"enum-initializer",
            enum+"-><init>(Ljava/lang/String;I)V":"enum-constructor",
            enum+"->values()[Lfixture/Marker;":"enum-values",
            enum+"->valueOf(Ljava/lang/String;)Lfixture/Marker;":"enum-valueOf"}
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count")!=5 or coverage["status"]!="partial"
                or coverage.get("generic_signature_projections")
                or {r["identity"]:r.get("projection_kind") for r in coverage["methods"] if r["status"]=="source-projected"}!=projected):
            raise RuntimeError("Scoped static field super recovery changed original entries or metadata contracts")
        bindings=coverage.get("class_source_bindings",[])
        if (len(bindings)!=1 or bindings[0].get("class")!=enum or bindings[0].get("binding_kind")!="enum-declaration"
                or bindings[0].get("compiler_generated_members_status")!="unverified"
                or bindings[0].get("values_field")!="$VALUES"
                or [(c["field"],c["ordinal"],c["implementation"],c["constructor"]) for c in bindings[0]["constants"]]
                   !=[(n,i,enum,enum+"-><init>(Ljava/lang/String;I)V") for i,n in enumerate(("FIRST","SECOND"))]):
            raise RuntimeError("Original enum declaration boundary changed")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        compiler_helpers=set(actual_methods)-methods-helpers
        before_records=class_identity.compiler_classes(rebuilt);before_exceptions=exception_inventory(rebuilt)
        marker=rebuilt/"fixture/Marker.class";marker_before=marker.read_bytes()
        code=method_code(marker_before)
        if (actual_classes!=classes or not helpers<=expected_helpers or len(compiler_helpers)!=1
                or any(not re.fullmatch(re.escape(enum)+r"->\$values\$+\(\)\["+re.escape(enum),m) for m in compiler_helpers)
                or any(before_records[enum]["methods"][m]["access"]!=0x100A
                       or before_records[enum]["methods"][m]["signature"] is not None
                       or before_exceptions[m] or not code[m.split("->",1)[1].encode()] for m in compiler_helpers)):
            raise RuntimeError("Enum compiler introduced an unaccounted class or method")
        identity=enum+"->$values()[Lfixture/Marker;"
        expected=dict(records[enum]["methods"][identity])
        if expected["access"]!=0x100A:raise RuntimeError("Original enum helper role changed")
        expected["access"]=0x000A
        if before_records[enum]["methods"].get(identity)!=expected:
            raise RuntimeError("Recompiled original enum helper changed more than its synthetic flag")
        frozen_rebuilt=hashes(rebuilt)
        (work/"before-enum-restoration.class").open("xb").write(marker_before)
        restoration=change_method_flags(marker,b"()[Lfixture/Marker;",0x000A,0x100A,name=b"$values")
        marker_after=marker.read_bytes()
        if (method_code(marker_after)!=code or len(marker_before)!=len(marker_after)
                or sum(a!=b for a,b in zip(marker_before,marker_after))!=1
                or {n for n,h in frozen_rebuilt.items() if hashes(rebuilt)[n]!=h}!={"fixture/Marker.class"}):
            raise RuntimeError("Enum header restoration changed Code or another declaration")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        if set(actual_methods)!=methods|helpers|compiler_helpers:
            raise RuntimeError("Scoped static field super lost an original runtime entry")
        (work/"enum-compiler-boundary.json").open("x").write(json.dumps({"binding":bindings,
            "generated_methods":sorted(compiler_helpers),"original_helper":identity,
            "synthetic_restoration":restoration,"original_Code_and_other_classfiles_unchanged":True},indent=2)+"\n")
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original scoped static field super class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original scoped static field super method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Scoped static field super recovery changed static field reads, original input identity, constructor effects, partial writes or exact failures")
        original_signature=signature.encode()
        negatives=(
            ('missing-nested-formal',original_signature.replace(b'Reference<TK;>',b'Reference<TMissing;>')),
            ('changed-nested-formal',original_signature.replace(b'Reference<TK;>',b'Reference<TV;>')),
            ('variant-nested-input',original_signature.replace(b'Reference<TK;>',b'Reference<+TK;>')),
            ('duplicated-input-slot',original_signature.replace(b';TV;>',b';TK;>')),
            ('changed-context-erasure',original_signature.replace(b'Lfixture/Marker;',b'Ljava/lang/Object;')),
            ('extra-method-formal',b'<Extra:Ljava/lang/Object;>'+original_signature),
        )
        for label,changed in negatives:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            if original_signature==changed:raise RuntimeError("Signature control did not change metadata: "+label)
            target=negative/"fixture/Parent.class";code_before=method_code(target.read_bytes())
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
            if result.returncode==0 or rejected.exists():raise RuntimeError("Unproved scoped static field super metadata accepted: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "scoped_static_field_super_result":"matched","enum_compiler_helpers":1,"enum_original_synthetic_flag_restored":True,"all_original_entries_and_metadata":"unchanged",
            "input_identity_configuration_partial_writes_callbacks_results_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
