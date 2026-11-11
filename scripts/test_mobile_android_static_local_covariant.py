#!/usr/bin/env python3
"""Verify original static local state and both covariant runtime entries."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib
from copy import deepcopy
import shutil
from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--kotlin-constructor-scope",action="store_true")
    parser.add_argument("--kotlin-lib",type=Path)
    parser.add_argument("--kotlin-java-home",type=Path)
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-local-covariant-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"static_local_covariant/java"
        harness_name="StaticLocalCovariant"
        originals,harness=work/"originals",work/"harness"
        dependencies=[];compiler=None;compiler_hashes={}
        if args.kotlin_constructor_scope:
            if not args.kotlin_lib or not args.kotlin_java_home:raise RuntimeError("Original Kotlin toolchain unavailable")
            kotlin=args.kotlin_lib.resolve();java_classes=work/"original-java"
            verify.compile_local([p for p in sorted(fixture.rglob("*.java")) if p.name not in {"Owner.java","Client.java"}],java_classes,release=8)
            for p in (kotlin/"kotlin-stdlib.jar",kotlin/"annotations-13.0.jar"):
                dest=work/p.name
                with zipfile.ZipFile(p) as before,zipfile.ZipFile(dest,"x") as after:
                    for entry in before.infolist():
                        if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):after.writestr(entry.filename,before.read(entry))
                dependencies.append(dest)
            compiler_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(kotlin.glob("*.jar"))}
            compiler=[choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(kotlin/"kotlin-stdlib.jar",java_classes))),"-jvm-target","1.8"]
            verify.run([*compiler,"-d",originals,*sorted((fixture.parent/"kotlin").rglob("*.kt"))],"kotlinc-original")
            for p in java_classes.rglob("*.class"):
                dest=originals/p.relative_to(java_classes);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
            client=work/"client";verify.compile_local([fixture/"fixture/Client.java"],client,classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
            shutil.copy2(client/"fixture/Client.class",originals/"fixture/Client.class")
        else:verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")],
            harness,classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=7 or len(all_methods)!=(12 if args.kotlin_constructor_scope else 13):raise RuntimeError("Original static local covariance inventory changed")
        declarations={"Lfixture/Api;","Lfixture/Item;","Lfixture/Narrow;"}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records=({r["name"]:r for p in sorted(originals.rglob("*.class")) for r in [KotlinFixtureClassFile(p.read_bytes()).facts()]} if args.kotlin_constructor_scope else class_identity.compiler_classes(originals))
        exceptions=exception_inventory(originals)
        anonymous="Lfixture/Owner$create$1;" if args.kotlin_constructor_scope else "Lfixture/Owner$1;"
        bridge=anonymous+"->get(I)Lfixture/Item;"
        target=anonymous+"->get(I)Lfixture/Narrow;"
        if bridge not in methods or target not in methods:raise RuntimeError("Missing original covariant slots")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Api","Item","Narrow"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:dependencies.append(dependency)
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        for p in dependencies:classpath += ["--classpath",p]
        def execute(runtime,label):
            outputs=[]
            for route in range(6):
                for mode in range(8):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),harness_name+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original static local covariance matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original static local covariance DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original static local covariance entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        expected=deepcopy(records)
        if args.kotlin_constructor_scope:
            annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
            if "VISIBILITY_BUILD" in annotations or Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations))!=Counter({"Lkotlin/Metadata;":2}):raise RuntimeError("Kotlin original annotation retention changed")
            dropped={}
            for c in classes:
                for identity,row in [(c,expected[c]),*expected[c]["fields"].items(),*expected[c]["methods"].items()]:
                    for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                        if row.get(key):dropped.setdefault(identity,{})[key]=row[key]
                        row[key]=[]
            (work/"d8-class-annotation-loss.json").write_text(json.dumps(dropped,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in coverage["methods"]}
        if (set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"]!="partial" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Static local covariance omitted entries or changed original generic contracts")
        if rows[bridge].get("bridge_target")!=target or rows[bridge].get("projection_kind")!="generic-interface-bridge":raise RuntimeError("Missing exact original interface bridge receipt")
        bindings=coverage.get("class_source_bindings",[])
        if len(bindings)!=1 or bindings[0]["class"]!=anonymous or bindings[0]["binding_kind"]!="anonymous-to-named-local" or not bindings[0].get("metadata_differences"):raise RuntimeError("Missing original local class source binding")
        binding=bindings[0]
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=os.pathsep.join(map(str,dependencies)) if dependencies else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        after=({r["name"]:r for p in sorted(rebuilt.rglob("*.class")) for r in [KotlinFixtureClassFile(p.read_bytes()).facts()]} if args.kotlin_constructor_scope else class_identity.compiler_classes(rebuilt))
        after_exceptions=exception_inventory(rebuilt)
        candidates=[c for c,row in after.items() if (row.get("inner_class") or {}).get("name")==binding["source_name"] and row["enclosing_method"]==records[anonymous]["enclosing_method"]]
        if len(candidates)!=1:raise RuntimeError("Reported source local class is not unique")
        renamed=candidates[0]
        def mapped(s):return s.replace(anonymous,renamed)
        helpers={mapped(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={mapped(c)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Api;","Lfixture/Item;"}}
        if args.kotlin_constructor_scope:
            expected_helpers.add("Lfixture/Owner;-><init>()V")
            if [r.get("kind") for r in coverage.get("generated_source_helpers",[]) if r["class"]=="Lfixture/Owner;" and r["name"]=="<init>"]!=["default-constructor"]:raise RuntimeError("Missing exact default constructor helper receipt")
        if actual_classes!={mapped(c) for c in classes} or set(actual_methods)!={mapped(m) for m in methods}|expected_helpers or helpers!=expected_helpers:raise RuntimeError("Static local recovery invented or omitted a runtime entry")
        for c in classes:
            for key in ("access","signature","superclass","interfaces","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                wanted=expected[c].get(key)
                if c==anonymous and key=="access":
                    if not any("class and InnerClass flags" in d for d in binding["metadata_differences"]):raise RuntimeError("Missing original local access flag receipt")
                    wanted &= ~0x000f
                if wanted!=after[mapped(c)].get(key):raise RuntimeError("Undisclosed local class metadata change: "+c+":"+key)
            import copy
            inner=copy.deepcopy(expected[c]["inner_class"])
            if c==anonymous:
                inner["inner"],inner["name"]=renamed,binding["source_name"]
                inner["access"] &= ~0x000f
            if after[mapped(c)]["inner_class"]!=inner:raise RuntimeError("Undisclosed static local lexical metadata change")
            expected_fields={mapped(i):dict(r) for i,r in expected[c]["fields"].items()}
            for r in expected_fields.values():
                if r["access"]&0x1000:
                    if not any("explicit captured fields" in d for d in binding["metadata_differences"]):raise RuntimeError("Missing original capture flag receipt")
                    r["access"] &= ~0x1000
            if after[mapped(c)]["fields"]!=expected_fields:raise RuntimeError("Undisclosed local state/capture field change: "+c)
            for identity,metadata in expected[c]["methods"].items():
                wanted=deepcopy(metadata)
                if c==anonymous and wanted["name"]=="<init>" and wanted.get("signature")=="()V":
                    if not any("constructor Signature metadata" in d for d in binding["metadata_differences"]):raise RuntimeError("Missing exact original capture constructor Signature receipt")
                    wanted["signature"]=None
                if after[mapped(c)]["methods"].get(mapped(identity))!=wanted or after_exceptions[mapped(identity)]!=exceptions[identity]:raise RuntimeError("Original local method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Static local covariance changed state, bridge dispatch, identity or failures")
        mutations=(("lost-seed","Owner","new Narrow[]{seed,null}","new Narrow[]{null,null}",0),
                   ("wrong-index","Owner","return values[index];","return values[0];",2),
                   ("lost-bits","Owner","Trace.second=second;","Trace.second=0;",0),
                   ("lost-failure","Trace","if(failAt==n)throw failure;","if(false)throw failure;",6))
        if args.kotlin_constructor_scope:
            mutations=(("lost-seed","Owner","arrayOf<Narrow?>(seed,null)","arrayOf<Narrow?>(null,null)",0),("wrong-index","Owner","return values[index]","return values[0]",2),("lost-bits","Owner","Trace.second=second","Trace.second=0f",0),("lost-failure","Trace","if(failAt==n)throw failure;","if(false)throw failure;",6))
        for label,name,before,after,mode in mutations:
            is_kotlin=args.kotlin_constructor_scope and name=="Owner"
            text=((fixture.parent/"kotlin/fixture/Owner.kt") if is_kotlin else fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original static local covariance counterexample anchor changed")
            changed=work/label/(name+(".kt" if is_kotlin else ".java"));changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            if is_kotlin:verify.run([*compiler,"-classpath",os.pathsep.join(map(str,(args.kotlin_lib/"kotlin-stdlib.jar",originals))),"-d",wrong,changed],"kotlinc-"+label)
            else:verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness,*dependencies))),
                harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            marker="owned static local covariance"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect static local covariance: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}!=dependency_hashes or (args.kotlin_constructor_scope and {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(args.kotlin_lib.resolve().glob("*.jar"))}!=compiler_hashes)):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,"kotlin_constructor_scope":args.kotlin_constructor_scope,
            "original_local_scope_and_covariant_bridge":"matched","all_original_entries_and_metadata":"matched_after_disclosed_local_name_and_capture_flags",
            "state_identity_primitive_bits_index_errors_and_failure_order":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
