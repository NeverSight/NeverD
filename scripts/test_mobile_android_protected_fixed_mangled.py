#!/usr/bin/env python3
"""Verify protected mangled calls retain concrete callback signatures and effects."""
from __future__ import annotations
import argparse
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_static_defaults import method_code


def patch_name(path: Path) -> int:
    before=path.read_bytes();after=bytearray(before)
    if before[:4]!=b"\xca\xfe\xba\xbe":raise RuntimeError("Not an owned class file")
    count=int.from_bytes(before[8:10],"big");index=1;offset=10;changed=0
    old,new=b"chooseValue",b"choose-Name"
    if len(old)!=len(new):raise RuntimeError("Method name patch must preserve UTF8 length")
    while index<count:
        tag=before[offset];offset+=1
        if tag==1:
            size=int.from_bytes(before[offset:offset+2],"big");offset+=2
            if before[offset:offset+size]==old:
                after[offset:offset+size]=new;changed+=1
            offset+=size
        elif tag in (3,4,9,10,11,12,17,18):offset+=4
        elif tag in (5,6):offset+=8;index+=1
        elif tag in (7,8,16,19,20):offset+=2
        elif tag==15:offset+=3
        else:raise RuntimeError("Unknown owned constant-pool tag")
        index+=1
    expected={name.replace(old,new,1) if name.startswith(old+b"(") else name:code for name,code in method_code(before).items()}
    if before[offset:]!=after[offset:] or method_code(bytes(after))!=expected:
        raise RuntimeError("Name encoding changed original Code or attributes")
    if changed:path.write_bytes(after)
    return changed


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-base",action="store_true")
    parser.add_argument("--enclosing",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-fixed-mangled-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"protected_fixed_mangled/java"
        harness_name="ProtectedFixedMangled"
        harness_source=FIXTURES/"harness"/(harness_name+"Harness.java")
        if args.enclosing:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            owner=fixture/"other/Owner.java";text=owner.read_text()
            old="Trace.sink=sink;Trace.hit(1);"
            if text.count(old)!=1:raise RuntimeError("Original enclosure allocation anchor changed")
            owner.write_text(text.replace(old,"Trace.sink=sink;Trace.local=new Object() {};Trace.hit(1);"))
            trace=fixture/"fixture/Trace.java";text=trace.read_text()
            trace.write_text(text.replace("receiver,scope,sink,touched,accepted,result,output;","receiver,scope,sink,touched,accepted,result,output,local;"))
            harness_source=work/(harness_name+"Harness.java");text=(FIXTURES/"harness"/(harness_name+"Harness.java")).read_text()
            old="Object actual=null;Throwable caught=null;"
            text=text.replace(old,"Trace.local=null;"+old)
            old="require(Trace.constructed==1);\n            System.out.println"
            new='require(Trace.constructed==1);\n            require(Trace.local!=null);Class<?> local=Trace.local.getClass();\n            require(local.getEnclosingMethod()!=null&&local.getEnclosingMethod().getName().equals(name)&&local.getEnclosingMethod().getDeclaringClass()==Owner.class);\n            Field outer=local.getDeclaredField("this$0");outer.setAccessible(true);require(outer.get(Trace.local)==receiver);\n            System.out.println'
            if text.count(old)!=1:raise RuntimeError("Original enclosure observer anchor changed")
            harness_source.open("x").write(text.replace(old,new))
        authored,originals,harness=work/"authored",work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),authored,release=8)
        verify.compile_local([FIXTURES/"harness"/(harness_name+"Caller.java"),harness_source],
            harness,classpath=authored,release=8)
        authored_hashes=hashes(authored);before=class_identity.compiler_classes(authored)
        shutil.copytree(authored,originals)
        witness={}
        for path in sorted(originals.rglob("*.class")):
            changed=patch_name(path)
            if changed:witness[path.relative_to(originals).as_posix()]=changed
        expected_witness={"fixture/Child.class":1,"other/Owner.class":1,"other/Peer.class":1}
        if args.enclosing:expected_witness["other/Owner$1.class"]=1
        if witness!=expected_witness:
            raise RuntimeError("Original mangled-name encoding sites changed")
        (work/"name-encoding-witness.json").open("x").write(json.dumps({"UTF8_entries":witness,"Code_descriptors_and_all_other_attributes":"unchanged"},indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(9 if args.enclosing else 8) or len(all_methods)!=(13 if args.enclosing else 12):raise RuntimeError("Original protected fixed mangled inventory changed")
        declarations={"Lother/Base;"}
        classes=all_classes-(declarations if args.dependency_base else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        prototype="(Lapi/Press;JLapi/Sink;)Ljava/lang/Object;"
        original_name="Lother/Owner;->choose-Name"+prototype
        legal_name="Lother/Owner;->chooseValue"+prototype
        wanted=deepcopy(before)
        method=wanted["Lother/Owner;"]["methods"].pop(legal_name);method["name"]="choose-Name"
        wanted["Lother/Owner;"]["methods"][original_name]=method
        if args.enclosing:wanted["Lother/Owner$1;"]["enclosing_method"]["name"]="choose-Name"
        for owner in records:
            for key in wanted[owner]:
                if key!="sha256" and wanted[owner][key]!=records[owner].get(key):
                    raise RuntimeError("Name encoding changed other metadata: "+owner+":"+key)
        signature="(Lapi/Press;JLapi/Sink<-Lapi/Value;>;)Ljava/lang/Object;"
        if records["Lother/Owner;"]["methods"][original_name]["signature"]!=signature:
            raise RuntimeError("Original concrete contravariant callback signature changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("other/Base",):
                jar.writestr(name+".class",(originals/(name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_base:classpath += ["--classpath",dependency]
        def execute(runtime,label,spelling):
            outputs=[]
            for route in range(6):
                for mode in range(13):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                        str(route),str(mode),spelling],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original protected fixed mangled matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((authored,harness),"authored","chooseValue")
        if execute((originals,harness),"encoded","choose-Name")!=baseline:
            raise RuntimeError("Equal-length name encoding changed original behavior")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original protected fixed mangled DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original protected fixed mangled entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        mutations=(("wrong-result","other/Owner","Object result=Trace.output;","Object result=scope;",0),
                   ("lost-wide","fixture/Child","this.chooseValue(scope,bits,sink)","this.chooseValue(scope,(int)bits,sink)",0),
                   ("skipped-touch","other/Owner","scope.touch();Trace.hit(2);","Trace.hit(2);",0),
                   ("delayed-publication","other/Owner","Trace.result=result;Trace.hit(6);","Trace.hit(6);Trace.result=result;",10))
        for label,name,old,new,mode in mutations:
            text=(fixture/(name+".java")).read_text()
            if text.count(old)!=1:raise RuntimeError("Original protected fixed mangled counterexample anchor changed")
            changed=work/label/(name+".java");changed.parent.mkdir(parents=True);changed.open("x").write(text.replace(old,new))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=authored,release=8)
            if patch_name(wrong/(name+".class"))!=1:raise RuntimeError("Wrong program name encoding changed")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_name+"Harness","1",str(mode),"choose-Name"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned protected fixed mangled" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect protected fixed mangled: "+label)
        if args.baseline_only:
            if (hashes(authored)!=authored_hashes or hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                    or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                    or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
            evidence={"status":"baseline-only","class_count":len(classes),"method_count":len(methods),
                "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_base":args.dependency_base,"anonymous_enclosure":args.enclosing,
                "independent_wrong_programs":len(mutations),"original_Code_and_all_metadata":"unchanged except witnessed method name"}
            (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
            return 0
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0)!=(2 if args.enclosing else 1) or coverage["status"]!="partial"
                or bool(coverage.get("class_source_bindings"))!=args.enclosing or coverage.get("generic_signature_projections")):
            raise RuntimeError("Protected fixed mangled recovery changed original entries or metadata contracts")
        row=next(r for r in coverage["methods"] if r["identity"]==original_name)
        spelling=row.get("source_name");renamed="Lother/Owner;->"+str(spelling)+prototype
        if (row.get("projection_kind")!="virtual-method-identifier" or row.get("source_identity")!=renamed
                or row.get("source_name_declaration")!=original_name or row.get("source_name_family")!=[original_name]
                or spelling!="__neverdVirtualMethod0" or len(row.get("metadata_differences",[]))!=3):
            raise RuntimeError("Protected method identifier projection changed its exact scope")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_base else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        local="Lother/Owner$1;";local_constructor=local+"-><init>(Lother/Owner;)V"
        bindings=coverage.get("class_source_bindings",[]);class_names={}
        if args.enclosing:
            if len(bindings)!=1 or bindings[0]["class"]!=local or bindings[0]["binding_kind"]!="anonymous-to-named-local":raise RuntimeError("Missing exact anonymous enclosing binding")
            matches=[c for c in actual_classes if c.endswith(bindings[0]["source_name"]+";")]
            if len(matches)!=1:raise RuntimeError("Ambiguous rebuilt local class")
            class_names[local]=matches[0]
            expected={"identity":renamed,"owner":"Lother/Owner;","name":spelling,"prototype":prototype,"parameters":["Lapi/Press;","J","Lapi/Sink;"],"returns":"Ljava/lang/Object;"}
            if bindings[0].get("source_enclosing_method")!=expected:raise RuntimeError("Missing exact source EnclosingMethod projection")
            if not any("constructor parameters may differ" in d for d in bindings[0]["metadata_differences"]):raise RuntimeError("Missing original outer capture disclosure")
        def mapped(identity):
            if args.enclosing and identity==local_constructor:identity=local+"-><init>(Lother/Owner;Lother/Owner;)V"
            identity=identity.replace(original_name,renamed)
            for old,new in class_names.items():identity=identity.replace(old,new)
            return identity
        helpers={mapped(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={mapped(c)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lapi/Press;","Lapi/Sink;"} }
        if actual_classes!={mapped(c) for c in classes} or set(actual_methods)!={mapped(m) for m in methods}|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Protected fixed mangled recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                wanted=deepcopy(records[c].get(key))
                if args.enclosing and c==local:
                    if key=="inner_class":wanted["inner"]=mapped(c);wanted["name"]=bindings[0]["source_name"]
                    elif key=="enclosing_method":wanted["name"]=spelling
                    elif key=="fields":
                        wanted={mapped(k):v for k,v in wanted.items()}
                        field=wanted[mapped(local+"->this$0:Lother/Owner;")]
                        if len(wanted)!=1 or field["access"]!=0x1010:raise RuntimeError("Original outer capture fields changed")
                        field["access"] &= ~0x1000
                if wanted!=after[mapped(c)].get(key):raise RuntimeError("Original protected fixed mangled class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                selected=mapped(identity);wanted=deepcopy(metadata)
                if identity==original_name:wanted["name"]=spelling
                if args.enclosing and identity==local_constructor:wanted["prototype"]="(Lother/Owner;Lother/Owner;)V"
                if after[mapped(c)]["methods"].get(selected)!=wanted or after_exceptions[selected]!=exceptions[identity]:
                    raise RuntimeError("Original protected fixed mangled method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_base else (rebuilt,harness)
        if execute(runtime,"rebuilt",spelling)!=baseline:raise RuntimeError("Protected fixed mangled recovery changed receiver/input/result identity, ordered effects or failures")
        if (hashes(authored)!=authored_hashes or hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_base":args.dependency_base,"anonymous_enclosure":args.enclosing,
            "protected_final_mangled_name_and_contravariant_callback":"matched","all_original_entries_and_metadata":("matched with disclosed method, local-name, capture and constructor projections" if args.enclosing else "unchanged except disclosed method name"),
            "cross_and_same_package_access_wide_callback_receiver_result_identity_effects_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
