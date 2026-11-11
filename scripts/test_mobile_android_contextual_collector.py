#!/usr/bin/env python3
"""Verify contextual collector factories retain result-only formals and closed configuration roles."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-contextual-collector-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"contextual_collector/java"
        harness_prefix="ContextualCollector"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=3 or len(all_methods)!=9:raise RuntimeError("Original contextual collector inventory changed")
        declarations={"Lfixture/Factory;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        signature=("<T:Ljava/lang/Object;>(Ljava/lang/CharSequence;"
                   ")Ljava/util/stream/Collector<TT;*Ljava/lang/String;>;")
        full_signature=signature.replace("(Ljava/lang/CharSequence;", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;Ljava/lang/CharSequence;",1)
        short_identity="Lfixture/Factory;->make(Ljava/lang/CharSequence;)Ljava/util/stream/Collector;"
        full_identity=short_identity.replace("(Ljava/lang/CharSequence;", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;Ljava/lang/CharSequence;",1)
        for target, contract in ((short_identity, signature), (full_identity, full_signature)):
            if records["Lfixture/Factory;"]["methods"][target]["signature"]!=contract:
                raise RuntimeError("Original contextual collector Signature changed")
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
                for mode in range(24):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_prefix+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original contextual collector observer changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(
            ('lost-before','Factory.java','Trace.before.run();',';',0,0),
            ('duplicate-before','Factory.java','Trace.before.run();','Trace.before.run();Trace.before.run();',0,0),
            ('early-context','Factory.java','Trace.hit(1);','separator.length();Trace.hit(1);',0,0),
            ('lost-context','Factory.java','Trace.separatorLength = separator.length();','Trace.separatorLength = 0;',0,0),
            ('swapped-contexts','Factory.java','Trace.prefix = prefix;','Trace.prefix = suffix;',0,0),
            ('lost-action','Factory.java','Trace.action.run();',';',0,0),
            ('early-result','Factory.java','Trace.before.run();','Object saved = Trace.result;Trace.before.run();',0,19),
            ('lost-return-cast','Factory.java','return (Collector<T, ?, String>)Trace.result;','return Trace.result instanceof Collector ? (Collector<T, ?, String>)Trace.result : null;',1,16),
            ('reread-context','Factory.java','Trace.prefixLength = prefix.length();','Trace.prefixLength = Trace.prefixConfig.length();',0,20),
            ('lost-delegation','Factory.java','return make(separator, Trace.prefixConfig, Trace.suffixConfig);','return (Collector<T, ?, String>)Trace.result;',0,0),
        )
        for label,filename,before,after,route,mode in mutations:
            original=(fixture/"fixture"/filename).read_text()
            if before not in original:raise RuntimeError("Original contextual collector counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(before,after).replace("return (Collector<T, ?, String>)Trace.result;", "return (Collector<T, ?, String>)saved;") if label == "early-result" else original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness",str(route),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned contextual collector" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect contextual collector behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original contextual collector DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original contextual collector entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Contextual collector recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=expected_helpers:
            raise RuntimeError("Contextual collector recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original contextual collector class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original contextual collector method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Contextual collector recovery changed input identities, configuration, bounded results or effects")
        short=signature.encode();full=full_signature.encode()
        negatives=(
            ("short-wrong-formal-bound",short_identity,short,short.replace(b"T:Ljava/lang/Object;",b"T:Ljava/lang/Number;")),
            ("short-missing-formal",short_identity,short,short.replace(b"Collector<TT;*",b"Collector<TMissing;*")),
            ("short-bound-state",short_identity,short,short.replace(b"Collector<TT;*",b"Collector<TT;TT;")),
            ("short-open-result",short_identity,short,short.replace(b"*Ljava/lang/String;",b"*TT;")),
            ("full-variant-input-result",full_identity,full,full.replace(b"Collector<TT;*",b"Collector<+TT;*")),
            ("full-variant-closed-result",full_identity,full,full.replace(b"*Ljava/lang/String;",b"*+Ljava/lang/String;")),
            ("full-extra-formal",full_identity,full,full.replace(b"T:Ljava/lang/Object;>",b"T:Ljava/lang/Object;U:Ljava/lang/Object;>")),
            ("full-wrong-context-erasure",full_identity,full,full.replace(b"(Ljava/lang/CharSequence;",b"(Ljava/lang/Object;",1)),
        )
        for label, target_identity, original_signature, changed in negatives:
            if records["Lfixture/Factory;"]["methods"].get(target_identity,{}).get("signature")!=original_signature.decode():
                raise RuntimeError("Signature control identity does not name its original method")
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            if original_signature==changed:raise RuntimeError("Signature control did not change metadata: "+label)
            target=negative/"fixture/Factory.class";code_before=method_code(target.read_bytes())
            receipt=change_signature(target,original_signature,changed)
            receipt["target_identity"]=target_identity
            if method_code(target.read_bytes())!=code_before:raise RuntimeError("Signature control changed Code")
            negative_classpath=classpath
            if declarations:
                changed_dependency=work/(label+"-provider.jar")
                with zipfile.ZipFile(changed_dependency,"x") as jar:
                    for owner in sorted(declarations):
                        name=owner[1:-1]+".class";jar.writestr(name,(negative/name).read_bytes())
                negative_classpath=["--classpath",args.android_jar.resolve(),"--classpath",changed_dependency]
            (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
            negative_dex=work/(label+"-dex");negative_dex.mkdir()
            encoding_log=verify.run([*verify.d8,"--debug","--min-api","24",*negative_classpath,"--output",negative_dex,*[negative/path.relative_to(originals) for path in paths]],label+"-d8")
            if "Signature is ignored" in encoding_log:raise RuntimeError("D8 removed a Signature control: "+label)
            rejected=work/(label+"-rejected")
            result=subprocess.run([args.neverd.resolve(),"mobile",negative_dex/"classes.dex",*negative_classpath,"-o",rejected],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or rejected.exists():raise RuntimeError("Unproved contextual collector metadata accepted: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "contextual_collector_result":"matched","all_original_entries_and_metadata":"unchanged",
            "input_identity_configuration_partial_writes_callbacks_results_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
