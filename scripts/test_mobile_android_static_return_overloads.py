#!/usr/bin/env python3
"""Verify independently named static return-only slots retain their original bodies."""
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
import subprocess
import tempfile
try:
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8, class_identity
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8, class_identity
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def runtime_names(path: Path, *, strict: bool = True) -> dict:
    before=path.read_bytes();after=before
    for name in (b"genericSlot", b"nominalSlot"):
        old=b"\x01"+len(name).to_bytes(2,"big")+name
        if before.count(old)!=(1 if strict else min(1,before.count(old))):
            raise RuntimeError("Owned method-name constant changed")
        after=after.replace(old,b"\x01\x00\x06select")
    original=class_records(before);changed=class_records(after)
    if before[original["pool_end"]:]!=after[changed["pool_end"]:]:
        raise RuntimeError("Name corpus construction changed non-pool bytes")
    codes=method_code(before)
    expected={key.replace(b"genericSlot(",b"select(").replace(b"nominalSlot(",b"select("):value for key,value in codes.items()}
    if method_code(after)!=expected: raise RuntimeError("Name corpus construction changed Code")
    path.write_bytes(after)
    return {"all_Code_and_non_pool_bytes":"unchanged","names_only":["genericSlot","nominalSlot"],
            "before_sha256":hashlib.sha256(before).hexdigest(),"after_sha256":hashlib.sha256(after).hexdigest()}


def synthetic_slots(path: Path) -> dict:
    before=path.read_bytes();after=bytearray(before);shape=class_records(before)
    strings=shape["strings"];at=shape["pool_end"]+6
    at+=2+2*int.from_bytes(before[at:at+2],"big")
    def skip(offset,count):
        for _ in range(count):offset+=6+int.from_bytes(before[offset+2:offset+6],"big")
        return offset
    fields=int.from_bytes(before[at:at+2],"big");at+=2
    for _ in range(fields):at=skip(at+8,int.from_bytes(before[at+6:at+8],"big"))
    methods=int.from_bytes(before[at:at+2],"big");at+=2;offsets=[]
    for _ in range(methods):
        flags=int.from_bytes(before[at:at+2],"big");name=int.from_bytes(before[at+2:at+4],"big")
        if strings[name]==b"select":
            if not flags&8 or flags&0x1040:raise RuntimeError("Owned original slot flags changed")
            offsets.append(at);after[at:at+2]=(flags|0x1000).to_bytes(2,"big")
        at=skip(at+8,int.from_bytes(before[at+6:at+8],"big"))
    changed={i for i,(a,b) in enumerate(zip(before,after)) if a!=b}
    if len(offsets)!=2 or changed!=set(offsets) or method_code(before)!=method_code(bytes(after)):
        raise RuntimeError("Synthetic corpus construction changed more than two flag bytes")
    path.write_bytes(after)
    return {"set_flag":"ACC_SYNTHETIC","method_count":2,"all_Code_and_other_bytes":"unchanged",
            "before_sha256":hashlib.sha256(before).hexdigest(),"after_sha256":hashlib.sha256(after).hexdigest()}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"): parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"): parser.add_argument("--"+name,type=Path)
    parser.add_argument("--synthetic-slots",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-static-return-overloads-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"static_return_overloads"
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        sdk_hash=sha(args.android_jar)
        originals=work/"originals";harness=work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),originals,release=8)
        shutil.copytree(originals,work/"javac-originals")
        construction={name:runtime_names(originals/("fixture/"+name+".class")) for name in ("Core","Client")}
        (work/"name-corpus-construction.json").open("x").write(json.dumps(construction,indent=2)+"\n")
        if args.synthetic_slots:
            receipt=synthetic_slots(originals/"fixture/Core.class")
            (work/"synthetic-corpus-construction.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        verify.compile_local(sorted((fixture/"harness").glob("*.java")),harness,classpath=originals,release=8)
        frozen={"originals":hashes(originals),"harness":hashes(harness)}
        (work/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n")
        classes,methods=compiler_inventory(verify,originals)
        records=class_identity.compiler_classes(originals);exceptions=exception_inventory(originals)
        def execute(runtime,alias,label):
            rows=[]
            for route in range(4):
                for slot in range(2):
                    for consume in range(2):
                        for mode in range(13):
                            text=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"StaticReturnHarness",
                                             str(route),str(slot),str(consume),str(mode),alias],f"{label}-{route}-{slot}-{consume}-{mode}")
                            pattern=f"route={route},slot={slot},consume={consume},mode={mode},attempt=[01],events=[0-9]+"
                            if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()):
                                raise RuntimeError("Frozen static return observer changed")
                            rows.append(text)
            return rows
        baseline=execute((originals,harness),"select","baseline")
        classpath=["--classpath",args.android_jar.resolve()]
        dex=work/"dex";dex.mkdir()
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*sorted(originals.rglob("*.class"))],"d8")
        source_dex=dex/"classes.dex"
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]}!=classes or {row["identity"] for row in inventory["methods"]}!=set(methods):
            raise RuntimeError("D8 changed a static return corpus definition")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        synthetic_methods={identity for owner in records.values() for identity,row in owner["methods"].items() if row["access"]&0x1000}
        if args.synthetic_slots and (len(synthetic_methods)!=2 or {row["identity"] for row in inventory["methods"] if row["access_flags"]&0x1000}!=synthetic_methods):
            raise RuntimeError("D8 changed original synthetic slot flags")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        report=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={row["identity"]:row for row in report["methods"]}
        named=[row for row in rows.values() if row.get("source_name")]
        nominal=next(identity for identity in methods if "->select(" in identity and identity.endswith(")Lfixture/Box;"))
        generic=nominal[:-len("Lfixture/Box;")]+"Ljava/lang/Object;"
        if (set(rows)!=set(methods) or report["class_count"]!=len(classes) or report["method_count"]!=len(methods)
                or report["unrecovered_method_count"] or report.get("projected_method_count")!=(2 if args.synthetic_slots else 1) or report["status"]!="partial"
                or report.get("class_source_bindings") or report.get("generic_signature_projections") or len(named)!=1
                or named[0]["identity"]!=nominal or named[0].get("projection_kind")!="incompatible-static-return-slot"
                or named[0].get("conflicting_local_contracts")!=[generic] or len(named[0].get("metadata_differences",[]))<3):
            raise RuntimeError("Static return projection changed original inventory or unrelated contracts")
        if args.synthetic_slots:
            if rows[generic].get("projection_kind")!="synthetic-static-return-slot" or rows[generic].get("source_name"):
                raise RuntimeError("Unchanged synthetic return slot lacks its independent flag receipt")
            for identity in synthetic_methods:
                if not any("synthetic access flag" in text for text in rows[identity].get("metadata_differences",[])):
                    raise RuntimeError("Missing original synthetic flag disclosure")
        alias=named[0]["source_name"];renamed=nominal.replace("->select(","->"+alias+"(")
        if alias!="__neverdStaticReturnSlot_":raise RuntimeError("Original reserved name lost")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=report.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={row["class"]+"->"+row["name"]+row["prototype"] for row in report.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner!="Lfixture/Factory;"}
        if actual_classes!=classes or set(actual_methods)!=(set(methods)-{nominal})|{renamed}|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Static return recovery invented or omitted an original method/helper")
        after=class_identity.compiler_classes(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            before=records[owner];actual=after[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if before.get(key)!=actual.get(key):raise RuntimeError("Original static return class metadata changed: "+key)
            for identity,row in before["methods"].items():
                mapped=renamed if identity==nominal else identity
                wanted=copy.deepcopy(row)
                if identity==nominal:wanted["name"]=alias
                if args.synthetic_slots and identity in synthetic_methods:wanted["access"] &= ~0x1000
                if actual["methods"].get(mapped)!=wanted or after_exceptions[mapped]!=exceptions[identity]:
                    raise RuntimeError("Unreported original static return method metadata/Exceptions changed: "+identity)
        if execute((rebuilt,harness),alias,"rebuilt")!=baseline:raise RuntimeError("Static return bodies, casts, identity or failure state changed")
        bodies={name:(fixture/("java/fixture/"+name+".java")).read_text() for name in ("Core","Client")}
        wrongs=(
            ("wrong-slot","Client","Core.nominalSlot(input,box,factory,stamp,flags,wide)","(Box)Core.genericSlot(input,box,factory,stamp,flags,wide)",1,0,0),
            ("duplicate","Client","Object value=Core.genericSlot(input,box,factory,stamp,flags,wide);","Core.genericSlot(input,box,factory,stamp,flags,wide);Object value=Core.genericSlot(input,box,factory,stamp,flags,wide);",0,0,0),
            ("lost-result-cast","Core","return (Box<T>)result;","return null;",1,0,10),
            ("copied-result","Core","return (Box<T>)result;","Box<T> old=(Box<T>)result;return old==null?null:new Box<T>(old.value);",1,0,0),
            ("lost-input","Core","Trace.input=input;","Trace.input=null;",0,0,0),
            ("wrong-wide","Core","Trace.wide=wide;","Trace.wide=(int)wide;",1,0,8),
            ("early-payload-cast","Client","Box value=Core.nominalSlot(input,box,factory,stamp,flags,wide);","Box value=Core.nominalSlot(input,box,factory,stamp,flags,wide);String early=(String)value.value;",1,1,11),
            ("replaced-throw","Core","throw Trace.failure;","throw new IllegalArgumentException();",0,0,2),
        )
        for label,owner,old,new,slot,consume,mode in wrongs:
            if old not in bodies[owner]:raise RuntimeError("Owned wrong program anchor changed: "+label)
            wrong=work/label/("fixture/"+owner+".java");wrong.parent.mkdir(parents=True);wrong.open("x").write(bodies[owner].replace(old,new))
            compiled=work/(label+"-classes")
            verify.compile_local([wrong],compiled,classpath=work/"javac-originals",release=8)
            runtime_names(compiled/("fixture/"+owner+".class"),strict=False)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),"StaticReturnHarness","0",str(slot),str(consume),str(mode),"select"],capture_output=True,text=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned static return overload changed" not in result.stderr:raise RuntimeError("Frozen observer missed "+label)
        if {"originals":hashes(originals),"harness":hashes(harness)}!=frozen or sha(args.android_jar)!=sdk_hash:
            raise RuntimeError("Frozen static return originals/observers/SDK changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,
                  "independent_wrong_programs":len(wrongs),"source_aliases":1,"synthetic_slots":args.synthetic_slots,"synthetic_flag_differences":2 if args.synthetic_slots else 0,"all_original_bodies_and_Code":"retained",
                  "original_metadata_and_exceptions":"unchanged except disclosed method name and synthetic flags" if args.synthetic_slots else "unchanged except disclosed method name","input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
