#!/usr/bin/env python3
"""Verify closed generic superclass calls reuse their original guarded prefix and frame."""
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
import struct
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


DEFAULTS={"zeroCallback":("Lfixture/Fn;","null"),"zeroBits":("J","0L"),
          "zeroFlag":("Z","false"),"zeroArray":("[Ljava/lang/Object;","null")}
def encode_finals(path):
    before=path.read_bytes();shape=class_records(before);strings=shape["strings"]
    pos=shape["pool_end"]+6;pos+=2+2*int.from_bytes(before[pos:pos+2],"big")
    count=int.from_bytes(before[pos:pos+2],"big");pos+=2;after=bytearray(before);changed=[]
    for _ in range(count):
        flags,name,descriptor,attrs=struct.unpack_from(">HHHH",before,pos)
        field=strings[name].decode();kind=strings[descriptor].decode()
        if field in DEFAULTS:
            if flags!=1 or kind!=DEFAULTS[field][0]:raise RuntimeError("Original guarded default field changed")
            struct.pack_into(">H",after,pos,0x11);changed.append(pos+1)
        pos+=8
        for _ in range(attrs):pos+=6+int.from_bytes(before[pos+2:pos+6],"big")
    if len(changed)!=4 or {i for i,(a,b) in enumerate(zip(before,after)) if a!=b}!=set(changed) or method_code(before)!=method_code(bytes(after)):
        raise RuntimeError("Final encoding changed original Code or other attributes")
    path.write_bytes(after)
    return {"fields":DEFAULTS,"access_before":1,"access_after":17,"all_Code_descriptors_Signatures_and_other_bytes":"unchanged",
            "before_sha256":hashlib.sha256(before).hexdigest(),"after_sha256":hashlib.sha256(after).hexdigest()}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--linear-prefix",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-guarded-typed-defaults-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        version=verify.run([verify.java,"--version"],"jdk-version")
        major=re.search(r"(?m)^(?:openjdk|java) (\d+)",version)
        if not major:raise RuntimeError("Unknown owned JVM version")
        java_options=["--enable-final-field-mutation=ALL-UNNAMED"] if int(major[1])>=26 else []
        fixture=FIXTURES/"guarded_typed_defaults/java"
        if args.linear_prefix:
            copied=work/"linear-fixture";shutil.copytree(fixture,copied);fixture=copied
            child=fixture/"fixture/Child.java";text=child.read_text()
            original="token!=null ? Trace.choice(token) : Trace.choice(Trace.fallback)"
            if text.count(original)!=1:raise RuntimeError("Original conditional prefix anchor changed")
            child.write_text(text.replace(original,"Trace.normalize(token)"))
            trace=fixture/"fixture/Trace.java";text=trace.read_text()
            original="public static Object choice(Object token)"
            if text.count(original)!=1:raise RuntimeError("Original choice declaration changed")
            trace.write_text(text.replace(original,"public static Object normalize(Object token) { return choice(token!=null ? token : fallback); }\n    "+original))
        harness_name="GuardedTypedDefaults"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=25)
        compatible=work/"java8-declarations"
        verify.compile_local([p for p in sorted(fixture.rglob("*.java")) if p.name!="Child.java"],compatible,classpath=originals,release=8)
        for p in compatible.rglob("*.class"):shutil.copy2(p,originals/p.relative_to(compatible))
        reference=work/"unencoded-originals";shutil.copytree(originals,reference)
        (work/"final-field-encoding.json").open("x").write(json.dumps(encode_finals(originals/"fixture/Child.class"),indent=2)+"\n")
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=7 or len(all_methods)!=(14 if args.linear_prefix else 13):raise RuntimeError("Original guarded typed defaults inventory changed")
        declarations={"Lfixture/Fn;","Lfixture/Unit;","Lfixture/Observer;","Lfixture/Parent;","Lfixture/Trace;"}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        signature="Lfixture/Fn<Ljava/lang/Object;Lfixture/Unit;>;"
        if records["Lfixture/Child;"]["fields"]["Lfixture/Child;->zeroCallback:Lfixture/Fn;"]["signature"]!=signature:
            raise RuntimeError("Original generic callback default field changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Fn","Unit","Observer","Parent","Trace"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for variant in range(2):
                    for mode in (*range(9),10,11,12,20):
                        output=verify.run([verify.java,*java_options,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                            str(route),str(variant),str(mode)],f"{label}-{route}-{variant}-{mode}")
                        pattern=f"route={route},variant={variant},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original guarded typed default matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original guarded typed defaults DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original guarded typed defaults entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-closed-super-disassembly")
        witnessed=False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            name=re.search(r"(?m)^\s+name\s+: '([^']+)'",block[2])
            if block[1]!="Lfixture/Child;" or not name or name[1]!="<init>":continue
            ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)",block[2])
            if ("move-result-wide" not in ops or sum(op.startswith("if-") for op in ops)!=(0 if args.linear_prefix else 1)
                    or "Lfixture/Parent;.<init>:(JLjava/lang/Object;)V" not in block[2]
                    or ops.index("move-result-wide")>=next(i for i,op in enumerate(ops) if op.startswith("invoke-direct"))):
                raise RuntimeError("Original guarded superclass prefix witness changed")
            witnessed=True
        if not witnessed:raise RuntimeError("Missing original guarded superclass prefix")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("generic_signature_projections")):
            raise RuntimeError("Guarded typed defaults recovery changed original entries or metadata contracts")
        if coverage.get("minimum_java_release")!=25:raise RuntimeError("Closed superclass prologue source release changed")
        defaults=coverage.get("default_instance_final_initializers",[])
        if len(defaults)!=4:raise RuntimeError("Missing exact typed default receipts")
        for row in defaults:
            if (row["class"]!="Lfixture/Child;" or row["field"] not in DEFAULTS
                    or (row["type"],row["source_initializer"])!=DEFAULTS[row["field"]]
                    or row["original_write_count"]!=0 or row["ConstantValue"]!="absent"
                    or row["initialization_point"]!="before-super-constructor-before-original-body"):
                raise RuntimeError("Typed default receipt changed allocation value or timing")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in ("Lfixture/Fn;","Lfixture/Observer;") }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Guarded typed defaults recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original guarded typed defaults class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original guarded typed defaults method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Guarded typed defaults recovery changed seed/null branch, primitive raw bits, actual result identity or failures")
        same_code=(
            ("unknown-field-formal",signature.replace("<Ljava/lang/Object;","<TT;")),
            ("wrong-field-erasure","Ljava/lang/Object;"),
        )
        for label,changed in same_code:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            receipt=change_signature(negative/"fixture/Child.class",signature.encode(),changed.encode())
            (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
            negative_dex=work/(label+"-dex");negative_dex.mkdir()
            verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",negative_dex,
                *[negative/p.relative_to(originals) for p in paths]],label+"-d8")
            if args.dependency_inputs:
                changed_jar=work/(label+"-provider.jar")
                with zipfile.ZipFile(changed_jar,"x") as jar:
                    for owner in sorted(declarations):
                        name=owner[1:-1]+".class";jar.writestr(name,(negative/name).read_bytes())
                negative_classpath=classpath[:-1]+[changed_jar]
            else:negative_classpath=classpath
            rejected=work/(label+"-rejected")
            result=subprocess.run([str(args.neverd.resolve()),"mobile",str(negative_dex/"classes.dex"),*map(str,negative_classpath),"-o",str(rejected)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or rejected.exists() or not any(t in result.stderr.lower() for t in ("generic","signature","constructor","erasure")):
                raise RuntimeError("Guarded typed defaults accepted an unproved signature: "+label)
        mutations=(
            ("reset-after-super","Child","super(bits,selected);","super(bits,selected);zeroCallback=null;zeroBits=0;zeroFlag=false;zeroArray=null;",0),
            ("narrowed-wide","Child","long bits=Trace.wide();","long bits=(int)Trace.wide();",0),
            ("lost-token","Child","super(bits,selected);","super(bits,null);",0),
            ("lost-observation","Parent","Trace.observer.prepared((Child)this);",";",0),
            ("early-final-store","Child","Trace.hit(5);\n        marker=selected;","marker=selected;\n        Trace.hit(5);",5),
            ("lost-callback","Child","return zeroCallback.apply(input);","return null;",0),
            ("changed-failure","Child","long bits=Trace.wide();","long bits;try {bits=Trace.wide();} catch(RuntimeException e) {throw new IllegalStateException(e);}",1),
            ("early-preparation","Child","long bits=Trace.wide();","Trace.choice(token);long bits=Trace.wide();",0),
        )
        for label,name,before,after,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original guarded defaults counterexample anchor changed: "+label)
            changed=work/label/(name+".java");changed.parent.mkdir();changed.open("x").write(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=reference,release=25)
            if name=="Child":
                (work/(label+"-final-encoding.json")).open("x").write(json.dumps(encode_finals(wrong/"fixture/Child.class"),indent=2)+"\n")
            result=subprocess.run([verify.java,*java_options,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_name+"Harness","1","0",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned guarded typed defaults" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect guarded default timing: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,"linear_prefix":args.linear_prefix,
            "guarded_prefix_typed_defaults_and_preparation_order":"matched","all_original_entries_and_metadata":"unchanged",
            "superclass_observation_reflective_mutation_exact_failure_and_callback_cast":"matched",
            "same_code_signature_mutations":len(same_code),"independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
