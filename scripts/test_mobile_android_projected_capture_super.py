#!/usr/bin/env python3
"""Verify projected captures leave an unchanged self consumer superclass argument."""
from __future__ import annotations
import argparse
import copy
import struct
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.test_mobile_android_shared_upper_bounds import class_records, owner_metadata
    from scripts.test_mobile_android_static_defaults import method_code
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from test_mobile_android_shared_upper_bounds import class_records, owner_metadata
    from test_mobile_android_static_defaults import method_code
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def project_enclosure(path: Path, *, marker: bool = False,
                      recursive: bool = False, object_bound: bool = False) -> dict:
    """Change only Signature/enclosure metadata; compare all Code attributes."""
    original = path.read_bytes()
    old_signature = (b"<T::Lfixture/Flag<+Lfixture/Value;>;>Ljava/lang/Object;Lfixture/Reader;" if marker
                     else b"<T:Lfixture/UpperBound;>Lfixture/Base;Lfixture/Reader<TT;>;")
    new_signature = b"Ljava/lang/Object;Lfixture/Reader;" if marker else b"Lfixture/Base;Lfixture/Reader<TT;>;"
    if recursive:
        old_signature = (b"<T:Ljava/lang/Object;K::Ljava/lang/Comparable<-TK;>;>"
                         b"Ljava/lang/Object;Lfixture/Reader<TT;Ljava/lang/Integer;>;")
        new_signature = b"Ljava/lang/Object;Lfixture/Reader<TT;Ljava/lang/Integer;>;"
    if object_bound:
        old_signature = old_signature.replace(b"Lfixture/UpperBound;", b"Ljava/lang/Object;")
    old = b"\x01" + len(old_signature).to_bytes(2, "big") + old_signature
    new = b"\x01" + len(new_signature).to_bytes(2, "big") + new_signature
    if original.count(old) != 1:
        raise RuntimeError("Expected one owned Worker class Signature")
    data = original.replace(old, new, 1)
    shape = class_records(data)
    count, strings, classes = shape["count"], shape["strings"], shape["classes"]
    this_class, pool_end = shape["this_class"], shape["pool_end"]
    class_attributes, attribute_count, rows = shape["class_attributes"], shape["attribute_count"], shape["rows"]
    owners = [key for key, value in classes.items() if strings[value] == b"fixture/Owner"]
    if (len(owners) != 1 or strings[classes[this_class]] != b"fixture/Owner$Worker"
            or any(key == b"EnclosingMethod" for key, _, _ in rows)):
        raise RuntimeError("Owned Worker/Owner enclosure changed")
    owner = owners[0]
    tail = bytearray(data[pool_end:])
    targets = 0
    for name, start, stop in rows:
        if name != b"InnerClasses":
            continue
        size = int.from_bytes(data[start + 6:start + 8], "big")
        if stop != start + 8 + 8 * size:
            raise RuntimeError("Unexpected owned InnerClasses table size")
        for i in range(size):
            at = start + 8 + i * 8
            inner, outer, short_name, flags = struct.unpack_from(">HHHH", data, at)
            if inner != this_class:
                continue
            if outer != owner or strings[short_name] != b"Worker" or flags != 25:
                raise RuntimeError("Owned Worker nesting flags or identity changed")
            struct.pack_into(">HH", tail, at + 2 - pool_end, 0, 0)
            targets += 1
    if targets != 1:
        raise RuntimeError("Missing unique owned Worker InnerClasses entry")
    struct.pack_into(">H", tail, class_attributes - pool_end, attribute_count + 1)
    records = b""
    prototype = (b"(Lfixture/Selector;Ljava/lang/Comparable;)Ljava/lang/Object;" if recursive
                 else b"(Lfixture/UpperBound;JLfixture/Sink;)Lfixture/Base;")
    if object_bound:
        prototype = prototype.replace(b"Lfixture/UpperBound;", b"Ljava/lang/Object;")
    for value in (b"EnclosingMethod", b"make", prototype):
        records += b"\x01" + len(value).to_bytes(2, "big") + value
    records += struct.pack(">BHH", 12, count + 1, count + 2)
    enclosing = struct.pack(">HIHH", count, 4, owner, count + 3)
    changed = data[:8] + (count + 4).to_bytes(2, "big") + data[10:pool_end] + records + tail + enclosing
    before_code, after_code = method_code(original), method_code(changed)
    if before_code != after_code:
        raise RuntimeError("Captured enclosure edit changed original Code attributes or descriptors")
    path.write_bytes(changed)
    return {"class_signature": new_signature.decode(), "enclosing_method": "fixture.Owner.make" + prototype.decode(),
            "inner_name": "absent", "original_method_bytecode": "unchanged",
            "before_sha256": hashlib.sha256(original).hexdigest(),
            "after_sha256": hashlib.sha256(changed).hexdigest(),
            "original_code_sha256": {key.decode(): hashlib.sha256(code).hexdigest()
                                     for key, code in before_code.items()}}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--method-scope",action="store_true")
    parser.add_argument("--object-bound",action="store_true")
    parser.add_argument("--static-result",action="store_true")
    args=parser.parse_args()
    if args.object_bound and not args.method_scope:
        parser.error("--object-bound requires --method-scope")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-projected-capture-super-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"projected_capture_super/java"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("ProjectedCaptureSuperCaller","ProjectedCaptureSuperHarness")]
        bound="Ljava/lang/Object;" if args.object_bound else "Lfixture/UpperBound;"
        if args.method_scope or args.static_result:
            variant=work/"fixture-sources"
            for original in sorted(fixture.rglob("*.java")):
                code=original.read_text()
                if original.name=="Owner.java" and args.method_scope:
                    anchor="public Base make("
                    if code.count(anchor)!=1:raise RuntimeError("Original generic factory anchor changed")
                    code=code.replace(anchor,"public static <T extends UpperBound> Base make(")
                if args.static_result:
                    if original.name=="Owner.java":
                        anchor="super(1,(Sink)completion);"
                        if code.count(anchor)!=1:raise RuntimeError("Original static-result argument anchor changed")
                        code=code.replace(anchor,"super(1,(Sink)Trace.take());")
                    elif original.name=="Base.java":
                        code=code.replace("public final Sink<Object> completion;","public final Sink<? super Base> completion;")
                        code=code.replace("public Base(int arity,Sink<Object> completion)","public Base(int arity,Sink<? super Base> completion)")
                    elif original.name=="Trace.java":
                        anchor="public static long events;"
                        if code.count(anchor)!=1:raise RuntimeError("Original static-result observer anchor changed")
                        code=code.replace(anchor,anchor+'\n    public static Sink<Object> incoming;\n    @SuppressWarnings("rawtypes")\n    public static Sink take() { hit(7); return incoming; }')
                if args.object_bound and original.name!="UpperBound.java":code=code.replace("UpperBound","Object")
                destination=variant/original.relative_to(fixture)
                destination.parent.mkdir(parents=True,exist_ok=True);destination.open("x").write(code)
            fixture=variant
            frozen=[]
            for original in harness_sources:
                code=original.read_text()
                if args.object_bound:code=code.replace("UpperBound","Object")
                if original.name=="ProjectedCaptureSuperHarness.java":
                    if args.method_scope:
                        anchor='findVirtual(Owner.class,"make",'
                        tail='.invokeExact(owner,token,wide,completion)'
                        if code.count(anchor)!=1 or code.count(tail)!=1:raise RuntimeError("Original static factory observer anchor changed")
                        code=code.replace(anchor,'findStatic(Owner.class,"make",').replace(tail,'.invokeExact(token,wide,completion)')
                    if args.static_result:
                        edits={
                            'failures={0,0,1,2,3,4,5,6,0}':'failures={0,0,1,2,3,4,5,6,0,7}',
                            'traces={1426435,1426435,1,142,142643,14,1426435,1426,142}':'traces={71426435,71426435,71,7142,7142643,714,71426435,71426,7142,7}',
                            'Trace.failAt=0;\n            if(route==4)':'Trace.failAt=0;Trace.incoming=callback;\n            if(route==4)',
                            'Trace.events=0;':'Trace.incoming=incoming;Trace.events=0;',
                            'require(Trace.events==traces[mode] && Trace.partial instanceof Base);':
                                'require(Trace.events==traces[mode]);\n            if(mode==9) { require(Trace.partial==null && Trace.seen==null && Trace.callback==null);\n                System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+traces[mode]);continue; }\n            require(Trace.partial instanceof Base);'}
                        for anchor,replacement in edits.items():
                            if code.count(anchor)!=1:raise RuntimeError("Original getter-effect observer anchor changed")
                            code=code.replace(anchor,replacement)
                destination=work/"harness-sources"/original.name
                destination.parent.mkdir(exist_ok=True);destination.open("x").write(code);frozen.append(destination)
            harness_sources=frozen
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=25)
        verify.compile_local(harness_sources,
            harness,classpath=originals,release=8)
        witness=project_enclosure(originals/"fixture/Owner$Worker.class",object_bound=args.object_bound)
        owner_witness=owner_metadata(originals/"fixture/Owner.class")
        (work/"owner-enclosure-witness.json").write_text(json.dumps(owner_witness,indent=2)+"\n")
        (work/"original-enclosure-witness.json").write_text(json.dumps(witness,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=7 or len(all_methods)!=(14 if args.object_bound else 15)+int(args.static_result):raise RuntimeError("Original projected capture super inventory changed")
        classes=all_classes-({"Lfixture/Base;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/"owned-provider.jar"
        parent_bytes=(originals/"fixture/Base.class").read_bytes()
        if int.from_bytes(parent_bytes[6:8],"big")!=69:raise RuntimeError("Original Java25 parent version changed")
        declaration_bytes=parent_bytes[:6]+(61).to_bytes(2,"big")+parent_bytes[8:]
        if method_code(parent_bytes)!=method_code(declaration_bytes):raise RuntimeError("Declaration version changed parent Code")
        (work/"parent-declaration-witness.json").write_text(json.dumps({"original_major":69,"declaration_major":61,
            "only_major_version_bytes_changed":True,"Code_descriptors_and_all_other_attributes":"unchanged",
            "original_sha256":hashlib.sha256(parent_bytes).hexdigest(),"declaration_sha256":hashlib.sha256(declaration_bytes).hexdigest()},indent=2)+"\n")
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Base.class",declaration_bytes)
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(5):
                for mode in range(10 if args.static_result else 9):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"ProjectedCaptureSuperHarness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original projected capture super matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original projected capture super DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original projected capture super entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"]!="partial"
                or not coverage.get("class_source_bindings") or not coverage.get("generic_signature_projections")):
            raise RuntimeError("Projected capture super recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
        bindings=coverage["class_source_bindings"]
        if len(bindings)!=1 or bindings[0]["class"]!="Lfixture/Owner$Worker;":
            raise RuntimeError("Missing exactly one shared source class binding")
        binding=bindings[0]
        if binding.get("binding_kind")!="anonymous-to-shared-member" or binding.get("source_enclosing_class")!="Lfixture/Owner;":
            raise RuntimeError("Unproven shared class source owner")
        old="Lfixture/Owner$Worker;";new="Lfixture/Owner$"+binding["source_name"]+";"
        def mapped(value):return value.replace(old,new)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={mapped(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={mapped(c)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                          if c not in {"Lfixture/Sink;","Lfixture/Reader;"}}
        if actual_classes!={mapped(c) for c in classes} or set(actual_methods)!={mapped(m) for m in methods}|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Original class/method/helper source inventory changed")
        projected={r["declaration"]:r for r in coverage["generic_signature_projections"]}
        expected_projected={old,old+"->token:"+bound,
                            old+"-><init>("+bound+"JLfixture/Sink;)V",old+"->read()"+bound}
        if set(projected)!=expected_projected:raise RuntimeError("Unrelated generic signature was projected")
        origin="Lfixture/Owner;->make("+bound+"JLfixture/Sink;)Lfixture/Base;" if args.method_scope else "Lfixture/Owner;"
        kind="shared-method-type-variable" if args.object_bound else "shared-method-class-upper-bound" if args.method_scope else "shared-class-upper-bound"
        for identity,row in projected.items():
            changes=row["changes"]
            if len(changes)!=1 or changes[0].get("kind")!=kind or changes[0].get("original_type")!=origin+"::T" or changes[0].get("source_type")!=bound or changes[0].get("scope_origin")!=(None if args.object_bound else origin):
                raise RuntimeError("Unproven shared capture upper bound projection")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            actual=after[mapped(c)];record=records[c]
            for key in ("access","signature","superclass","interfaces","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                wanted=record.get(key)
                if c==old and key=="signature":wanted="Lfixture/Base;Lfixture/Reader<"+bound+">;"
                if c==old and key=="inner_class":wanted={"inner":new,"outer":"Lfixture/Owner;","name":binding["source_name"],"access":25}
                if c==old and key=="enclosing_method":wanted=None
                if wanted!=actual.get(key):raise RuntimeError("Undisclosed class metadata change: "+c+":"+key)
            for field,metadata in record["fields"].items():
                wanted=copy.deepcopy(metadata)
                if field==old+"->token:"+bound:wanted["signature"]=None
                if wanted!=actual["fields"].get(mapped(field)):raise RuntimeError("Undisclosed field metadata change: "+field)
            for method,metadata in record["methods"].items():
                wanted=copy.deepcopy(metadata)
                if c==old and wanted["signature"]:
                    wanted["signature"]=wanted["signature"].replace("TT;",bound).replace(old,new)
                    if wanted["signature"]=="()"+bound:wanted["signature"]=None
                if wanted!=actual["methods"].get(mapped(method)) or after_exceptions[mapped(method)]!=exceptions[method]:
                    raise RuntimeError("Undisclosed method metadata/Exceptions change: "+method)
        (work/"metadata-reconciliation.json").write_text(json.dumps({"all_original_definitions":True,
            "only_reported_shared_name_nesting_and_four_upper_bound_projections":True,
            "all_original_access_flags_and_exceptions":True,"constructor_consumer_slot":"unchanged"},indent=2)+"\n")
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Projected capture super recovery changed seed identities, converter and callback order, return identity or failures")
        mutations=(("lost-token","this.token=token;","this.token=null;",0),
                   ("lost-wide","this.wide=wide;","this.wide=(int)wide;",0),
                   ("late-capture","this.token=token;this.wide=wide;\n            super(1,(Sink)completion);",
                    "super(1,(Sink)completion);\n            this.token=token;this.wide=wide;",0))
        if args.static_result:
            mutations=tuple((label,before.replace("(Sink)completion","(Sink)Trace.take()"),
                             after.replace("(Sink)completion","(Sink)Trace.take()"),mode)
                            for label,before,after,mode in mutations)
            mutations+=(("skipped-getter","super(1,(Sink)Trace.take());","super(1,(Sink)Trace.incoming);",0),
                        ("duplicated-getter","super(1,(Sink)Trace.take());","super(1,(Sink)(Trace.take()==null?null:Trace.take()));",0))
        original=(fixture/"fixture/Owner.java").read_text()
        for label,before,after,mode in mutations:
            if original.count(before)!=1:raise RuntimeError("Original projected capture counterexample anchor changed")
            changed=work/label/"Owner.java";changed.parent.mkdir();changed.write_text(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=25)
            project_enclosure(wrong/"fixture/Owner$Worker.class",object_bound=args.object_bound)
            owner_metadata(wrong/"fixture/Owner.class")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                "ProjectedCaptureSuperHarness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned projected capture super" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect capture: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "method_scope":args.method_scope,"object_bound":args.object_bound,"static_result":args.static_result,
            "unchanged_self_consumer_super_slot":"matched","metadata":"matched only reported shared name, nesting and upper bound projections",
            "captured_token_wide_parent_virtual_dispatch_partial_fields_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
