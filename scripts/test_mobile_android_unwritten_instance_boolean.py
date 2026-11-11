#!/usr/bin/env python3
"""Verify unwritten Boolean finals preserve allocation defaults and mutable field reads."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import zipfile
try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def encode_final(path):
    before=path.read_bytes();shape=class_records(before);strings=shape["strings"]
    pos=shape["pool_end"]+6;pos+=2+2*int.from_bytes(before[pos:pos+2],"big")
    count=int.from_bytes(before[pos:pos+2],"big");pos+=2;after=bytearray(before);changed=0
    for _ in range(count):
        flags,name,descriptor,attrs=struct.unpack_from(">HHHH",before,pos)
        if strings[name]==b"zeroFlag":
            if flags!=1 or strings[descriptor]!=b"Z":raise RuntimeError("Original instance field changed")
            struct.pack_into(">H",after,pos,0x11);changed+=1
        pos+=8
        for _ in range(attrs):pos+=6+int.from_bytes(before[pos+2:pos+6],"big")
    if changed!=1 or method_code(before)!=method_code(bytes(after)):raise RuntimeError("Field encoding changed original Code")
    path.write_bytes(after)
    return {"field":"zeroFlag:Z","access_before":1,"access_after":17,"Code_descriptors_Signatures_and_all_other_attributes":"unchanged"}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-observer",action="store_true")
    parser.add_argument("--open-owner",action="store_true");args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-unwritten-instance-boolean-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        version=verify.run([verify.java,"--version"],"jdk-version")
        major=re.search(r"(?m)^(?:openjdk|java) (\d+)",version)
        if not major:raise RuntimeError("Unknown owned JVM version")
        java_options=["--enable-final-field-mutation=ALL-UNNAMED"] if int(major[1])>=26 else []
        fixture=FIXTURES/"unwritten_instance_boolean/java"
        if args.open_owner:
            source=work/"open-owner-sources"
            shutil.copytree(fixture,source)
            owner=source/"fixture/Owner.java";text=owner.read_text()
            if text.count("public final class Owner")!=1:raise RuntimeError("Owned open class anchor changed")
            owner.write_text(text.replace("public final class Owner","public class Owner"))
            fixture=source
        originals,encoded,harness=(work/n for n in ("originals","encoded","harness"))
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("UnwrittenInstanceBooleanCaller","UnwrittenInstanceBooleanHarness")]
        if args.open_owner:
            local=work/"open-owner-observer";local.mkdir()
            child=local/"OwnedOpenDefaultsChild.java"
            child.open("x").write("import fixture.Owner;\npublic final class OwnedOpenDefaultsChild extends Owner { public OwnedOpenDefaultsChild(long value,int count){super(value,count);} public boolean inherited(){return zeroFlag;} }\n")
            observer=local/"UnwrittenInstanceBooleanHarness.java";text=harness_sources[1].read_text()
            text=text.replace("if(route==0)","if(route==4)return new OwnedOpenDefaultsChild(value,count);\n        if(route==0)")
            anchor="check(Trace.partial.zeroFlag==mutate"
            if text.count(anchor)!=1:raise RuntimeError("Owned inherited read anchor changed")
            text=text.replace(anchor,"if(Trace.partial instanceof OwnedOpenDefaultsChild)check(((OwnedOpenDefaultsChild)Trace.partial).inherited()==mutate);\n            "+anchor)
            observer.open("x").write(text);harness_sources=[harness_sources[0],observer,child]
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=4 or len(all_methods)!=8:raise RuntimeError("Original instance-final inventory changed")
        def execute(runtime,flag,label):
            result=[]
            for route in range(5 if args.open_owner else 4):
                for mode in range(9):
                    out=verify.run([verify.java,*java_options,"-cp",os.pathsep.join(map(str,runtime)),"UnwrittenInstanceBooleanHarness",str(route),str(mode),str(flag).lower()],f"{label}-{route}-{mode}")
                    if len(out.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in out.splitlines()):raise RuntimeError("Original default-field observer changed")
                    result.append(out)
            return result
        baseline=execute((originals,harness),False,"mutable")
        before=class_identity.compiler_classes(originals);shutil.copytree(originals,encoded)
        receipt=encode_final(encoded/"fixture/Owner.class")
        (work/"field-encoding-witness.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        records=class_identity.compiler_classes(encoded);wanted=copy.deepcopy(before)
        field="Lfixture/Owner;->zeroFlag:Z"
        if wanted["Lfixture/Owner;"]["fields"][field]["constant_value"] is not None:raise RuntimeError("Original Boolean field became a constant")
        wanted["Lfixture/Owner;"]["fields"][field]["access"]|=0x10
        for owner in records:
            for key in wanted[owner]:
                if key!="sha256" and wanted[owner][key]!=records[owner].get(key):
                    raise RuntimeError("Final-field encoding changed other original metadata: "+owner+":"+key)
        if execute((encoded,harness),True,"encoded")!=baseline:raise RuntimeError("Field flag encoding changed original behavior")
        encoded_hashes,exceptions=hashes(encoded),exception_inventory(encoded)
        classes=all_classes-({"Lfixture/Trace;"} if args.dependency_observer else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        dependency=work/"owned-observer.jar"
        with zipfile.ZipFile(dependency,"x") as jar:jar.writestr("fixture/Trace.class",(encoded/"fixture/Trace.class").read_bytes())
        dep_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_observer:classpath += ["--classpath",dependency]
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(encoded.rglob("*.class")) if "L"+p.relative_to(encoded).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original instance-final DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inv=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inv["classes"]}!=classes or {r["identity"] for r in inv["methods"]}!=methods:raise RuntimeError("D8 changed an original instance-final entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inv,indent=2)+"\n")
        (work/"baseline-cli-sha256.txt").open("x").write(hashlib.sha256(args.neverd.read_bytes()).hexdigest()+"\n")
        frozen_dex=hashlib.sha256(source_dex.read_bytes()).hexdigest()
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"encoded":encoded_hashes,"harness":harness_hashes,"dex":frozen_dex},indent=2)+"\n")
        output=work/"recovered";verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        cov=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in cov["methods"]}!=methods or cov["class_count"]!=len(classes) or cov["method_count"]!=len(methods) or cov["unrecovered_method_count"] or cov.get("projected_method_count",0) or cov["status"]!="recovered" or cov.get("class_source_bindings") or cov.get("generic_signature_projections")):raise RuntimeError("Instance-final recovery changed original contracts")
        init=cov.get("default_instance_final_initializers",[])
        if len(init)!=1 or init[0]["class"]!="Lfixture/Owner;" or init[0]["field"]!="zeroFlag" or init[0]["type"]!="Z" or init[0]["original_write_count"]!=0 or init[0]["ConstantValue"]!="absent" or init[0]["source_initializer"]!="(null == null ? false : false)" or init[0]["initialization_point"]!="after-Object-constructor-before-original-body":raise RuntimeError("Missing exact instance-default initialization receipt")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_observer else None,release=cov.get("minimum_java_release",8))
        cls,meth=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in cov.get("generated_source_helpers",[])}
        expected={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Hook;"}
        if cls!=classes or set(meth)!=methods|expected or helpers!=expected:raise RuntimeError("Instance-final recovery lost or invented entries/helpers")
        after,after_exc=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original instance-final class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exc[identity]!=exceptions[identity]:raise RuntimeError("Original instance-final method metadata/Exceptions changed")
        runtime=(rebuilt,harness,dependency) if args.dependency_observer else (rebuilt,harness)
        if execute(runtime,True,"rebuilt")!=baseline:raise RuntimeError("Default field, loops, partial state, reflective mutation or exact failures changed")
        original=(fixture/"fixture/Owner.java").read_text()
        mutants=(("constant-getter","return zeroFlag;","return false;",6),("late-default","Trace.observe(this,1);","Trace.observe(this,1);zeroFlag=false;",6),("lost-wide","this.value=value;","this.value=(int)value;",0),("skipped-loop","i++","i+=count",7))
        for label,old,new,mode in mutants:
            if original.count(old)!=1:raise RuntimeError("Instance-final counterexample anchor changed")
            changed=work/label/"Owner.java";changed.parent.mkdir();changed.open("x").write(original.replace(old,new))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8);encode_final(wrong/"fixture/Owner.class")
            result=subprocess.run([verify.java,*java_options,"-cp",os.pathsep.join(map(str,(wrong,encoded,harness))),"UnwrittenInstanceBooleanHarness","1",str(mode),"true"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned unwritten instance Boolean" not in result.stderr:raise RuntimeError("Frozen observer missed "+label)
        if hashes(originals)!=original_hashes or hashes(encoded)!=encoded_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dep_hash:raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"baseline_stages_compared":3,"attempts_per_case":2,"stages":3,"dependency_observer":args.dependency_observer,"open_owner":args.open_owner,"allocation_default_and_field_reads":"matched","partial_fields_loops_reflective_mutation_and_exact_failures":"matched","all_metadata_and_ConstantValue_absence":"unchanged except witnessed final flag","independent_wrong_programs":len(mutants),"original_Code_descriptors_and_other_attributes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
