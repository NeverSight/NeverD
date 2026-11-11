#!/usr/bin/env python3
"""Verify independent parent carrier and directly seeded child result formals."""
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
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--bounded-carrier",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-split-seed-factory-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"split_seed_factory/java"
        if args.bounded_carrier:
            authored=work/"authored-fixture"
            shutil.copytree(fixture,authored)
            fixture=authored
            (fixture/"fixture/Bound.java").write_text("package fixture;\npublic class Bound {}\n")
            (fixture/"fixture/Link.java").write_text("package fixture;\npublic final class Link<T,V extends Bound> {}\n")
            provider=fixture/"fixture/Provider.java"
            text=provider.read_text()
            anchors=(("<S,T>","<S,T,V extends Bound>"),
                     ("T second, String label","T second, Link<T,V> link, String label"),
                     ("Trace.parent = parent;","Trace.link = link;\n        Trace.parent = parent;"))
            for before,after in anchors:
                if text.count(before)!=1:raise RuntimeError("Original bounded carrier source anchor changed")
                text=text.replace(before,after)
            provider.write_text(text)
            client=fixture/"fixture/Client.java"
            text=client.read_text().replace("Object second, String label","Object second, Link link, String label")
            text=text.replace("parent, first, second, label, mode","parent, first, second, link, label, mode")
            client.write_text(text)
            trace=fixture/"fixture/Trace.java"
            trace.write_text(trace.read_text().replace("public final class Trace {","public final class Trace {\n    public static Object link;"))
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("SplitSeedFactoryCaller","SplitSeedFactoryHarness")]
        if args.bounded_carrier:
            adapted=work/"authored-harness";adapted.mkdir()
            changed=[]
            for original in harness_sources:
                text=original.read_text()
                text=text.replace("Object second, String label","Object second, Link link, String label")
                text=text.replace("parent, first, second, label, mode","parent, first, second, link, label, mode")
                text=text.replace("Object.class,String.class,int.class","Object.class,Link.class,String.class,int.class")
                text=text.replace("parent,first,second,label,mode","parent,first,second,link,label,mode")
                text=text.replace("String label=mode==12?null:","Link link=mode==12?null:new Link<Object,Bound>();\n            String label=mode==12?null:")
                text=text.replace("Trace.events=0;Trace.parent=Trace.created=null;","Trace.events=0;Trace.link=null;Trace.parent=Trace.created=null;")
                text=text.replace("mode==3?null:parent,first,second,label,kind","mode==3?null:parent,first,second,link,label,kind")
                text=text.replace("require(formals.length==2", "require(formals.length==3")
                marker="            if(mode!=4)require(Trace.parent=="
                if original.stem=="SplitSeedFactoryHarness":
                    if text.count(marker)!=1:raise RuntimeError("Original bounded input observer anchor changed")
                    text=text.replace(marker,"            require(Trace.link==(mode==4?null:link));\n"+marker)
                    marker="        int[] failures="
                    if text.count(marker)!=1:raise RuntimeError("Original bounded formal observer anchor changed")
                    text=text.replace(marker,
                        "        require(formals[2].getBounds().length==1 && formals[2].getBounds()[0]==Bound.class);\n"
                        "        Type[] linkSlots=((ParameterizedType)factory.getGenericParameterTypes()[3]).getActualTypeArguments();\n"
                        "        require(linkSlots.length==2 && linkSlots[0].equals(formals[1]) && linkSlots[1].equals(formals[2]));\n"+marker)
                output=adapted/original.name;output.write_text(text);changed.append(output)
            harness_sources=changed
        verify.compile_local(harness_sources,
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(6 if args.bounded_carrier else 4) or len(all_methods)!=(11 if args.bounded_carrier else 9):raise RuntimeError("Original split-seed inventory changed")
        classes=all_classes-({"Lfixture/Provider;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Provider;->create(Lfixture/Carrier;Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/String;I)Lfixture/Carrier;"
        signature="<S:Ljava/lang/Object;T:Ljava/lang/Object;>(Lfixture/Carrier<TS;>;TT;TT;Ljava/lang/String;I)Lfixture/Carrier<TT;>;"
        if args.bounded_carrier:
            factory=factory.replace("Ljava/lang/String;I)","Lfixture/Link;Ljava/lang/String;I)")
            signature=signature.replace("T:Ljava/lang/Object;>","T:Ljava/lang/Object;V:Lfixture/Bound;>")
            signature=signature.replace("TT;TT;Ljava/lang/String;","TT;TT;Lfixture/Link<TT;TV;>;Ljava/lang/String;")
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original separate parent/child binding relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            jar.writestr("fixture/Provider.class",(originals/"fixture/Provider.class").read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(14):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"SplitSeedFactoryHarness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original split-seed matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original split-seed DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original split-seed entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Split-seed recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Split-seed recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original split-seed class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original split-seed method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Split-seed recovery changed parent, child, fields, result identity or failures")
        mutations=(("swapped-seeds","Provider","new Carrier<T>(first, second)","new Carrier<T>(second, first)",0),
                   ("replaced-result","Provider","return (Carrier<T>)Trace.other;","return new Carrier<T>(first, second);",10),
                   ("early-first-write","Carrier","Trace.hit(4);","this.first = first; Trace.hit(4);",6))
        if args.bounded_carrier:
            mutations += (("lost-link","Provider","Trace.link = link;","Trace.link = null;",0),)
        for label,name,old,new,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(old)!=1:raise RuntimeError("Split-seed counterexample anchor changed")
            if label=="early-first-write":text=text.replace("        this.first = first;\n","")
            wrong=work/label/("fixture/"+name+".java");wrong.parent.mkdir(parents=True);wrong.write_text(text.replace(old,new))
            variant=work/(label+"-classes");verify.compile_local([wrong],variant,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness))),"SplitSeedFactoryHarness",
                "1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned split seed factory" not in result.stderr:raise RuntimeError("Frozen observer missed "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"dependency_provider":args.dependency_provider,
            "fresh_jvm_cases_per_stage":len(baseline),"stages":2,"attempts_per_case":2,"separate_parent_and_child_formals":"retained",
            "bounded_carrier_input":args.bounded_carrier,
            "original_seed_parent_and_return_identity":"matched","constructor_publication_partial_fields_and_failures":"matched",
            "metadata_and_exceptions":"unchanged","independent_counterexamples":len(mutations),"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
