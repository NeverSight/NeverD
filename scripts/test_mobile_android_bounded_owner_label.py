#!/usr/bin/env python3
"""Verify bounded owner raw constructor forwards its exact seeds, converter and label."""
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
import subprocess
import tempfile
import zipfile

try:
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--mask",action="store_true")
    parser.add_argument("--interface-owner",action="store_true")
    parser.add_argument("--bounded-interface",action="store_true")
    args=parser.parse_args()
    if args.interface_owner and not args.mask:parser.error("--interface-owner requires --mask")
    if args.bounded_interface and not args.interface_owner:parser.error("--bounded-interface requires --interface-owner")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-owner-label-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"bounded_owner_label/java"
        harness_sources=[FIXTURES/"harness"/(name+".java") for name in ("BoundedOwnerLabelCaller","BoundedOwnerLabelHarness")]
        mask_prototype="(Ljava/lang/Object;Lfixture/Link;Ljava/lang/Object;Ljava/lang/String;ILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        if args.mask:
            authored=work/"authored-fixture";shutil.copytree(fixture,authored);fixture=authored
            marker=fixture/"kotlin/jvm/internal/DefaultConstructorMarker.java";marker.parent.mkdir(parents=True)
            marker.write_text("package kotlin.jvm.internal;\npublic final class DefaultConstructorMarker { private DefaultConstructorMarker() {} }\n")
            core=fixture/"fixture/Core.java";text=core.read_text();at=text.rfind("}")
            text=text[:at]+"""    @SuppressWarnings("unchecked")
    public Core(Object seed, Link converter, Object threshold, String label, int mask,
                kotlin.jvm.internal.DefaultConstructorMarker marker) {
        this((T)seed,(Link<T,V>)converter,(mask&4)!=0?null:(T)threshold,
             (mask&8)!=0?"owned default label":label);
    }
"""+text[at:];core.write_text(text)
            client=fixture/"fixture/Client.java";text=client.read_text()
            text=text.replace("Link converter,Object threshold)","Link converter,Object threshold,String label,int mask)")
            text=text.replace("(seed,converter,threshold);","(seed,converter,threshold,label,mask,null);");client.write_text(text)
            adapted=work/"authored-harness";adapted.mkdir();changed=[]
            for original in harness_sources:
                text="import kotlin.jvm.internal.DefaultConstructorMarker;\n"+original.read_text()
                text=text.replace("Link converter,Object threshold)","Link converter,Object threshold,String label,int mask)")
                text=text.replace("Client.make(seed,converter,threshold)","Client.make(seed,converter,threshold,label,mask)")
                text=text.replace("BoundedOwnerLabelCaller.make(seed,converter,threshold)","BoundedOwnerLabelCaller.make(seed,converter,threshold,label,mask)")
                text=text.replace("new Core<Object,Bound>(seed,converter,threshold)","new Core<Object,Bound>(seed,converter,threshold,label,mask,null)")
                text=text.replace("getConstructor(Object.class,Link.class,Object.class)","getConstructor(Object.class,Link.class,Object.class,String.class,int.class,DefaultConstructorMarker.class)")
                text=text.replace("newInstance(seed,converter,threshold)","newInstance(seed,converter,threshold,label,mask,null)")
                text=text.replace("MethodType.methodType(void.class,Object.class,Link.class,Object.class)","MethodType.methodType(void.class,Object.class,Link.class,Object.class,String.class,int.class,DefaultConstructorMarker.class)")
                text=text.replace("invokeExact(seed,converter,threshold)","invokeExact(seed,converter,threshold,label,mask,(DefaultConstructorMarker)null)")
                if original.stem=="BoundedOwnerLabelHarness":
                    text=text.replace("Trace.failAt=mode>=4?mode-3:0;","Trace.failAt=mode>=4&&mode<8?mode-3:0;")
                    text=text.replace("Core result=null;","int mask=(mode%4)<<2; String label=mode>=8?null:new String(\"owned incoming label\");\n            Core result=null;")
                    text=text.replace("make(route,seed,converter,threshold)","make(route,seed,converter,threshold,label,mask)")
                    text=text.replace("mode==4||mode==5?null:threshold","mode==4||mode==5||(mask&4)!=0?null:threshold")
                    text=text.replace('mode==4||mode==5||mode==6?null:"owned label"','mode==4||mode==5||mode==6?null:(mask&8)!=0?"owned default label":label')
                dest=adapted/original.name;dest.write_text(text);changed.append(dest)
            harness_sources=changed
        if args.interface_owner:
            core=fixture/"fixture/Core.java";text=core.read_text()
            text=text.replace("public final class Core<T,V extends Bound> {", "public final class Core<T,V extends Bound> implements View<T> {")
            at=text.rfind("}")
            text=text[:at]+"    public T value() { return seed; }\n"+text[at:];core.write_text(text)
            shutil.copyfile(FIXTURES/"bounded_owner_label/java_interface/fixture/View.java",fixture/"fixture/View.java")
            for source in harness_sources:
                if source.stem=="BoundedOwnerLabelHarness":
                    text=source.read_text().replace("if(Trace.failAt==0)require(result==partial && result!=previous);",
                        "if(Trace.failAt==0)require(result==partial && result!=previous && ((View)result).value()==seed);")
                    text=text.replace("Constructor<?> target=", "Type[] edges=Core.class.getGenericInterfaces();require(edges.length==1 && ((ParameterizedType)edges[0]).getRawType()==View.class && ((ParameterizedType)edges[0]).getActualTypeArguments()[0].equals(formals[0]));\n        Constructor<?> target=")
                    source.write_text(text)
        if args.bounded_interface:
            core=fixture/"fixture/Core.java";text=core.read_text()
            text=text.replace("implements View<T>","implements View<V>")
            text=text.replace("public T value() { return seed; }","public V value() { return null; }")
            core.write_text(text)
            for source in harness_sources:
                if source.stem=="BoundedOwnerLabelHarness":
                    text=source.read_text().replace("((View)result).value()==seed","((View)result).value()==null")
                    text=text.replace("getActualTypeArguments()[0].equals(formals[0])","getActualTypeArguments()[0].equals(formals[1])")
                    source.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        raw_prototype="(Ljava/lang/Object;Lfixture/Link;Ljava/lang/Object;)V"
        flag_receipt=change_method_flags(originals/"fixture/Core.class",raw_prototype.encode(),1,0x1001,name=b"<init>")
        (work/"synthetic-flag-witness.json").write_text(json.dumps(flag_receipt,indent=2)+"\n")
        mask_receipt=change_method_flags(originals/"fixture/Core.class",mask_prototype.encode(),1,0x1001,name=b"<init>") if args.mask else None
        if mask_receipt:(work/"mask-flag-witness.json").write_text(json.dumps(mask_receipt,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        (work/"baseline-cli-sha256.txt").open("x").write(hashlib.sha256(args.neverd.read_bytes()).hexdigest()+"\n")
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(7 if args.interface_owner else 6 if args.mask else 5) or len(all_methods)!=(13 if args.bounded_interface else 12 if args.interface_owner else 10 if args.mask else 8):raise RuntimeError("Original bounded label constructor inventory changed")
        classes=all_classes-({"Lfixture/Link;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        raw="Lfixture/Core;-><init>"+raw_prototype
        if records["Lfixture/Core;"]["methods"][raw]["signature"] is not None:
            raise RuntimeError("Original raw constructor unexpectedly carries a Signature")
        projected_constructors={raw}
        if args.mask:projected_constructors.add("Lfixture/Core;-><init>"+mask_prototype)
        projected_bridges={"Lfixture/Core;->value()Ljava/lang/Object;"} if args.bounded_interface else set()
        expected_projections=projected_constructors|projected_bridges
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Link.class",(originals/"fixture/Link.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label,synthetic):
            outputs=[]
            for route in range(4):
                for mode in range(16 if args.mask else 8):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"BoundedOwnerLabelHarness",
                        str(route),str(mode),str(synthetic).lower()],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original bounded label constructor matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline",True)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original bounded label constructor DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original bounded label constructor entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0)!=len(expected_projections) or coverage["status"]!="partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Bounded label constructor recovery changed original entries or metadata contracts")
        projected={r["identity"] for r in coverage["methods"] if r["status"]=="source-projected"}
        if projected!=expected_projections:raise RuntimeError("Only original constructors and the exact compiler bridge may be projected")
        for identity in projected_constructors:
            row=next(r for r in coverage["methods"] if r["identity"]==identity)
            if row.get("projection_kind")!="synthetic-constructor" or row.get("source_identity")!=identity or len(row.get("metadata_differences",[]))!=1:
                raise RuntimeError("Missing exact original synthetic constructor disclosure")
        for identity in projected_bridges:
            row=next(r for r in coverage["methods"] if r["identity"]==identity)
            if (row.get("projection_kind")!="generic-interface-bridge"
                    or row.get("bridge_dispatch")!="Lfixture/View;->value()Ljava/lang/Object;"
                    or row.get("bridge_target")!="Lfixture/Core;->value()Lfixture/Bound;"):
                raise RuntimeError("Missing exact wider-interface compiler bridge role")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/View;" }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Bounded label constructor recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original bounded label constructor class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                wanted=copy.deepcopy(metadata)
                if identity in projected_constructors:wanted["access"] &= ~0x1000
                if after[c]["methods"].get(identity)!=wanted or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original bounded label constructor method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt",False)!=baseline:raise RuntimeError("Bounded label constructor recovery changed seed identities, converter and callback order, return identity or failures")
        mutations=(("wrong-label","threshold,\"owned label\"","threshold,\"wrong label\"",0),
                   ("swapped-seeds","this((T)seed,","this((T)threshold,",0),
                   ("early-seed","Trace.partial=this;Trace.hit(1);","Trace.partial=this;this.seed=seed;Trace.hit(1);",4))
        if args.mask:
            mutations=(("wrong-default-label",'"owned default label"','"wrong default label"',2),
                       ("swapped-seeds","this.seed=seed;Trace.hit(2);","this.seed=threshold;Trace.hit(2);",0),
                       ("early-seed","Trace.partial=this;Trace.hit(1);","Trace.partial=this;this.seed=seed;Trace.hit(1);",4),
                       ("lost-threshold-mask","(mask&4)!=0?null:(T)threshold","(T)threshold",1))
        if args.bounded_interface:
            mutations+=(("wrong-interface-value","public V value() { return null; }","public V value() { return (V)new Bound(); }",0),)
        elif args.interface_owner:
            mutations+=(("wrong-interface-value","public T value() { return seed; }","public T value() { return threshold; }",0),)
        original=(fixture/"fixture/Core.java").read_text()
        for label,before,after,mode in mutations:
            text=original
            if text.count(before)!=1:raise RuntimeError("Original constructor counterexample anchor changed")
            if label=="early-seed":text=text.replace("this.seed=seed;Trace.hit(2);","Trace.hit(2);")
            changed=work/label/"Core.java";changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            change_method_flags(wrong/"fixture/Core.class",raw_prototype.encode(),1,0x1001,name=b"<init>")
            if args.mask:change_method_flags(wrong/"fixture/Core.class",mask_prototype.encode(),1,0x1001,name=b"<init>")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                "BoundedOwnerLabelHarness","1",str(mode),"true"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned bounded label constructor" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect constructor: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,"mask":args.mask,"interface_owner":args.interface_owner,"bounded_interface":args.bounded_interface,
            "bounded_owner_and_constant_label_relation":"matched","all_original_entries":"unchanged","metadata":"unchanged except disclosed synthetic constructor flag",
            "seed_converter_identity_nulls_partial_final_fields_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
