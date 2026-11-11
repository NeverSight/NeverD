#!/usr/bin/env python3
"""Verify inherited unwritten Boolean finals preserve superclass observations and mutations."""
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


def encode_final(path, expected_access=1):
    before=path.read_bytes();shape=class_records(before);strings=shape["strings"]
    pos=shape["pool_end"]+6;pos+=2+2*int.from_bytes(before[pos:pos+2],"big")
    count=int.from_bytes(before[pos:pos+2],"big");pos+=2;after=bytearray(before);changed=0
    for _ in range(count):
        flags,name,descriptor,attrs=struct.unpack_from(">HHHH",before,pos)
        if strings[name]==b"zeroFlag":
            if flags!=expected_access or strings[descriptor]!=b"Z":raise RuntimeError("Original instance field changed")
            struct.pack_into(">H",after,pos,expected_access|0x10);changed+=1
        pos+=8
        for _ in range(attrs):pos+=6+int.from_bytes(before[pos+2:pos+6],"big")
    if changed!=1 or method_code(before)!=method_code(bytes(after)):raise RuntimeError("Field encoding changed original Code")
    path.write_bytes(after)
    return {"field":"zeroFlag:Z","access_before":expected_access,"access_after":expected_access|0x10,"Code_descriptors_Signatures_and_all_other_attributes":"unchanged"}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-observer",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    parser.add_argument("--typed-super",action="store_true")
    parser.add_argument("--package-owner",action="store_true");args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-unwritten-boolean-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        version=verify.run([verify.java,"--version"],"jdk-version")
        major=re.search(r"(?m)^(?:openjdk|java) (\d+)",version)
        if not major:raise RuntimeError("Unknown owned JVM version")
        java_options=["--enable-final-field-mutation=ALL-UNNAMED"] if int(major[1])>=26 else []
        fixture=FIXTURES/"inherited_unwritten_boolean/java"
        harness_names=("InheritedUnwrittenBooleanCaller","InheritedUnwrittenBooleanHarness")
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in harness_names]
        harness_main="InheritedUnwrittenBooleanHarness"
        field_access=2 if args.package_owner else 1
        if args.package_owner or args.typed_super:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
        if args.package_owner:
            owner=fixture/"fixture/Owner.java";text=owner.read_text()
            if text.count("public abstract class Owner")!=1 or text.count("public boolean zeroFlag;")!=1:raise RuntimeError("Original package owner anchors changed")
            owner.write_text(text.replace("public abstract class Owner","abstract class Owner").replace("public boolean zeroFlag;","private boolean zeroFlag;"))
            trace=fixture/"fixture/Trace.java";text=trace.read_text()
            if text.count("owner.zeroFlag")!=1:raise RuntimeError("Original trace field observation anchor changed")
            trace.write_text(text.replace("owner.zeroFlag","owner.flag()"))
            sources=work/"harness-source";sources.mkdir();harness_sources=[]
            for name in harness_names:
                text=(FIXTURES/"harness"/(name+".java")).read_text()
                if name.endswith("Caller"):
                    if text.count("owner.zeroFlag")!=1:raise RuntimeError("Original caller field observation anchor changed")
                    text=text.replace("owner.zeroFlag","owner.flag()")
                else:
                    if text.count('Owner.class.getField("zeroFlag")')!=1 or text.count("Trace.partial.zeroFlag")!=1:raise RuntimeError("Original private field observer anchors changed")
                    text=text.replace('Owner.class.getField("zeroFlag")','Owner.class.getDeclaredField("zeroFlag")').replace("Trace.partial.zeroFlag","flag.getBoolean(Trace.partial)")
                path=sources/(name+".java");path.open("x").write("package fixture;\n"+text);harness_sources.append(path)
            harness_main="fixture.InheritedUnwrittenBooleanHarness"
        if args.typed_super:
            def replace_once(path,before,after):
                text=path.read_text()
                if text.count(before)!=1:raise RuntimeError("Typed superclass fixture anchor changed")
                path.write_text(text.replace(before,after))
            replace_once(fixture/"fixture/Hook.java","interface Hook {","interface Hook<T> {")
            replace_once(fixture/"fixture/Base.java","protected Base() {","protected Base(Hook<Object> input) {\n        Trace.baseHook=input;")
            replace_once(fixture/"fixture/Owner.java","super();","super(Trace.prepare());")
            replace_once(fixture/"fixture/Trace.java","    public static long events;",
                "    public static Hook baseHook;\n    public static Hook<Object> prepare() { hit(6); return (Hook<Object>)hook; }\n    public static long events;")
            copied=work/"typed-harness";copied.mkdir();adapted=[]
            for source in harness_sources:
                text=source.read_text()
                if source.name.endswith("Harness.java"):
                    text=text.replace("Trace.input=value;Trace.events=0;", "Trace.input=value;Trace.events=0;Trace.baseHook=null;")
                    text=text.replace("            check(Trace.partial!=null);", '''            if(failure==6) {
                check(Trace.partial==null&&Trace.baseHook==null&&Trace.events==6);
                System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
                continue;
            }
            check(Trace.baseHook==Trace.hook);
            check(Trace.partial!=null);''')
                    text=text.replace("long expected=0;", "long expected=6;")
                target=copied/source.name;target.open("x").write(text);adapted.append(target)
            harness_sources=adapted
        originals,encoded,harness=(work/n for n in ("originals","encoded","harness"))
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=6 or len(all_methods)!=(15 if args.typed_super else 14):raise RuntimeError("Original inherited-final inventory changed")
        def execute(runtime,flag,label):
            result=[]
            for route in range(4):
                for mode in range(16 if args.typed_super else 14):
                    out=verify.run([verify.java,*java_options,"-cp",os.pathsep.join(map(str,runtime)),harness_main,str(route),str(mode),str(flag).lower()],f"{label}-{route}-{mode}")
                    if len(out.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in out.splitlines()):raise RuntimeError("Original default-field observer changed")
                    result.append(out)
            return result
        baseline=execute((originals,harness),False,"mutable")
        before=class_identity.compiler_classes(originals);shutil.copytree(originals,encoded)
        receipt=encode_final(encoded/"fixture/Owner.class",field_access)
        (work/"field-encoding-witness.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        records=class_identity.compiler_classes(encoded);wanted=copy.deepcopy(before)
        if args.typed_super:
            identity="Lfixture/Base;-><init>(Lfixture/Hook;)V"
            expected="(Lfixture/Hook<Ljava/lang/Object;>;)V"
            if records["Lfixture/Base;"]["methods"][identity]["signature"]!=expected:
                raise RuntimeError("Typed superclass lost its complete carrier parameter")
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
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original inherited-final DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inv=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inv["classes"]}!=classes or {r["identity"] for r in inv["methods"]}!=methods:raise RuntimeError("D8 changed an original inherited-final entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inv,indent=2)+"\n")
        if args.typed_super:
            disassembly=verify.run([verify.dexdump,"-d",source_dex],"typed-super-disassembly")
            if "Lfixture/Trace;.prepare:()Lfixture/Hook;" not in disassembly or "Lfixture/Base;.<init>:(Lfixture/Hook;)V" not in disassembly:
                raise RuntimeError("D8 lost the original static preparation or typed superclass call")
        mutants=(("reset-after-super","Owner.java","super();","super();zeroFlag=false;",2),
                 ("constant-getter","Owner.java","return zeroFlag;","return false;",2),
                 ("skipped-super-callback","Base.java","        callback();\n","",2),
                 ("lost-wide","Owner.java","this.value=value;","this.value=(int)value;",0))
        if args.typed_super:
            mutants=tuple((label,filename,"super(Trace.prepare());" if label=="reset-after-super" else old,
                "super(Trace.prepare());zeroFlag=false;" if label=="reset-after-super" else new,mode)
                for label,filename,old,new,mode in mutants)
            mutants+=( ("duplicate-prepare","Owner.java","super(Trace.prepare());","super(Trace.prepare());Trace.prepare();",0), )
        for label,filename,old,new,mode in mutants:
            original=(fixture/"fixture"/filename).read_text()
            if original.count(old)!=1:raise RuntimeError("Inherited-final counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(old,new))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            if filename=="Owner.java":encode_final(wrong/"fixture/Owner.class",field_access)
            result=subprocess.run([verify.java,*java_options,"-cp",os.pathsep.join(map(str,(wrong,encoded,harness))),harness_main,"1",str(mode),"true"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned inherited unwritten Boolean" not in result.stderr:raise RuntimeError("Frozen observer missed "+label)
        if args.baseline_only:
            if hashes(originals)!=original_hashes or hashes(encoded)!=encoded_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dep_hash:raise RuntimeError("Original inputs or frozen observers changed")
            evidence={"status":"baseline-only","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"stages":2,"dependency_observer":args.dependency_observer,"package_owner":args.package_owner,"typed_super":args.typed_super,"independent_wrong_programs":len(mutants),"original_Code_descriptors_and_other_attributes":"unchanged"}
            (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
            return 0
        output=work/"recovered";verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        cov=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in cov["methods"]}!=methods or cov["class_count"]!=len(classes) or cov["method_count"]!=len(methods) or cov["unrecovered_method_count"] or cov.get("projected_method_count",0) or cov["status"]!="recovered" or cov.get("class_source_bindings") or cov.get("generic_signature_projections")):raise RuntimeError("Inherited-final recovery changed original contracts")
        init=cov.get("default_instance_final_initializers",[])
        if len(init)!=1 or init[0]["class"]!="Lfixture/Owner;" or init[0]["field"]!="zeroFlag" or init[0]["type"]!="Z" or init[0]["original_write_count"]!=0 or init[0]["ConstantValue"]!="absent" or init[0]["source_initializer"]!="false" or init[0]["initialization_point"]!="before-super-constructor-before-original-body" or cov.get("minimum_java_release")!=25:raise RuntimeError("Missing exact inherited-default initialization receipt")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_observer else None,release=cov.get("minimum_java_release",8))
        cls,meth=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in cov.get("generated_source_helpers",[])}
        expected={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Hook;"}
        if cls!=classes or set(meth)!=methods|expected or helpers!=expected:raise RuntimeError("Inherited-final recovery lost or invented entries/helpers")
        after,after_exc=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original inherited-final class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exc[identity]!=exceptions[identity]:raise RuntimeError("Original inherited-final method metadata/Exceptions changed")
        runtime=(rebuilt,harness,dependency) if args.dependency_observer else (rebuilt,harness)
        if execute(runtime,True,"rebuilt")!=baseline:raise RuntimeError("Superclass observation, partial state, reflective mutation or exact failures changed")
        if hashes(originals)!=original_hashes or hashes(encoded)!=encoded_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dep_hash:raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"stages":3,"dependency_observer":args.dependency_observer,"package_owner":args.package_owner,"typed_super":args.typed_super,"allocation_default_and_superclass_callback_reads":"matched","partial_fields_delegation_reflective_mutation_and_exact_failures":"matched","all_metadata_and_ConstantValue_absence":"unchanged except witnessed final flag","independent_wrong_programs":len(mutants),"original_Code_descriptors_and_other_attributes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
