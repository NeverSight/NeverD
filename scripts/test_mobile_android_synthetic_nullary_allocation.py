#!/usr/bin/env python3
"""Verify synthetic allocations and scoped same-owner constructor delegation."""
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
    from scripts.test_mobile_android_empty_package_metadata import class_access_offset
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code
    from test_mobile_android_empty_package_metadata import class_access_offset
    from test_mobile_android_protected_array_callbacks import change_method_flags


def mark_synthetic(path: Path, descriptor: bytes = b"()V") -> dict:
    before=path.read_bytes();offset=class_access_offset(before)
    if int.from_bytes(before[offset:offset+2],"big")!=0x31:raise RuntimeError("Owned adapter class flags changed")
    path.write_bytes(before[:offset]+(0x1031).to_bytes(2,"big")+before[offset+2:])
    receipt=change_method_flags(path,descriptor,1,0x1001,name=b"<init>")
    after=path.read_bytes()
    if (method_code(before)!=method_code(after) or len(before)!=len(after)
            or sum(a!=b for a,b in zip(before,after))!=2):
        raise RuntimeError("Synthetic header edit changed Code or other metadata")
    return {"class":"Lfixture/Adapter;","original_flags":0x31,"changed_flags":0x1031,"constructor":receipt,"Code_and_all_other_bytes_unchanged":True}

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--covariant-maker",action="store_true")
    parser.add_argument("--primitive-capture",action="store_true")
    args=parser.parse_args()
    covariant=args.covariant_maker or args.primitive_capture
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-synthetic-nullary-allocation-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"synthetic_nullary_allocation/java"
        if covariant:
            selected=work/"fixture-source";shutil.copytree(fixture,selected)
            variant="primitive_capture" if args.primitive_capture else "covariant_maker"
            for original in (fixture.parent/variant/"fixture").glob("*.java"):
                shutil.copyfile(original,selected/"fixture"/original.name)
            fixture=selected
        harness_prefix="SyntheticPrimitiveAllocation" if args.primitive_capture else "SyntheticNullaryAllocation"
        constructor_descriptor=b"(Z)V" if args.primitive_capture else b"()V"
        constructor_identity="Lfixture/Adapter;-><init>"+constructor_descriptor.decode()
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        declarations_for_compiler=work/"compiler-declarations";shutil.copytree(originals,declarations_for_compiler)
        compiler_hashes=hashes(declarations_for_compiler)
        (work/"before-input-synthetic.class").open("xb").write((originals/"fixture/Adapter.class").read_bytes())
        input_roles=mark_synthetic(originals/"fixture/Adapter.class",constructor_descriptor)
        (work/"input-synthetic-roles.json").open("x").write(json.dumps(input_roles,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=5 or len(all_methods)!=9:raise RuntimeError("Original synthetic nullary allocation inventory changed")
        declarations={"Lfixture/Maker;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        signature="(FLjava/util/Map<+TK;+Ljava/util/Collection<TV;>;>;Lfixture/Maker<Ljava/util/Collection<TV;>;>;)V"
        if covariant:signature=signature.replace("Lfixture/Maker<", "Lfixture/Maker<+")
        identity="Lfixture/Core;-><init>(FLjava/util/Map;Lfixture/Maker;)V"
        if records["Lfixture/Core;"]["methods"][identity]["signature"]!=signature:
            raise RuntimeError("Original synthetic allocation target Signature changed")
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
                            raise RuntimeError("Original synthetic nullary allocation matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(
            ('changed-float','Core.java','this(factor,source,','this(factor+1,source,',0,0),
            ('changed-map','Core.java','this(factor,source,','this(factor,new java.util.HashMap<K,Collection<V>>(),',1,0),
            ('lost-allocation-action','Adapter.java','Trace.allocateAction.run();','',0,0),
            ('changed-callback-result','Adapter.java','return result;','return Trace.result;',1,0),
            ('duplicate-target-action','Core.java','Trace.targetAction.run();','Trace.targetAction.run();Trace.targetAction.run();',0,0),
            ('replaced-allocation-failure','Adapter.java','Trace.allocateAction.run();','try {Trace.allocateAction.run();} catch(RuntimeException failure){throw new IllegalArgumentException(failure);}',0,4),
        )
        if args.primitive_capture:
            mutations+=(
                ('changed-capture-input','Core.java','new Adapter(flag)','new Adapter(!flag)',0,0),
                ('reversed-capture-store','Adapter.java','this.flag=flag;','this.flag=!flag;',1,0),
            )
        for label,filename,before,after,operation,mode in mutations:
            original=(fixture/"fixture"/filename).read_text()
            if before not in original:raise RuntimeError("Original synthetic nullary allocation counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=declarations_for_compiler,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1",str(operation),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or ("owned synthetic primitive allocation" if args.primitive_capture else "owned synthetic nullary allocation") not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect synthetic nullary allocation behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original synthetic nullary allocation DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original synthetic nullary allocation entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count")!=1 or coverage["status"]!="partial"
                or {r["identity"]:r.get("projection_kind") for r in coverage["methods"] if r["status"]=="source-projected"}!={constructor_identity:"synthetic-constructor"}
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Synthetic nullary allocation recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        adapter=rebuilt/"fixture/Adapter.class";adapter_before=adapter.read_bytes();before_hashes=hashes(rebuilt)
        (work/"before-synthetic-restoration.class").open("xb").write(adapter_before)
        restoration=mark_synthetic(adapter,constructor_descriptor)
        if {n for n,h in before_hashes.items() if hashes(rebuilt)[n]!=h}!={"fixture/Adapter.class"}:
            raise RuntimeError("Synthetic header restoration changed another classfile")
        (work/"synthetic-role-restoration.json").open("x").write(json.dumps(restoration,indent=2)+"\n")
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=expected_helpers:
            raise RuntimeError("Synthetic nullary allocation recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original synthetic nullary allocation class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original synthetic nullary allocation method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Synthetic nullary allocation recovery changed original allocation, nested inputs, selected constructor, opaque results or exact failures")
        original_signature=signature.encode()
        maker_open=b'Maker<'+(b'+' if covariant else b'')
        negatives=(
            ('missing-map-formal',original_signature.replace(b'Map<+TK;',b'Map<+TMissing;')),
            ('changed-map-variance',original_signature.replace(b'Map<+TK;',b'Map<-TK;')),
            ('missing-maker-element',original_signature.replace(maker_open+b'Ljava/util/Collection<TV;>;>',maker_open+b'Ljava/util/Collection<TMissing;>;>')),
            ('contravariant-maker',original_signature.replace(maker_open+b'Ljava/util/Collection',b'Maker<-Ljava/util/Collection')),
            ('changed-float-erasure',original_signature.replace(b'(F',b'(I')),
            ('extra-method-formal',b'<Extra:Ljava/lang/Object;>'+original_signature),
        )
        for label,changed in negatives:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            if original_signature==changed:raise RuntimeError("Signature control did not change metadata: "+label)
            target=negative/"fixture/Core.class";code_before=method_code(target.read_bytes())
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
            if result.returncode==0 or rejected.exists():raise RuntimeError("Unproved synthetic nullary allocation metadata accepted: "+label)
        # A different already-declared owner formal is a valid independent
        # callback view. The raw allocated object supplies no K/V correspondence.
        positive_signature=original_signature.replace(maker_open+b'Ljava/util/Collection<TV;>;>',maker_open+b'Ljava/util/Collection<TK;>;>')
        positive_originals=work/"separate-maker-formal-positive-originals";shutil.copytree(originals,positive_originals)
        positive_receipt=change_signature(positive_originals/"fixture/Core.class",original_signature,positive_signature)
        for original in originals.rglob("*.class"):
            if method_code(original.read_bytes())!=method_code((positive_originals/original.relative_to(originals)).read_bytes()):
                raise RuntimeError("Allowed scoped control changed original Code")
        positive_dex=work/"separate-maker-formal-positive-dex";positive_dex.mkdir()
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",positive_dex,
            *[positive_originals/path.relative_to(originals) for path in paths]],"positive-d8")
        positive_output=work/"separate-maker-formal-positive-recovered"
        verify.run([args.neverd.resolve(),"mobile",positive_dex/"classes.dex",*classpath,"-o",positive_output],"positive-recover")
        positive_coverage=json.loads((positive_output/"report.json").read_text())["android_method_recovery"]
        if (positive_coverage["class_count"]!=len(classes) or positive_coverage["method_count"]!=len(methods)
                or {row["identity"] for row in positive_coverage["methods"]}!=methods
                or positive_coverage["unrecovered_method_count"] or positive_coverage["projected_method_count"]!=1
                or positive_coverage.get("generic_signature_projections") or positive_coverage.get("class_source_bindings")
                or {row["identity"]:row.get("projection_kind") for row in positive_coverage["methods"] if row["status"]=="source-projected"}
                    !={constructor_identity:"synthetic-constructor"}):
            raise RuntimeError("Allowed scoped control changed original recovery roles")
        positive_rebuilt=work/"separate-maker-formal-positive-rebuilt"
        verify.compile_local(sorted((positive_output/"sources").rglob("*.java")),positive_rebuilt,
            classpath=dependency if declarations else None,release=positive_coverage.get("minimum_java_release",8))
        positive_adapter=positive_rebuilt/"fixture/Adapter.class"
        (work/"before-positive-synthetic-restoration.class").open("xb").write(positive_adapter.read_bytes())
        positive_restoration=mark_synthetic(positive_adapter,constructor_descriptor)
        pc,pm=compiler_inventory(verify,positive_rebuilt)
        ph={row["class"]+"->"+row["name"]+row["prototype"] for row in positive_coverage.get("generated_source_helpers",[])}
        if pc!=classes or set(pm)!=methods|ph or not ph<=expected_helpers:
            raise RuntimeError("Allowed scoped control omitted or invented a runtime entry")
        expected_records=class_identity.compiler_classes(positive_originals)
        actual_records=class_identity.compiler_classes(positive_rebuilt)
        actual_exceptions=exception_inventory(positive_rebuilt)
        for owner in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if expected_records[owner].get(key)!=actual_records[owner].get(key):
                    raise RuntimeError("Allowed scoped control changed original class metadata")
            for method,metadata in expected_records[owner]["methods"].items():
                if actual_records[owner]["methods"].get(method)!=metadata or actual_exceptions[method]!=exceptions[method]:
                    raise RuntimeError("Allowed scoped control changed original method metadata or Exceptions")
        positive_runtime=(positive_rebuilt,harness,dependency) if declarations else (positive_rebuilt,harness)
        positive_outputs=execute(positive_runtime,"positive-rebuilt")
        if positive_outputs!=baseline:
            raise RuntimeError("Allowed independent owner formal changed runtime effects or identities")
        (work/"separate-maker-formal-positive-proof.json").open("x").write(json.dumps({
            "status":"verified","signature":positive_signature.decode(),"encoding":positive_receipt,
            "synthetic_restoration":positive_restoration,"original_Code_unchanged":True,
            "compiled_original_entries_and_metadata":"unchanged","fresh_jvm_cases":len(positive_outputs),"attempts_per_case":2},indent=2)+"\n")
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashes(declarations_for_compiler)!=compiler_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,"covariant_maker":covariant,"primitive_capture":args.primitive_capture,"constructor_descriptor":constructor_descriptor.decode(),
            "synthetic_nullary_allocation_result":"matched","synthetic_class_and_constructor_roles_restored":True,"accepted_independent_owner_formal_jvm_cases":len(positive_outputs),"all_original_entries_and_metadata":"unchanged",
            "input_identity_configuration_partial_writes_callbacks_results_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
