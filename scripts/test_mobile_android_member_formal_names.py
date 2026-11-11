#!/usr/bin/env python3
"""Verify scoped source names for class formals hidden by member declarations."""
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
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature

OWNER="Lfixture/Owner;"


def signature_location(records, identity):
    if identity in records:return records[identity]
    owner=identity.split("->",1)[0]
    for kind in ("fields","methods"):
        if identity in records[owner][kind]:return records[owner][kind][identity]
    raise RuntimeError("Projection named an unknown original declaration")


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--reserve-name",action="store_true")
    parser.add_argument("--dependency-trace",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-member-formal-names-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"member_formal_names/java"
        if args.reserve_name:
            authored=work/"reserved-source";shutil.copytree(fixture,authored);fixture=authored
            path=fixture/"fixture/Owner.java";text=path.read_text();anchor="public interface Item"
            if text.count(anchor)!=1:raise RuntimeError("Reserved member anchor changed")
            path.write_text(text.replace(anchor,"public static class __neverdType0 {}\n    "+anchor))
        originals,encoded,harness=work/"originals",work/"encoded",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(name+".java") for name in ("MemberFormalCaller","MemberFormalHarness")],
                             harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(4 if args.reserve_name else 3) or len(all_methods)!=(7 if args.reserve_name else 6):
            raise RuntimeError("Original member/formal inventory changed")
        original_records=class_identity.compiler_classes(originals)
        shutil.copytree(originals,encoded)
        owner=encoded/"fixture/Owner.class"
        signatures=(b"<Value::Lfixture/Owner$Item;>Ljava/lang/Object;",b"TValue;",b"(TValue;)V",b"(TValue;)TValue;")
        receipts=[change_signature(owner,signature,signature.replace(b"<Value:",b"<Item:").replace(b"TValue;",b"TItem;")) for signature in signatures]
        (work/"signature-encoding-witness.json").open("x").write(json.dumps(receipts,indent=2)+"\n")
        wanted=copy.deepcopy(original_records)
        def encode_metadata(node):
            if isinstance(node,dict):
                for key,value in node.items():
                    if key=="signature" and isinstance(value,str):node[key]=value.replace("<Value:","<Item:").replace("TValue;","TItem;")
                    else:encode_metadata(value)
            elif isinstance(node,list):
                for value in node:encode_metadata(value)
        encode_metadata(wanted)
        records=class_identity.compiler_classes(encoded)
        actual_metadata=copy.deepcopy(records)
        for collection in (wanted,actual_metadata):
            for record in collection.values():
                record.pop('size',None);record.pop('sha256',None)
        if actual_metadata!=wanted or compiler_inventory(verify,encoded)!=(all_classes,all_methods):
            raise RuntimeError("Witnessed Signature encoding changed another original attribute")
        encoded_hashes=hashes(encoded);exceptions=exception_inventory(encoded)
        classes=all_classes-({"Lfixture/Trace;"} if args.dependency_trace else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        dependency=work/"owned-trace.jar"
        with zipfile.ZipFile(dependency,"x") as jar:jar.writestr("fixture/Trace.class",(encoded/"fixture/Trace.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_trace:classpath += ["--classpath",dependency]
        def execute(runtime,label,name):
            outputs=[]
            for route in range(4):
                for operation in range(2):
                    for mode in range(8):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"MemberFormalHarness",
                            str(route),str(operation),str(mode),name],f"{label}-{route}-{operation}-{mode}")
                        pattern=f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original member/formal observer matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline","Value")
        if execute((encoded,harness),"encoded","Item")!=baseline:raise RuntimeError("Signature-only witness changed behavior")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(encoded.rglob("*.class")) if "L"+p.relative_to(encoded).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original member/formal DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original member/formal runtime entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered";verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="partial"
                or coverage.get("class_source_bindings")):
            raise RuntimeError("Member/formal recovery lost entries or concealed its metadata projection")
        projections=coverage.get("generic_signature_projections",[])
        if len(projections)!=4 or len({r['declaration'] for r in projections})!=4:
            raise RuntimeError("Missing complete per-declaration scoped name receipts")
        source_name="__neverdType0_" if args.reserve_name else "__neverdType0"
        expected=copy.deepcopy(records)
        for row in projections:
            location=signature_location(expected,row['declaration']);signature=location['signature']
            if row['original_signature']!=signature or row['status']!='source-projected':raise RuntimeError("Incorrect original name receipt")
            changes=row['changes']
            if not changes or any(c.get('scope_origin')!=OWNER for c in changes):raise RuntimeError("Unowned formal name receipt")
            for change in sorted(changes,key=lambda c:c['offset'],reverse=True):
                if change['kind']=='type-variable-declaration-source-name':
                    if (row['declaration']!=OWNER or change['offset']!=0 or change['original_type']!='Item'
                            or change['source_type']!=source_name or not signature.startswith('<Item:')):
                        raise RuntimeError("Incorrect formal declaration name receipt")
                    signature='<'+source_name+signature[len('<Item'):]
                elif change['kind']=='type-variable-reference-source-name':
                    offset=change['offset']
                    if (change['original_type']!='TItem;' or change['source_type']!='T'+source_name+';'
                            or signature[offset:offset+len('TItem;')]!='TItem;'):
                        raise RuntimeError("Incorrect scoped reference receipt offset")
                    signature=signature[:offset]+change['source_type']+signature[offset+len('TItem;'):]
                else:raise RuntimeError("Unexpected generic metadata projection")
            location['signature']=signature
        rebuilt=work/"rebuilt";verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_trace else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r['class']+'->'+r['name']+r['prototype'] for r in coverage.get('generated_source_helpers',[])}
        expected_helpers={c+'->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;' for c in classes if c!='Lfixture/Owner$Item;'}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Member/formal recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ('access','signature','superclass','interfaces','fields','inner_class','enclosing_method',
                        'deprecated_attribute','runtime_visible_annotations','runtime_invisible_annotations'):
                if expected[c].get(key)!=after[c].get(key):raise RuntimeError("Unreported member/formal class metadata change: "+c+':'+key)
            for method,metadata in expected[c]['methods'].items():
                if after[c]['methods'].get(method)!=metadata or after_exceptions[method]!=exceptions[method]:
                    raise RuntimeError("Unreported member/formal method metadata or Exceptions change: "+method)
        runtime=(rebuilt,harness,dependency) if args.dependency_trace else (rebuilt,harness)
        if execute(runtime,'rebuilt',source_name)!=baseline:raise RuntimeError("Scoped source spelling changed original behavior")
        text=(fixture/'fixture/Owner.java').read_text()
        mutations=(('lost-store','value = input;','value = null;'),('lost-effect','Trace.hit(2);',''),
                   ('changed-result','return value;','return null;'))
        for label,before,replacement in mutations:
            if text.count(before)!=1:raise RuntimeError("Missing independent member/formal counterexample anchor")
            changed=work/label/'Owner.java';changed.parent.mkdir();changed.open('x').write(text.replace(before,replacement))
            wrong=work/(label+'-classes');verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,'-cp',os.pathsep.join(map(str,(wrong,originals,harness))),'MemberFormalHarness',
                '1','0','7','Value'],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+'-counterexample.log')).open('x').write(result.stdout+result.stderr)
            if result.returncode==0 or 'owned member formal names' not in result.stderr:raise RuntimeError('Observer missed altered behavior: '+label)
        wrong=work/'wrong-formal-scope/fixture/Owner.class';wrong.parent.mkdir(parents=True);shutil.copy2(encoded/'fixture/Owner.class',wrong)
        receipt=change_signature(wrong,b'(TItem;)TItem;',b'(Lfixture/Owner$Item;)Lfixture/Owner$Item;')
        (work/'wrong-formal-scope-encoding.json').open('x').write(json.dumps(receipt,indent=2)+'\n')
        result=subprocess.run([verify.java,'-cp',os.pathsep.join(map(str,(wrong.parents[1],encoded,harness))),'MemberFormalHarness',
            '1','0','7','Item'],text=True,capture_output=True,timeout=300,env=verify.environment)
        (work/'wrong-formal-scope-counterexample.log').open('x').write(result.stdout+result.stderr)
        if result.returncode==0 or 'owned member formal names' not in result.stderr:raise RuntimeError('Observer missed nominal member replacing a class formal')
        if (hashes(originals)!=original_hashes or hashes(encoded)!=encoded_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError('Original inputs or frozen observers changed')
        evidence={'status':'success','class_count':len(classes),'method_count':len(methods),'fresh_jvm_cases':len(baseline),
            'attempts_per_case':2,'reserve_name':args.reserve_name,'dependency_trace':args.dependency_trace,'independent_wrong_programs':4,
            'Code_and_all_other_original_attributes':'unchanged by witnessed Signature encoding',
            'all_Java_entries_metadata_and_Exceptions':'matched exact scoped name receipts',
            'formal_owner_bound_member_identity_stores_effects_and_exact_failures':'matched','originals_and_frozen_observers':'unchanged'}
        (work/'evidence.json').open('x').write(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=='__main__':raise SystemExit(main())
