#!/usr/bin/env python3
"""Verify scalar carrier getter names with frozen Kotlin compiler observers."""
from __future__ import annotations
import argparse
from collections import Counter
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile

OWNER="Lfixture/Scalar;"
UNBOX=OWNER+"->unbox-impl()F"
def compiler_facts(root):
    return {r["name"]:r for p in sorted(root.rglob("*.class")) for r in [KotlinFixtureClassFile(p.read_bytes()).facts()]}

def source_hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--primitive", choices=("float", "long", "double"), default="float")
    parser.add_argument("--dependency-trace", action="store_true")
    args=parser.parse_args()
    descriptor={"float":"F","long":"J","double":"D"}[args.primitive]
    UNBOX=OWNER+"->unbox-impl()"+descriptor
    modes=12 if args.primitive=="float" else 16
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-carrier-accessor-name-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"carrier_accessor_name";kotlin=args.kotlin_lib.resolve()
        fixture_sources=sorted(p for p in fixture.rglob("*") if p.is_file())
        observer_sources=[FIXTURES/"harness"/(n+".java") for n in ("CarrierAccessorNameHarness","CarrierAccessorNameCaller")]
        inputs=[args.android_jar.resolve(),*sorted(kotlin.glob("*.jar")),*fixture_sources,*observer_sources]
        if not (kotlin/"kotlin-compiler.jar").is_file():raise RuntimeError("Original Kotlin compiler unavailable")
        input_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        dependencies=[]
        for p in (kotlin/"kotlin-stdlib.jar",kotlin/"annotations-13.0.jar"):
            dest=work/p.name
            with zipfile.ZipFile(p) as before,zipfile.ZipFile(dest,"x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe compiler dependency")
                        after.writestr(entry.filename,before.read(entry))
            dependencies.append(dest)
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        original_java,originals,harness=work/"original-java",work/"originals",work/"harness"
        authored=work/"authored-source";authored.mkdir()
        source=(fixture/"kotlin/fixture/Values.kt").read_text()
        source=source.replace("Float",args.primitive.capitalize())
        kotlin_source=authored/"Values.kt";kotlin_source.write_text(source)
        observers=work/"observer-source";observers.mkdir()
        for path in observer_sources:
            text=path.read_text()
            if args.primitive!="float":
                if path.stem=="CarrierAccessorNameHarness":
                    vector=re.search(r"  int\[\] bits=\{[^\n]+\};",text)
                    if not vector:raise RuntimeError("Original scalar observer vector changed")
                    values=("0x123456789abcdef0L,Long.MIN_VALUE,Long.MAX_VALUE,0x100000000L,0xffffffff00000000L,-1L,1L,2L,3L,4L,5L,6L,0L,0x7fffffffL,0x80000000L,0x100000001L" if args.primitive=="long" else
                            "0x3ff8000000000001L,0xbff8000000000000L,0x7ff8123456789abcL,0x8000000000000000L,0x7ff0000000000000L,0xfff0000000000000L,1L,0x7fffffffffffffffL,0x3ff0000000000000L,0x3ff0000000000000L,0x3ff0000000000000L,0x3ff0000000000000L,0L,0x8000000000000001L,0xfff923456789abcdL,0x0010000000000000L")
                    text=text[:vector.start()]+"  long[] bits={"+values+"};"+text[vector.end():]
                    text=text.replace("0x40000000", "2L" if args.primitive=="long" else "0x4000000000000000L")
                    if args.primitive=="long":
                        text=text.replace("Float.intBitsToFloat", "identityBits").replace("Float.floatToRawIntBits", "identityBits")
                        text=text.replace(" static void require", " static long identityBits(long value){return value;}\n static void require")
                    else:
                        text=text.replace("Float.intBitsToFloat", "Double.longBitsToDouble").replace("Float.floatToRawIntBits", "Double.doubleToRawLongBits")
                text=text.replace("float",args.primitive).replace("Float",args.primitive.capitalize())
            (observers/path.name).write_text(text)
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),original_java,release=8);originals.mkdir()
        compiler=[choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect",
                  "-classpath",os.pathsep.join(map(str,(kotlin/"kotlin-stdlib.jar",original_java))),"-jvm-target","1.8"]
        verify.run([*compiler,"-d",originals,kotlin_source],"kotlinc-original")
        for p in original_java.rglob("*.class"):
            dest=originals/p.relative_to(original_java);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        verify.compile_local(sorted(observers.glob("*.java")),harness,
            classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        all_classes,all_methods=compiler_inventory(verify,originals);all_methods=set(all_methods)
        records,exceptions=compiler_facts(originals),exception_inventory(originals)
        if len(all_classes)!=3 or UNBOX not in all_methods:raise RuntimeError("Original scalar class/getter inventory changed")
        classes=set(all_classes);methods=set(all_methods)
        if args.dependency_trace:
            provider=work/"trace-provider.jar"
            with zipfile.ZipFile(provider,"x") as archive:
                archive.write(originals/"fixture/Trace.class","fixture/Trace.class")
            dependencies.append(provider)
            classes.remove("Lfixture/Trace;")
            methods={identity for identity in methods if not identity.startswith("Lfixture/Trace;->")}
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        frozen={"originals":original_hashes,"harness":harness_hashes,"inputs":input_hashes,
                "authored_source":source_hashes(authored),"observer_source":source_hashes(observers)}
        (work/"frozen-input-hashes.json").write_text(json.dumps(frozen,indent=2)+"\n")
        def execute(runtime,name,label):
            outputs=[]
            for route in range(4):
                for mode in range(modes):
                    out=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),"CarrierAccessorNameHarness",str(route),str(mode),name],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(out.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in out.splitlines()):raise RuntimeError("Changed original scalar observer")
                    outputs.append(out)
            return outputs
        baseline=execute((originals,harness),"unbox-impl","baseline")
        (work/"baseline.json").write_text(json.dumps(baseline,indent=2)+"\n")
        classpath=["--classpath",args.android_jar.resolve()]
        for p in dependencies:classpath += ["--classpath",p]
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original carrier accessor name DEX count changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original carrier accessor name entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
        visible=Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations))
        if "VISIBILITY_BUILD" in annotations or visible!=Counter({"Lkotlin/Metadata;":2,"Lkotlin/jvm/JvmInline;":1}):raise RuntimeError("D8 changed carrier accessor name annotation retention")
        expected={c:deepcopy(records[c]) for c in classes};dropped={}
        for c in classes:
            for identity,row in [(c,expected[c]),*expected[c]["fields"].items(),*expected[c]["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    if row.get(key):dropped.setdefault(identity,{})[key]=row[key]
                    row[key]=[]
        (work/"d8-class-annotation-loss.json").write_text(json.dumps(dropped,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-getter-disassembly")
        getters=[]
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            name=re.search(r"(?m)^\s+name\s+: '([^']+)'",block[2]);proto=re.search(r"(?m)^\s+type\s+: '([^']+)'",block[2])
            if block[1]!=OWNER or not name or name[1]!="unbox-impl" or not proto or proto[1]!="()"+descriptor:continue
            registers=re.search(r"(?m)^\s+registers\s+:\s+(\d+)",block[2]);incoming=re.search(r"(?m)^\s+ins\s+:\s+(\d+)",block[2])
            instructions=re.findall(r"\|[0-9a-fA-F]+:\s+([^\n]+)",block[2])
            words=1 if args.primitive=="float" else 2
            read="iget" if words==1 else "iget-wide";ret="return" if words==1 else "return-wide"
            if (not registers or int(registers[1])!=words+1 or not incoming or int(incoming[1])!=1 or len(instructions)!=2
                    or not re.match(read+r" v0, v"+str(words)+r", Lfixture/Scalar;[.;]raw:"+descriptor,instructions[0])
                    or instructions[1]!=ret+" v0"):
                raise RuntimeError("Original scalar getter word/receiver proof changed")
            getters.append({"method":UNBOX,"registers":int(registers[1]),"incoming":1,"instructions":instructions})
        if len(getters)!=1:raise RuntimeError("Missing original scalar getter witness")
        (work/"input-method-witness.json").write_text(json.dumps({"dex_sha256":hashlib.sha256(source_dex.read_bytes()).hexdigest(),"getters":getters},indent=2)+"\n")
        source=kotlin_source.read_text()
        zero={"float":"0.0f","long":"0L","double":"0.0"}[args.primitive]
        counterexamples=[("lost-bits","Scalar(value)","Scalar("+zero+")",0),("reverse-order","raw.compareTo(other.raw)","other.raw.compareTo(raw)",0),("lost-effect","Trace.hit(2)","Trace.hit(3)",0),("lost-failure","Trace.hit(1)","if (Trace.failAt == 0) Trace.hit(1)",9)]
        if args.primitive=="long":
            counterexamples += [("narrowed-word","Scalar(value)","Scalar(value.toInt().toLong())",0),
                                ("swapped-words","Scalar(value)","Scalar(java.lang.Long.rotateLeft(value, 32))",0)]
        elif args.primitive=="double":
            counterexamples += [("narrowed-word","Scalar(value)","Scalar(value.toFloat().toDouble())",0),
                                ("canonicalized-nan","Scalar(value)","Scalar(java.lang.Double.longBitsToDouble(java.lang.Double.doubleToLongBits(value)))",2)]
        for label,old,new,mode in counterexamples:
            if source.count(old)!=1:raise RuntimeError("Scalar counterexample anchor changed")
            wrong=work/label/"Values.kt";wrong.parent.mkdir();wrong.write_text(source.replace(old,new));variant=work/(label+"-classes");variant.mkdir()
            verify.run([*compiler,"-d",variant,wrong],"kotlinc-"+label)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),"CarrierAccessorNameHarness","1",str(mode),"unbox-impl"],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned carrier accessor name" not in result.stderr:raise RuntimeError("Independent scalar observer missed "+label)
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["unrecovered_method_count"] or coverage["status"]!="partial":raise RuntimeError("Scalar recovery changed original entries")
        unbox=rows[UNBOX];name=unbox.get("source_name")
        if not name or unbox.get("projection_kind")!="virtual-method-identifier" or not unbox.get("metadata_differences"):raise RuntimeError("Scalar accessor lost exact source-name receipt")
        projections=coverage.get("generic_signature_projections",[])
        if len(projections)!=1 or projections[0]["declaration"]!=OWNER or projections[0]["original_signature"]!=records[OWNER]["signature"] or projections[0]["changes"][0]["kind"]!="checked-value-carrier-interface":raise RuntimeError("Missing exact scalar Comparable scope projection")
        rebuilt=work/"rebuilt";verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)),release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        def mapped(identity):return identity.split("->",1)[0]+"->"+rows[identity].get("source_name",rows[identity]["name"])+identity[identity.index("("):]
        if actual_classes!=classes or set(actual_methods)!={mapped(i) for i in methods}|helpers:raise RuntimeError("Scalar recovery omitted or invented an entry")
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if after[c].get(key)!=expected[c].get(key):raise RuntimeError("Undisclosed scalar class metadata change: "+c+":"+key)
            wanted_signature=None if c==OWNER else expected[c].get("signature")
            if after[c].get("signature")!=wanted_signature:raise RuntimeError("Scalar source interface signature changed")
            for identity,metadata in expected[c]["methods"].items():
                wanted=deepcopy(metadata);wanted["name"]=rows[identity].get("source_name",wanted["name"])
                if rows[identity].get("projection_kind")=="value-carrier-interface-bridge":
                    if not rows[identity].get("metadata_differences") or not rows[identity].get("bridge_target") or not rows[identity].get("unbox_methods"):raise RuntimeError("Missing exact checked scalar bridge flag receipt")
                    wanted["access"] &= ~0x1040
                if wanted["access"]&0x1000 and not wanted["access"]&0x40:
                    if not rows[identity].get("metadata_differences"):raise RuntimeError("Missing scalar synthetic flag disclosure")
                    wanted["access"] &= ~0x1000
                if after[c]["methods"].get(mapped(identity))!=wanted or after_exceptions[mapped(identity)]!=exceptions[identity]:raise RuntimeError("Undisclosed scalar method metadata or Exceptions change: "+identity)
        if execute((rebuilt,harness),name,"rebuilt")!=baseline:raise RuntimeError("Scalar recovery changed raw bits, comparison dispatch or failures")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}!=input_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}!=dependency_hashes:raise RuntimeError("Original inputs or frozen observers changed")
        if source_hashes(authored)!=frozen["authored_source"] or source_hashes(observers)!=frozen["observer_source"]:
            raise RuntimeError("Authored sources or frozen observer sources changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases_per_stage":len(baseline),"stages":2,"attempts_per_case":2,"original_getter_bits_virtual_bridge_dispatch_failures_and_Exceptions":"matched","metadata_after_exact_names_flags_and_Comparable_projection":"matched","compiler_CLASS_annotation_loss_sites":len(dropped),"independent_counterexamples":len(counterexamples),"primitive":args.primitive,"dependency_trace":args.dependency_trace,"original_code_bytes_and_frozen_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
