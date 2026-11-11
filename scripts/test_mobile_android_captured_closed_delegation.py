#!/usr/bin/env python3
"""Verify closed generic this-delegation through an original outer capture."""
from __future__ import annotations
import argparse
import copy
from contextlib import nullcontext
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
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


OWNER="Lfixture/Owner;"
WORKER="Lfixture/Owner$Worker;"
CAPTURE=WORKER+"->this$0:"+OWNER
ACCESS_PROTOTYPE="("+OWNER+"IJLfixture/Callback;Ljava/lang/Object;)V"
ACCESS=WORKER+"-><init>"+ACCESS_PROTOTYPE


def synthetic_access(path: Path) -> dict:
    before=path.read_bytes();shape=class_records(before);strings=shape['strings']
    position=shape['pool_end']+6
    position+=2+2*int.from_bytes(before[position:position+2],'big')
    def attributes(at,count):
        for _ in range(count):
            _,size=struct.unpack_from('>HI',before,at);at+=6+size
        return at
    count=int.from_bytes(before[position:position+2],'big');position+=2
    for _ in range(count):position=attributes(position+8,int.from_bytes(before[position+6:position+8],'big'))
    count=int.from_bytes(before[position:position+2],'big');position+=2;found=[]
    for _ in range(count):
        start=position;flags,name,prototype,size=struct.unpack_from('>HHHH',before,start)
        position=attributes(start+8,size)
        if strings[name]==b'<init>' and strings[prototype]==ACCESS_PROTOTYPE.encode():
            if flags!=1:raise RuntimeError('Original raw access constructor flags changed')
            found.append(start)
    if len(found)!=1:raise RuntimeError('Missing unique owned raw access constructor')
    at=found[0];after=before[:at]+struct.pack('>H',0x1001)+before[at+2:]
    if method_code(before)!=method_code(after):raise RuntimeError('Synthetic flag encoding changed original Code/descriptors')
    path.write_bytes(after)
    return {'method':ACCESS,'original_flags':1,'input_flags':0x1001,'all_code_descriptors_and_other_bytes':'unchanged',
        'before_sha256':hashlib.sha256(before).hexdigest(),'after_sha256':hashlib.sha256(after).hexdigest()}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--public-member",action="store_true")
    parser.add_argument("--synthetic-access",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-closed-delegation-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"captured_closed_delegation/java"
        if args.public_member or args.synthetic_access:
            owned=work/"public-source";shutil.copytree(fixture,owned);fixture=owned
            path=fixture/"fixture/Owner.java";text=path.read_text();anchor="private final class Worker {"
            if text.count(anchor)!=1:raise RuntimeError("Original public member anchor changed")
            if args.public_member:text=text.replace(anchor,"public final class Worker {")
            if args.synthetic_access:
                anchor='public Worker(int index,long bits,Callback<? super Value,? extends Value> callback)'
                if text.count(anchor)!=1:raise RuntimeError('Original raw access target anchor changed')
                text=text.replace(anchor,anchor.replace('public Worker','private Worker'))
                anchor='        public Owner owner()'
                if text.count(anchor)!=1:raise RuntimeError('Original raw access declaration anchor changed')
                text=text.replace(anchor,'        public Worker(int index,long bits,Callback callback,Object marker) { this(index,bits,callback); }\n'+anchor)
                anchor='return new Worker(index,bits,callback);'
                if text.count(anchor)!=1:raise RuntimeError('Original raw access caller anchor changed')
                text=text.replace(anchor,'return new Worker(index,bits,callback,null);')
            path.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=17)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in
            ("CapturedClosedDelegationCaller","CapturedClosedDelegationHarness")],harness,classpath=originals,release=17)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=5 or len(all_methods)!=(11 if args.synthetic_access else 10):raise RuntimeError("Original captured delegation inventory changed")
        declarations={"Lfixture/Callback;","Lfixture/Value;"}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        bytecode=originals
        if args.synthetic_access:
            bytecode=work/'encoded';shutil.copytree(originals,bytecode)
            (work/'input-metadata-change.json').open('x').write(json.dumps(synthetic_access(bytecode/'fixture/Owner$Worker.class'),indent=2)+'\n')
            encoded_records=class_identity.compiler_classes(bytecode)
            for c in all_classes:
                expected=copy.deepcopy(records[c])
                if c==WORKER:expected['methods'][ACCESS]['access'] |= 0x1000
                for key in expected:
                    if key!='sha256' and expected[key]!=encoded_records[c][key]:raise RuntimeError('Synthetic access encoding changed unrelated metadata: '+key)
            if exception_inventory(bytecode)!=exceptions:raise RuntimeError('Synthetic access encoding changed Exceptions')
            records=encoded_records
        bytecode_hashes=hashes(bytecode)
        dependency=work/"owned-inputs.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Callback","Value"):jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(17):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"CapturedClosedDelegationHarness",
                        str(route),str(mode),label,str(args.public_member).lower(),str(args.synthetic_access).lower()],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original captured delegation observer output changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        if args.synthetic_access and execute((bytecode,harness),'encoded')!=baseline:raise RuntimeError('Synthetic flag encoding changed original JVM behavior')
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(bytecode.rglob("*.class")) if "L"+p.relative_to(bytecode).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","26",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original captured delegation DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original captured delegation entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=(5 if args.synthetic_access else 4) or coverage["status"]!="partial"
                or coverage.get("minimum_java_release")!=25 or coverage.get("generic_signature_projections")):
            raise RuntimeError("Captured delegation changed original entries or generic contracts")
        projected=[r for r in coverage['methods'] if r['status']=='source-projected']
        if ({r['identity'] for r in projected}!={m for m in methods if m.startswith(WORKER+'->')}
                or any(r['projection_kind']!='inner-to-static-member' or r.get('source_identity') is not None for r in projected)):
            raise RuntimeError('Captured delegation changed its existing member projection')
        bindings=coverage.get("class_source_bindings",[])
        if (len(bindings)!=1 or bindings[0]["class"]!=WORKER or bindings[0]["binding_kind"]!="inner-to-static-member"
                or bindings[0].get("outer_capture_field")!=CAPTURE):raise RuntimeError("Captured delegation lost its exact capture receipt")
        synthetic_note='source constructors cannot retain original synthetic flags'
        if (synthetic_note in bindings[0].get('metadata_differences',[]))!=args.synthetic_access:
            raise RuntimeError('Captured delegation lost its precise synthetic constructor disclosure')
        ctors=[r for r in coverage["methods"] if r["class"]==WORKER and r["name"]=="<init>"]
        if len(ctors)!=(3 if args.synthetic_access else 2):raise RuntimeError("Captured delegation changed a constructor")
        for row in ctors:
            if row['identity']==ACCESS:
                if row.get('generic_signature') or row.get('generic_signature_parameter_indices'):raise RuntimeError('Raw access constructor gained a Signature')
                continue
            expected=list(range(1,4 if "IJ" in row["prototype"] else 3))
            if (row.get("generic_signature_omitted_parameters")!=1 or row.get("generic_signature_parameter_indices")!=expected):
                raise RuntimeError("Captured delegation invented a different implicit parameter mapping")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_inputs else None,release=25)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Callback;"}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Captured delegation invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            before=records[c];expected_fields=copy.deepcopy(before["fields"]);expected_inner=copy.deepcopy(before["inner_class"])
            if c==WORKER:
                if expected_fields[CAPTURE]["access"]!=0x1010 or not expected_inner:raise RuntimeError("Original capture metadata changed")
                expected_fields[CAPTURE]["access"] &= ~0x1000;expected_inner["access"] |= 8
            if after[c]["fields"]!=expected_fields or after[c]["inner_class"]!=expected_inner:
                raise RuntimeError("Captured delegation changed fields or member metadata beyond its receipt")
            for key in ("access","signature","superclass","interfaces","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if before.get(key)!=after[c].get(key):raise RuntimeError("Original captured delegation metadata changed: "+c+":"+key)
            for identity,metadata in before["methods"].items():
                expected=copy.deepcopy(metadata)
                if args.synthetic_access and identity==ACCESS:expected['access'] &= ~0x1000
                elif c==WORKER and "-><init>(" in identity:
                    signature=expected["signature"]
                    if not signature or signature.startswith("("+OWNER):raise RuntimeError("Original omitted capture Signature changed")
                    expected["signature"]="("+OWNER+signature[1:]
                if after[c]["methods"].get(identity)!=expected or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original captured delegation method metadata changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Captured delegation changed capture, callback, stores or failure identity")
        mutations=(("lost-callback","Value selected=callback.apply(Trace.value);","Value selected=Trace.value;",0),
            ("lost-wide","this.bits=bits;","this.bits=0;",5),
            ("early-wide","Trace.hit(8);this.bits=bits;","this.bits=bits;Trace.hit(8);",14),
            ("late-index","this.index=index;Trace.hit(3);","Trace.hit(3);this.index=index;",9),
            ("lost-value","this.value=selected;","this.value=null;",0),
            ("late-callback","this.callback=callback;Trace.hit(4);","Trace.hit(4);this.callback=callback;",10))
        if args.synthetic_access:
            mutations+=(("consumed-marker","Object marker) { this(index,bits,callback); }","Object marker) { this(index,bits,(Callback)marker); }",0),)
        source=(fixture/"fixture/Owner.java").read_text()
        for label,before,after,mode in mutations:
            if source.count(before)!=1:raise RuntimeError("Original captured delegation wrong-program anchor changed")
            changed=work/label/"fixture/Owner.java";changed.parent.mkdir(parents=True);changed.open("x").write(source.replace(before,after))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=17)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),"CapturedClosedDelegationHarness",
                "1",str(mode),"wrong",str(args.public_member).lower(),str(args.synthetic_access).lower()],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned captured closed delegation" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect captured delegation: "+label)
        if (hashes(originals)!=original_hashes or hashes(bytecode)!=bytecode_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen captured observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,
            "dependency_inputs":args.dependency_inputs,"public_member":args.public_member,"synthetic_access":args.synthetic_access,"independent_wrong_programs":len(mutations),
            "synthetic_flag_only_encoding":"original and encoded JVM behavior matched" if args.synthetic_access else None,
            "all_original_entries_metadata_and_exceptions":"matched with existing capture and implicit parameter receipts",
            "capture_callback_wide_and_partial_fields_and_failures":"matched","original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
