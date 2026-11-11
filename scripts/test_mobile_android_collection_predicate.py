#!/usr/bin/env python3
"""Verify invariant collection and complete variant predicate contracts."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--covariant-values",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-collection-predicate-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"collection_predicate/java"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("CollectionPredicateCaller","CollectionPredicateHarness")]
        if args.covariant_values:
            copied=work/"covariant-java";shutil.copytree(fixture,copied);fixture=copied
            provider=fixture/"fixture/Provider.java";text=provider.read_text()
            for before,after in (("List<T> values","List<? extends T> values"),
                                 ("Iterator<T> cursor","Iterator<? extends T> cursor")):
                if text.count(before)!=1:raise RuntimeError("Original covariant collection source anchor changed")
                text=text.replace(before,after)
            provider.write_text(text)
            text=harness_sources[1].read_text()
            before="list.getActualTypeArguments()[0].equals(formal)"
            if text.count(before)!=1:raise RuntimeError("Original covariance metadata observer changed")
            text=text.replace(before,"((WildcardType)list.getActualTypeArguments()[0]).getUpperBounds()[0].equals(formal)")
            observer=work/"CollectionPredicateHarness.java";observer.open("x").write(text);harness_sources[1]=observer
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=4 or len(all_methods)!=7:raise RuntimeError("Original collection predicate inventory changed")
        classes=all_classes-({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Provider;->remove(Ljava/util/List;Lfixture/Predicate;)Z"
        signature="<T:Ljava/lang/Object;>(Ljava/util/List<TT;>;Lfixture/Predicate<-TT;+Ljava/lang/Boolean;>;)Z"
        if args.covariant_values:signature=signature.replace("List<TT;>","List<+TT;>")
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete list/predicate binding relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Provider.class",(originals/"fixture/Provider.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(16):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"CollectionPredicateHarness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=-?[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original collection predicate matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original collection predicate DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original collection predicate entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Collection predicate recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Predicate;"}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Collection predicate recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original collection predicate class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original collection predicate method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Collection predicate recovery changed predicate order, removals, partial effects, returned boolean or failures")
        mutations=(("ignored-predicate","predicate.apply(value).booleanValue()","Boolean.TRUE.booleanValue()",0),
                   ("missing-removal","cursor.remove();","changed=true;",0),
                   ("wrong-result","return changed;","return false;",0))
        original=(fixture/"fixture/Provider.java").read_text()
        for label,before,after,mode in mutations:
            if original.count(before)!=1:raise RuntimeError("Original collection predicate counterexample anchor changed")
            changed=work/label/"Provider.java";changed.parent.mkdir();changed.write_text(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                "CollectionPredicateHarness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned collection predicate" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect provider behavior: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "covariant_values":args.covariant_values,
            "scoped_list_variant_predicate_and_closed_boolean":"matched","all_original_entries_and_metadata":"unchanged",
            "mutable_list_predicate_identity_nulls_partial_removals_boolean_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
