#!/usr/bin/env python3
"""Verify complete generic interface defaults with frozen compiler observers."""
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_protected_array_callbacks import change_method_flags

OWNER="Lfixture/DefaultApi;"
DEFAULT=OWNER+"->work(Lfixture/Link;)Lfixture/Vector;"
ACCESS=OWNER+"->access$work$jd(Lfixture/DefaultApi;Lfixture/Link;)Lfixture/Vector;"
SIGNATURE="<V:Lfixture/Bound;>(Lfixture/Link<Ljava/lang/String;TV;>;)Lfixture/Vector<TV;>;"

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-contracts",action="store_true")
    parser.add_argument("--closed-parameters",action="store_true")
    parser.add_argument("--synthetic-default",action="store_true")
    args=parser.parse_args()
    signature=("(Lfixture/Link<Ljava/lang/String;Lfixture/Bound;>;)Lfixture/Vector<Lfixture/Bound;>;" if args.closed_parameters else SIGNATURE)
    harness_name="ClosedDefaultAccessHarness" if args.closed_parameters else "GenericDefaultAccessHarness"
    caller_name="ClosedDefaultAccessCaller" if args.closed_parameters else "GenericDefaultAccessCaller"
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-generic-default-access-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/("closed_default_access" if args.closed_parameters else "generic_default_access");kotlin=args.kotlin_lib.resolve()
        harness_fixture=fixture/"harness" if args.closed_parameters else FIXTURES/"harness"
        if args.synthetic_default:
            copied=work/"frozen-harness-source";copied.mkdir()
            for name in (harness_name,caller_name):
                source=(harness_fixture/(name+".java")).read_text()
                if name==harness_name:
                    anchor="require(entry.isDefault());"
                    if source.count(anchor)!=1:raise RuntimeError("Original frozen default flag observer changed")
                    source=source.replace(anchor,anchor+"require(entry.isSynthetic()==original);")
                (copied/(name+".java")).open("x").write(source)
            harness_fixture=copied
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
        original_java,originals,harness=work/"original-java",work/"originals",work/"harness"
        verify.compile_local(sorted((FIXTURES/"generic_default_access/java").rglob("*.java")),original_java,release=8);originals.mkdir()
        compiler=[choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect",
                  "-classpath",os.pathsep.join(map(str,(kotlin/"kotlin-stdlib.jar",original_java))),"-jvm-target","1.8","-Xjvm-default=all-compatibility"]
        verify.run([*compiler,"-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        for p in original_java.rglob("*.class"):
            dest=originals/p.relative_to(original_java);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        verify.compile_local([harness_fixture/(n+".java") for n in (harness_name,caller_name)],harness,
            classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        compiler_view=originals
        compiler_view_hashes=None
        if args.synthetic_default:
            compiler_view=work/"compiler-view";shutil.copytree(originals,compiler_view)
            compiler_view_hashes=hashes(compiler_view)
            receipt=change_method_flags(originals/"fixture/DefaultApi.class",
                b"(Lfixture/Link;)Lfixture/Vector;",0x1,0x1001,name=b"work")
            (work/"synthetic-default-witness.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        all_classes,all_methods=compiler_inventory(verify,originals);all_methods=set(all_methods)
        records,exceptions=compiler_facts(originals),exception_inventory(originals)
        if len(all_classes)!=8 or len(all_methods)!=13 or records[OWNER]["methods"][DEFAULT]["signature"]!=signature:
            raise RuntimeError("Original generic default inventory or Signature changed")
        borrowed={"Lfixture/Bound;","Lfixture/Link;","Lfixture/Vector;"} if args.dependency_contracts else set()
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
                    for mode in range(13):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),harness_name,
                            str(route),str(variant),str(mode),name],f"{label}-{route}-{variant}-{mode}")
                        pattern=f"route={route},variant={variant},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError("Original generic default matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"work","baseline")
        classpath=["--classpath",args.android_jar.resolve()]
        for p in dependencies:classpath += ["--classpath",p]
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original generic default DEX count changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original generic default entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
        visible=Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations))
        if "VISIBILITY_BUILD" in annotations or visible!=Counter({"Lkotlin/Metadata;":4,"Ljava/lang/Deprecated;":1}):raise RuntimeError("D8 changed generic default annotation retention")
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
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):raise RuntimeError("Generic default recovery changed original entries or contract")
        if args.synthetic_default and not any("synthetic access flag" in text for text in rows[DEFAULT].get("metadata_differences",[])):
            raise RuntimeError("Synthetic default flag loss was not disclosed")
        helper=rows[DEFAULT]["default_body_helper"];name=helper.split("->",1)[1].split("(",1)[0]
        if rows[ACCESS].get("default_body_helper")!=helper:raise RuntimeError("Generic default and accessor duplicated original body")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)),release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helper_rows={r["class"]+"->"+r["name"]+r["prototype"]:r for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes-{"Lfixture/Link;"}}
        expected_helpers |= {helper,OWNER.replace(";","$DefaultImpls;")+"-><init>()V"}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or set(helper_rows)!=expected_helpers:raise RuntimeError("Generic default recovery invented or omitted an entry/helper")
        if helper_rows[helper].get("generic_signature")!=signature:raise RuntimeError("Missing helper's complete generic Signature receipt")
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        if after[OWNER]["methods"][helper]["signature"]!=signature:raise RuntimeError("Private default helper lost generic Signature")
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if after[c].get(key)!=expected[c].get(key):raise RuntimeError("Undisclosed generic default class metadata changed: "+c+":"+key)
            for identity,metadata in expected[c]["methods"].items():
                wanted=deepcopy(metadata)
                if identity==ACCESS or (args.synthetic_default and identity==DEFAULT):wanted["access"] &= ~0x1000
                if after[c]["methods"].get(identity)!=wanted or after_exceptions.get(identity)!=exceptions[identity]:raise RuntimeError("Undisclosed generic default method metadata or Exceptions changed: "+identity)
        if execute((rebuilt,harness),name,"rebuilt")!=baseline:raise RuntimeError("Generic default changed dispatch, identity, effects, wide values, partial construction or failures")
        source=(fixture/"kotlin/fixture/DefaultApi.kt").read_text()
        for label,old,new in (("lost-effect","Trace.hit(4)","Trace.hit(8)"),("wrong-receiver","Vector(this, value, Trace.wide)","Vector(null, value, Trace.wide)"),("lost-wide","Vector(this, value, Trace.wide)","Vector(this, value, Trace.wide.toInt().toLong())")):
            if source.count(old)!=1:raise RuntimeError("Generic default counterexample anchor changed")
            wrong=work/label/"DefaultApi.kt";wrong.parent.mkdir();wrong.write_text(source.replace(old,new))
            variant=work/(label+"-classes");variant.mkdir();verify.run([*compiler,"-d",variant,wrong],"kotlinc-"+label)
            if args.synthetic_default:
                change_method_flags(variant/"fixture/DefaultApi.class",
                    b"(Lfixture/Link;)Lfixture/Vector;",0x1,0x1001,name=b"work")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),harness_name,"1","1","0","work"],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned generic default access" not in result.stderr:raise RuntimeError("Frozen generic default observer missed "+label)
        wrong=work/"virtual-dispatch"/(caller_name+".java");wrong.parent.mkdir()
        source=(harness_fixture/(caller_name+".java")).read_text();old="return DefaultApi.DefaultImpls.work(receiver,link);"
        if source.count(old)!=1:raise RuntimeError("Frozen generic default caller anchor changed")
        wrong.write_text(source.replace(old,"return receiver.work(link);"));variant=work/"virtual-dispatch-classes"
        verify.compile_local([wrong],variant,classpath=os.pathsep.join(map(str,(compiler_view,*dependencies))),release=8)
        result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),harness_name,"1","1","0","work"],text=True,capture_output=True,timeout=600,env=verify.environment)
        (work/"virtual-dispatch-counterexample.log").write_text(result.stdout+result.stderr)
        if result.returncode==0 or "owned generic default access" not in result.stderr:raise RuntimeError("Frozen generic default observer missed virtual default dispatch")
        if args.closed_parameters:
            # Generic reflection is independently observable even when a
            # signature-only drift leaves every original Code byte intact.
            try:
                from scripts.test_mobile_android_static_defaults import method_code
            except ModuleNotFoundError:
                from test_mobile_android_static_defaults import method_code
            data=(originals/"fixture/DefaultApi.class").read_bytes()
            before=signature.encode();after=signature.replace("<Ljava/lang/String;", "<Ljava/lang/Object;").encode()
            old=b"\x01"+len(before).to_bytes(2,"big")+before
            new=b"\x01"+len(after).to_bytes(2,"big")+after
            if data.count(old)!=1:raise RuntimeError("Missing closed method Signature counterexample")
            changed=data.replace(old,new)
            if method_code(data)!=method_code(changed):raise RuntimeError("Signature counterexample changed Code")
            wrong=work/"signature-drift/fixture/DefaultApi.class";wrong.parent.mkdir(parents=True);wrong.write_bytes(changed)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parent.parent,originals,harness,*dependencies))),harness_name,"1","1","0","work"],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/"signature-drift-counterexample.log").write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned generic default access" not in result.stderr:raise RuntimeError("Frozen observer missed closed Signature drift")
        if args.synthetic_default:
            wrong=work/"flag-drift/fixture/DefaultApi.class";wrong.parent.mkdir(parents=True)
            shutil.copy2(originals/"fixture/DefaultApi.class",wrong)
            receipt=change_method_flags(wrong,b"(Lfixture/Link;)Lfixture/Vector;",0x1001,0x1,name=b"work")
            (work/"flag-drift-witness.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parent.parent,originals,harness,*dependencies))),harness_name,"1","1","0","work"],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/"flag-drift-counterexample.log").open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned generic default access" not in result.stderr:
                raise RuntimeError("Frozen observer missed unchanged-Code default flag drift")
            if hashes(compiler_view)!=compiler_view_hashes:
                raise RuntimeError("Frozen compile-only source view changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}!=input_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}!=dependency_hashes:raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","dependency_contracts":args.dependency_contracts,"closed_parameters":args.closed_parameters,"synthetic_default":args.synthetic_default,"class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases_per_stage":len(baseline),"stages":2,"attempts_per_case":2,
            "default_and_virtual_dispatch":"matched","complete_generic_Signatures":"matched","original_receiver_inputs_partial_fields_wide_and_exceptions":"matched",
            "original_code_bytes":"compiler output unchanged; optional witnessed synthetic flag only","independent_counterexamples":4+int(args.closed_parameters)+int(args.synthetic_default),"compiler_CLASS_annotation_loss_sites":len(dropped),"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
