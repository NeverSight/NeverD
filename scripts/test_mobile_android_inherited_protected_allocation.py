#!/usr/bin/env python3
"""Verify cross-package inherited protected calls preserve nested allocation and dispatch."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-protected-allocation-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"inherited_protected_allocation/java"
        harness_prefix="InheritedProtectedAllocation"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=3 or len(all_methods)!=6:raise RuntimeError("Original inherited protected allocation inventory changed")
        declarations={"Lbase/Parent;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        signature="<T:Ljava/lang/Object;>Ljava/lang/Object;Ljava/io/Serializable;"
        child_signature="Lbase/Parent<Ljava/net/URI;>;"
        if records["Lbase/Parent;"]["signature"]!=signature or records["Lfixture/Child;"]["signature"]!=child_signature:
            raise RuntimeError("Original protected inheritance Signature changed")
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
                            raise RuntimeError("Original inherited protected allocation matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(
            ('lost-dispatch','fixture/Child.java','this.text(value)','super.text(value)',1,0),
            ('lost-input','base/Parent.java','Trace.value=value;','Trace.value=null;',0,0),
            ('lost-result','base/Parent.java','return Trace.result;','return "wrong:result";',0,0),
            ('lost-action','base/Parent.java','Trace.action.run();',';',0,0),
            ('replaced-failure','base/Parent.java','Trace.action.run();','try {Trace.action.run();} catch(RuntimeException failure) {throw new IllegalArgumentException(failure);}',0,4),
            ('replaced-constructor','fixture/Child.java','new URI(this.text(value))','URI.create(this.text(value))',0,8),
        )
        for label,filename,before,after,operation,mode in mutations:
            original=(fixture/filename).read_text()
            if original.count(before)!=1:raise RuntimeError("Original inherited protected allocation counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir(parents=True);changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1",str(operation),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned inherited protected allocation" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect inherited protected allocation behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original inherited protected allocation DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original inherited protected allocation entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Inherited protected allocation recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=expected_helpers:
            raise RuntimeError("Inherited protected allocation recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original inherited protected allocation class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original inherited protected allocation method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Inherited protected allocation recovery changed input identities, configuration, constructor results or effects")
        parent_sig,child_sig=signature.encode(),child_signature.encode()
        negatives=(
            ("missing-bound","base/Parent.class",parent_sig,parent_sig.replace(b"Ljava/lang/Object;>",b"TMissing;>")),
            ("wrong-bound","base/Parent.class",parent_sig,parent_sig.replace(b"T:Ljava/lang/Object;",b"T:Ljava/lang/Number;")),
            ("raw-parent","fixture/Child.class",child_sig,b"Lbase/Parent;"),
            ("variant-parent","fixture/Child.class",child_sig,child_sig.replace(b"<Ljava/net/URI;>",b"<+Ljava/net/URI;>")),
            ("wrong-parent-arity","fixture/Child.class",child_sig,child_sig.replace(b"<Ljava/net/URI;>",b"<Ljava/net/URI;Ljava/lang/Object;>")),
            ("missing-edge-formal","fixture/Child.class",child_sig,b"Lbase/Parent<TMissing;>;"),
        )
        for label,relative,original_signature,changed in negatives:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            if original_signature==changed:raise RuntimeError("Signature control did not change metadata: "+label)
            target=negative/relative;code_before=method_code(target.read_bytes())
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
            if result.returncode==0 or rejected.exists():raise RuntimeError("Unproved inherited protected allocation metadata accepted: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "inherited_protected_allocation_result":"matched","all_original_entries_and_metadata":"unchanged",
            "input_identity_configuration_partial_writes_callbacks_results_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
