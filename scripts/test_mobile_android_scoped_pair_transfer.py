#!/usr/bin/env python3
"""Verify covariant binary input retains both invariant result scopes."""
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
    parser.add_argument("--private-transfer",action="store_true")
    parser.add_argument("--dependency-carrier",action="store_true")
    args=parser.parse_args()
    if args.private_transfer and args.dependency_provider:parser.error("Private implementation recovery requires its original body")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-pair-transfer-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/("private_pair_transfer/java" if args.private_transfer else "scoped_pair_transfer/java")
        harness_prefix="PrivatePairTransfer" if args.private_transfer else "ScopedPairTransfer"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=4 or len(all_methods)!=(12 if args.private_transfer else 11):raise RuntimeError("Original scoped pair transfer inventory changed")
        declarations=({"Lfixture/Provider;"} if args.dependency_provider else set()) | ({"Lfixture/Pair;"} if args.dependency_carrier else set())
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Provider;->make(Lfixture/Pair;)Ljava/util/Map;"
        signature=("<K:Ljava/lang/Object;V:Ljava/lang/Object;>(Lfixture/Pair<TK;+TV;>;)Ljava/util/Map<TK;TV;>;" if args.private_transfer else "<K:Ljava/lang/Object;V:Ljava/lang/Object;>(Lfixture/Pair<+TK;+TV;>;)Ljava/util/Map<TK;TV;>;")
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete input/result pair slot relation changed")
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
                for operation in range(3):
                    for mode in range(16):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_prefix+"Harness",
                            str(route),str(operation),str(mode)],f"{label}-{route}-{operation}-{mode}")
                        pattern=f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original flat pair matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(("swapped-getters","Trace.first=pair.first();","Trace.first=pair.second();",0),
                   ("replaced-result","return (Map<K,V>)Trace.result;","return new java.util.HashMap<K,V>();",0),
                   ("early-effect","Trace.hit(1);","Trace.hit(6);Trace.hit(1);",7))
        original=(fixture/"fixture/Provider.java").read_text()
        for label,before,after,mode in mutations:
            if original.count(before)!=1:raise RuntimeError("Original scoped pair transfer counterexample anchor changed")
            changed=work/label/"Provider.java";changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1","0",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned scoped pair transfer" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect provider behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original scoped pair transfer DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original scoped pair transfer entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Flat pair transfer recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Flat pair transfer recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original scoped pair transfer class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original scoped pair transfer method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Flat pair transfer recovery changed getter effects, partial writes, actual result identity or late checked casts")
        if True:
            original_signature=signature.encode()
            negatives=(
                ("swapped-input",original_signature.replace(b"Pair<+TK;+TV;>",b"Pair<+TV;+TK;>")),
                ("repeated-input",original_signature.replace(b"Pair<+TK;+TV;>",b"Pair<+TK;+TK;>")),
                ("contravariant-key",original_signature.replace(b"Pair<+TK;+TV;>",b"Pair<-TK;+TV;>")),
                ("variant-result",original_signature.replace(b"Map<TK;TV;>",b"Map<TK;+TV;>")),
                ("extra-formal",original_signature.replace(b"V:Ljava/lang/Object;>",b"V:Ljava/lang/Object;U:Ljava/lang/Object;>")),
                ("lost-result-slot",original_signature.replace(b"Map<TK;TV;>",b"Map<TK;Ljava/lang/Object;>")),
            )
            for label,changed in negatives:
                negative=work/(label+"-originals");shutil.copytree(originals,negative)
                if original_signature==changed:raise RuntimeError("Signature control did not change metadata: "+label)
                target=negative/"fixture/Provider.class";code_before=method_code(target.read_bytes())
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
                if result.returncode==0 or rejected.exists():raise RuntimeError("Unproved private pair relation accepted: "+label)
        if args.private_transfer:
            wrongs=(
                ("bypass-private","return make(pair);","return (Map)Trace.result;",0),
                ("duplicate-private","return make(pair);","make(pair);return make(pair);",0),
                ("copied-private-input","return make(pair);","return make(pair==null?null:new Pair(pair.first,pair.second));",0),
                ("lost-second-snapshot","Trace.second=pair.second();Trace.hit(3);","Trace.hit(3);",0),
                ("replaced-private-throw","return make(pair);","try{return make(pair);}catch(RuntimeException problem){throw new IllegalArgumentException(problem);}",1),
            )
            for label,before,after,mode in wrongs:
                if original.count(before)!=1:raise RuntimeError("Private pair wrong-program anchor changed")
                changed=work/label/"Provider.java";changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
                wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
                result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_prefix+"Harness","1","0",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
                (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
                if result.returncode==0 or "owned scoped pair transfer" not in result.stderr:raise RuntimeError("Private observer missed "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "covariant_input_and_invariant_result_full_slots":"matched","all_original_entries_and_metadata":"unchanged",
            "getter_mutations_nulls_partial_writes_actual_map_identity_late_casts_and_failures":"matched",
            "independent_wrong_programs":len(mutations)+(5 if args.private_transfer else 0),"private_transfer":args.private_transfer,"dependency_carrier":args.dependency_carrier,"same_code_signature_mutations":6,"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
