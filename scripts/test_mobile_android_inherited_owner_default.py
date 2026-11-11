#!/usr/bin/env python3
"""Verify genuine bounded generic interface defaults with frozen compiler observers."""
from __future__ import annotations
import argparse
from collections import Counter
from contextlib import nullcontext
from copy import deepcopy
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
    from scripts.test_mobile_android_interface_default_access import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_interface_default_access import compiler_facts

OWNER="Lfixture/DefaultApi;"
DEFAULT=OWNER+"->finish(Lfixture/Vector;Lfixture/Vector;Lfixture/Vector;)Lfixture/Vector;"
ACCESS=OWNER+"->access$finish$jd(Lfixture/DefaultApi;Lfixture/Vector;Lfixture/Vector;Lfixture/Vector;)Lfixture/Vector;"
SIGNATURE="(TV;TV;TV;)TV;"

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-contracts",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-owner-default-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"inherited_owner_default";kotlin=args.kotlin_lib.resolve()
        inputs=[args.android_jar.resolve(),*sorted(kotlin.glob("*.jar"))]
        if not (kotlin/"kotlin-compiler.jar").is_file():raise RuntimeError("Original Kotlin compiler unavailable")
        input_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        dependencies=[]
        for p in (kotlin/"kotlin-stdlib.jar",kotlin/"annotations-13.0.jar"):
            dest=work/p.name
            with zipfile.ZipFile(p) as before,zipfile.ZipFile(dest,"x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe compiler dependency")
                        after.writestr(entry.filename,before.read(entry))
            dependencies.append(dest)
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        original_java,originals,harness=work/"original-java",work/"originals",work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),original_java,release=8);originals.mkdir()
        compiler=[choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect",
                  "-classpath",os.pathsep.join(map(str,(kotlin/"kotlin-stdlib.jar",original_java))),"-jvm-target","1.8","-Xjvm-default=all-compatibility"]
        verify.run([*compiler,"-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        for p in original_java.rglob("*.class"):
            dest=originals/p.relative_to(original_java);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in ("InheritedOwnerDefaultHarness","InheritedOwnerDefaultCaller")],harness,
            classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        all_classes,all_methods=compiler_inventory(verify,originals);all_methods=set(all_methods)
        records,exceptions=compiler_facts(originals),exception_inventory(originals)
        if len(all_classes)!=10 or len(all_methods)!=17 or records[OWNER]["methods"][DEFAULT]["signature"]!=SIGNATURE:
            raise RuntimeError("Original inherited owner default inventory or Signature changed")
        borrowed={"Lfixture/Vector;"} if args.dependency_contracts else set()
        classes=all_classes-borrowed;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        if borrowed:
            dependency=work/"owned-contracts.jar"
            with zipfile.ZipFile(dependency,"x") as jar:
                for c in sorted(borrowed):jar.writestr(c[1:-1]+".class",(originals/(c[1:-1]+".class")).read_bytes())
            dependencies.append(dependency)
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        def execute(runtime,name,label):
            outputs=[]
            for route in range(4):
                for variant in range(2):
                    for mode in range(11):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),"InheritedOwnerDefaultHarness",
                            str(route),str(variant),str(mode),name],f"{label}-{route}-{variant}-{mode}")
                        pattern=f"route={route},variant={variant},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError("Original inherited owner default matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"finish","baseline")
        classpath=["--classpath",args.android_jar.resolve()]
        for p in dependencies:classpath += ["--classpath",p]
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original inherited owner default DEX count changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original inherited owner default entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
        visible=Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations))
        if "VISIBILITY_BUILD" in annotations or visible!=Counter({"Lkotlin/Metadata;":8,"Ljava/lang/Deprecated;":3}):raise RuntimeError("D8 changed inherited owner default annotation retention")
        expected={c:deepcopy(records[c]) for c in classes};dropped={}
        for c in classes:
            for identity,row in [(c,expected[c]),*expected[c]["fields"].items(),*expected[c]["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    if row.get(key):dropped.setdefault(identity,{})[key]=row[key]
                    row[key]=[]
        (work/"d8-class-annotation-loss.json").write_text(json.dumps(dropped,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if (set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"]!="partial" or rows[DEFAULT].get("projection_kind")!="interface-default-body" or rows[ACCESS].get("projection_kind")!="static-interface-default-accessor"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):raise RuntimeError("Inherited owner default recovery changed original entries or contract")
        inherited={c+"->access$finish$jd("+c+"Lfixture/Vector;Lfixture/Vector;Lfixture/Vector;)Lfixture/Vector;" for c in ("Lfixture/ChildApi;","Lfixture/GrandApi;")}
        if any(rows[i].get("projection_kind")!="inherited-interface-default-accessor" or rows[i].get("ancestor_accessor")!=ACCESS or not rows[i].get("original_dispatch") or not rows[i].get("metadata_differences") for i in inherited):raise RuntimeError("Missing original inherited default accessor receipts")
        helper=rows[DEFAULT]["default_body_helper"];name=helper.split("->",1)[1].split("(",1)[0]
        if rows[ACCESS].get("default_body_helper")!=helper:raise RuntimeError("Inherited owner default and accessor duplicated original body")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)),release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helper_rows={r["class"]+"->"+r["name"]+r["prototype"]:r for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        expected_helpers |= {helper,*{c+"-><init>()V" for c in ("Lfixture/DefaultApi$DefaultImpls;","Lfixture/ChildApi$DefaultImpls;","Lfixture/GrandApi$DefaultImpls;")}}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or set(helper_rows)!=expected_helpers:raise RuntimeError("Inherited owner default recovery invented or omitted an entry/helper")
        if helper_rows[helper].get("generic_signature")!=SIGNATURE:raise RuntimeError("Missing helper's complete generic Signature receipt")
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        if after[OWNER]["methods"][helper]["signature"]!=SIGNATURE:raise RuntimeError("Private default helper lost generic Signature")
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if after[c].get(key)!=expected[c].get(key):raise RuntimeError("Undisclosed inherited owner default class metadata changed: "+c+":"+key)
            for identity,metadata in expected[c]["methods"].items():
                wanted=deepcopy(metadata)
                if identity==ACCESS or identity in inherited:wanted["access"] &= ~0x1000
                if after[c]["methods"].get(identity)!=wanted or after_exceptions.get(identity)!=exceptions[identity]:raise RuntimeError("Undisclosed inherited owner default method metadata or Exceptions changed: "+identity)
        if execute((rebuilt,harness),name,"rebuilt")!=baseline:raise RuntimeError("Inherited owner default changed dispatch, input/result identity, effects, wide values or failures")
        source=(fixture/"kotlin/fixture/DefaultApi.kt").read_text()
        for label,old,new in (("wrong-input","Trace.third = third","Trace.third = second"),("lost-result","return third","return first"),("lost-wide","return Trace.wide","return Trace.wide.toInt().toLong()")):
            if source.count(old)!=1:raise RuntimeError("Bounded default counterexample anchor changed")
            wrong=work/label/"DefaultApi.kt";wrong.parent.mkdir();wrong.write_text(source.replace(old,new))
            variant=work/(label+"-classes");variant.mkdir();verify.run([*compiler,"-d",variant,wrong],"kotlinc-"+label)
            mode="0"
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),"InheritedOwnerDefaultHarness","1","1",mode,"finish"],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned inherited owner default" not in result.stderr:raise RuntimeError("Frozen bounded default observer missed "+label)
        wrong=work/"virtual-dispatch/InheritedOwnerDefaultCaller.java";wrong.parent.mkdir()
        source=(FIXTURES/"harness/InheritedOwnerDefaultCaller.java").read_text();old="return GrandApi.DefaultImpls.finish(receiver,a,b,c);"
        if source.count(old)!=1:raise RuntimeError("Frozen bounded default caller anchor changed")
        wrong.write_text(source.replace(old,"return receiver.finish(a,b,c);"));variant=work/"virtual-dispatch-classes"
        verify.compile_local([wrong],variant,classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),"InheritedOwnerDefaultHarness","1","1","0","finish"],text=True,capture_output=True,timeout=600,env=verify.environment)
        (work/"virtual-dispatch-counterexample.log").write_text(result.stdout+result.stderr)
        if result.returncode==0 or "owned inherited owner default" not in result.stderr:raise RuntimeError("Frozen bounded default observer missed virtual dispatch")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}!=input_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}!=dependency_hashes:raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases_per_stage":len(baseline),"stages":2,"attempts_per_case":2,
            "default_and_virtual_dispatch":"matched","original_bounded_owner_input_and_result_Signatures":"matched","original_receiver_three_inputs_result_wide_and_exceptions":"matched",
            "original_code_bytes":"compiler output unchanged","independent_counterexamples":4,"compiler_CLASS_annotation_loss_sites":len(dropped),"dependency_contracts":args.dependency_contracts,"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
