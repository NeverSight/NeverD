#!/usr/bin/env python3
"""Verify final void adapters keep a discarded original reference call."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_interface_default_access import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_interface_default_access import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def encode_adapter(path: Path, *, owner: bool) -> dict:
    """Encode an owned JVM name/flag only, preserving every Code attribute."""
    original=path.read_bytes();old=b"\x01\x00\x07discard";new=b"\x01\x00\x05apply"
    if original.count(old)!=1:raise RuntimeError("Missing unique owned adapter name")
    data=bytearray(original.replace(old,new,1));shape=class_records(bytes(data));strings=shape["strings"]
    offset=shape["pool_end"]+6;offset+=2+2*int.from_bytes(data[offset:offset+2],"big")
    def end(position):
        count=int.from_bytes(data[position+6:position+8],"big");position+=8
        for _ in range(count):position+=6+int.from_bytes(data[position+2:position+6],"big")
        return position
    count=int.from_bytes(data[offset:offset+2],"big");offset+=2
    for _ in range(count):offset=end(offset)
    count=int.from_bytes(data[offset:offset+2],"big");offset+=2;marked=0
    for _ in range(count):
        flags,name,prototype=struct.unpack_from(">HHH",data,offset)
        if owner and strings[name]==b"apply" and strings[prototype]==b"(C)V":
            if flags!=0x11:raise RuntimeError("Owned adapter lost public final flags")
            struct.pack_into(">H",data,offset,flags|0x1000);marked+=1
        offset=end(offset)
    if marked!=(1 if owner else 0):raise RuntimeError("Missing unique owned adapter access encoding")
    before=method_code(original);expected={(b"apply"+k[len(b"discard"):]) if k.startswith(b"discard(") else k:v for k,v in before.items()}
    changed=bytes(data)
    if method_code(changed)!=expected:raise RuntimeError("Adapter encoding changed original Code bytes")
    path.write_bytes(changed)
    return {"name_encoding":"discard -> apply","synthetic_adapter":owner,"code_attributes":"byte-identical",
            "before_sha256":hashlib.sha256(original).hexdigest(),"after_sha256":hashlib.sha256(changed).hexdigest(),
            "code_sha256":{k.decode():hashlib.sha256(v).hexdigest() for k,v in expected.items()}}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-discarded-reference-result-")
    with context as directory:
        w=Path(directory).resolve();v=Verify(w,choose_jdk(args.java_home),choose_d8(args.d8),1200)
        fixture=FIXTURES/"discarded_reference_result/java";originals=w/"originals";harness=w/"harness";name="DiscardedReferenceResultHarness"
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        v.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        encodings={n:encode_adapter(originals/("fixture/"+n+".class"),owner=n=="Core") for n in ("Core","Client")}
        (w/"owned-input-encoding.json").open("x").write(json.dumps(encodings,indent=2)+"\n")
        v.compile_local([FIXTURES/"harness"/(name+".java")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (w/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        all_classes,all_methods=compiler_inventory(v,originals)
        if (len(all_classes),len(all_methods))!=(5,10):raise RuntimeError("Original discarded reference definition inventory changed")
        facts=compiler_facts(originals);exceptions=exception_inventory(originals)
        declarations={"Lfixture/Port;","Lfixture/Point;","Lfixture/Trace;"};excluded=declarations if args.dependency_provider else set()
        classes=all_classes-excluded;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        provider=w/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar:
            for c in sorted(declarations):
                path=c[1:-1]+".class";jar.writestr(path,(originals/path).read_bytes())
        dependencies=[provider] if excluded else [];input_hashes={str(p):sha(p) for p in (args.android_jar,provider)}
        def execute(runtime,label,alias):
            result=[]
            for route in range(5):
                for mode in range(16):
                    text=v.run([v.java,"-cp",os.pathsep.join(map(str,runtime)),name,str(route),str(mode),alias],f"{label}-{route}-{mode}")
                    if text!="attempts=2,discarded-reference-result=matched\n":raise RuntimeError("Frozen discarded reference observer changed")
                    result.append(text)
            return result
        baseline=execute((originals,harness),"baseline","apply")
        (w/"baseline.json").open("x").write(json.dumps(baseline,indent=2)+"\n")
        text=(fixture/"fixture/Core.java").read_text()
        wrong=[("lost-write","total+=value;","total=total;",0,0),
               ("double-call","{apply(value);}","{apply(value);apply(value);}",0,0),
               ("lost-call","{apply(value);}","{}",0,0),
               ("wrong-char","{apply(value);}","{apply((char)(value+1));}",0,5),
               ("wrapped-failure","{apply(value);}","{try{apply(value);}catch(Throwable error){throw new IllegalStateException(error);}}",0,8),
               ("lost-after","Trace.hit(1,this,value);","",0,0),
               ("wrong-return","return Trace.result;","return this;",3,0),
               ("early-result","Trace.hit(1,this,value);\n  return Trace.result;","Core saved=Trace.result;Trace.hit(1,this,value);\n  return saved;",3,12),
               ("lost-adapter-frame","{apply(value);}","{Trace.hit(0,this,value);total+=value;Trace.hit(1,this,value);}",0,0),
               ("swallowed-failure","{apply(value);}","{try{apply(value);}catch(Throwable ignored){}}",0,7)]
        for label,before,after,route,mode in wrong:
            if text.count(before)!=1:raise RuntimeError("Discarded reference mutation anchor changed")
            source=w/label/"Core.java";source.parent.mkdir();source.open("x").write(text.replace(before,after));compiled=w/(label+"-classes")
            v.compile_local([source],compiled,classpath=originals,release=8)
            encode_adapter(compiled/"fixture/Core.class",owner=True)
            r=subprocess.run([v.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),name,str(route),str(mode),"apply"],text=True,capture_output=True,timeout=1200,env=v.environment)
            (w/(label+"-counterexample.log")).open("x").write(r.stdout+r.stderr)
            if r.returncode==0 or "owned discarded reference result" not in r.stderr:raise RuntimeError("Observer missed wrong discarded reference body: "+label)
        cp=["--classpath",args.android_jar.resolve()]
        if excluded:cp += ["--classpath",provider]
        dexdir=w/"dex";dexdir.mkdir();paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        v.run([*v.d8,"--debug","--min-api","24",*cp,"--output",dexdir,*paths],"d8-original")
        dex=dexdir/"classes.dex";frozen_dex=sha(dex)
        (w/"frozen-dex-hashes.json").open("x").write(json.dumps({str(dex):frozen_dex},indent=2)+"\n")
        (w/"baseline-cli-sha256.txt").open("x").write(sha(args.neverd)+"\n")
        dump=v.run([v.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory");inventory=parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed a discarded reference definition")
        (w/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=w/"recovered";v.run([args.neverd.resolve(),"mobile",dex,*cp,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        original="Lfixture/Core;->apply(C)V";row=rows.get(original,{})
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings") or coverage.get("minimum_java_release",8)!=8 or row.get("projection_kind")!="discarded-reference-result" or not row.get("metadata_differences"):raise RuntimeError("Discarded reference definitions or disclosed source contracts changed")
        alias=row["source_name"];source_identity="Lfixture/Core;->"+alias+"(C)V"
        if row.get("source_identity")!=source_identity or alias=="apply":raise RuntimeError("Void adapter lacks its own disclosed source identity")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        allowed={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Port;","Lfixture/Point;"}}
        rebuilt=w/"rebuilt";v.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)) if dependencies else None,release=8)
        actual_classes,actual_methods=compiler_inventory(v,rebuilt);expected=(methods-{original})|{source_identity}
        if actual_classes!=classes or set(actual_methods)!=expected|helpers or not helpers<=allowed:raise RuntimeError("Discarded reference source invented or omitted a definition")
        after=compiler_facts(rebuilt);after_exceptions=exception_inventory(rebuilt)
        for c in classes:
            wanted=copy.deepcopy(facts[c]);actual=copy.deepcopy(after[c]);actual["methods"]={m:r for m,r in actual["methods"].items() if m not in helpers}
            if c=="Lfixture/Core;":
                method=wanted["methods"].pop(original)
                if method["access"]!=0x1011:raise RuntimeError("Original final synthetic adapter flags changed")
                method["access"]&=~0x1000;method["name"]=alias;wanted["methods"][source_identity]=method
            if wanted!=actual:raise RuntimeError("Discarded reference metadata changed beyond disclosed name/flag: "+c)
            for m in facts[c]["methods"]:
                if exceptions[m]!=after_exceptions[source_identity if m==original else m]:raise RuntimeError("Discarded reference Exceptions changed")
        if execute((rebuilt,harness,*dependencies),"rebuilt",alias)!=baseline:raise RuntimeError("Discarded reference call, stack, char, result or failure changed")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or sha(dex)!=frozen_dex or any(sha(Path(p))!=h for p,h in input_hashes.items()):raise RuntimeError("Frozen discarded reference inputs changed")
        summary={"status":"verified","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":80,"attempts_per_case":2,"wrong_programs":10,"all_java_units":len(list((output/"sources").rglob("*.java"))),"minimum_java_release":8,"dependency_provider":args.dependency_provider,"input_encoding":"owned names/flags only; all compiler Code attributes unchanged"}
        (w/"summary.json").open("x").write(json.dumps(summary,indent=2)+"\n");print(json.dumps(summary,sort_keys=True))
    return 0


if __name__=="__main__":raise SystemExit(main())
