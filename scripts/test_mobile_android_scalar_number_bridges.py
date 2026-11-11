#!/usr/bin/env python3
"""Verify genuine Kotlin scalar superclass bridges with plain runtime headers."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from mobile_real_apps_android import dex_header, parse_dexdump


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):
        parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):
        parser.add_argument("--"+name,type=Path)
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scalar-number-bridges-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"scalar_number_bridges";kotlin=args.kotlin_lib.resolve()
        sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
        inputs=[args.android_jar.resolve(),*sorted(kotlin.glob("*.jar"))]
        if not (kotlin/"kotlin-compiler.jar").is_file(): raise RuntimeError("Unmodified Kotlin compiler distribution required")
        input_hashes={str(p):sha(p) for p in inputs}
        dependencies=[]
        for jar in (kotlin/"kotlin-stdlib.jar",kotlin/"annotations-13.0.jar"):
            dest=work/jar.name
            with zipfile.ZipFile(jar) as source,zipfile.ZipFile(dest,"x") as output:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if entry.filename.startswith("/") or ".." in Path(entry.filename).parts: raise RuntimeError("Unsafe compiler dependency entry")
                        output.writestr(entry.filename,source.read(entry))
            dependencies.append(dest)
        originals=work/"originals";harness=work/"harness";originals.mkdir()
        verify.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib","-no-reflect","-classpath",kotlin/"kotlin-stdlib.jar","-jvm-target","1.8","-d",originals,
                    *sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        verify.compile_local(sorted((fixture/"harness").glob("*.java")),harness,
                             classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        frozen={"originals":hashes(originals),"harness":hashes(harness)}
        (work/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n")
        classes,methods=compiler_inventory(verify,originals)
        facts=KotlinFixtureClassFile((originals/"fixture/Counter.class").read_bytes()).facts()
        bridges={identity for identity,row in facts["methods"].items() if row["access"]&0x40}
        if len(classes)!=1 or len(methods)!=13 or len(bridges)!=10 or facts.get("signature") is not None:
            raise RuntimeError("Kotlin did not produce the exact plain scalar bridge inventory")
        if any(row.get("signature") is not None for row in facts["methods"].values()): raise RuntimeError("Unexpected method Signature")
        exceptions=exception_inventory(originals)
        def execute(runtime,projected,label):
            rows=[]
            for route in range(4):
                for mode in range(20):
                    text=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),
                                     "ScalarBridgeHarness",str(route),str(mode),str(projected).lower()],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],operations=12"
                    if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()): raise RuntimeError("Frozen scalar observer changed")
                    rows.append(text)
            return rows
        baseline=execute((originals,harness),False,"baseline")
        classpath=["--classpath",args.android_jar.resolve()]
        for dep in dependencies: classpath += ["--classpath",dep]
        dex=work/"dex";dex.mkdir()
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*sorted(originals.rglob("*.class"))],"d8")
        source_dex=dex/"classes.dex"
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=set(methods): raise RuntimeError("D8 changed original scalar definitions")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-scalar-bridge-code")
        if "invoke-super" not in disassembly or "move-result-wide" not in disassembly or "Lfixture/Counter;.toByte:()B" not in disassembly: raise RuntimeError("D8 lost original scalar forwarding body")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        report=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in report["methods"]}
        projected={identity for identity,row in rows.items() if row["status"]=="source-projected"}
        if (set(rows)!=set(methods) or report["class_count"]!=1 or report["method_count"]!=13 or report["unrecovered_method_count"]
                or report.get("projected_method_count")!=10 or report["status"]!="partial" or projected!=bridges
                or report.get("generic_signature_projections") or report.get("class_source_bindings") or report.get("minimum_java_release",8)!=8):
            raise RuntimeError("Scalar bridge recovery changed inventory or unrelated contracts")
        if any(not rows[i].get("metadata_differences") or not rows[i].get("bridge_target") for i in bridges): raise RuntimeError("Missing original bridge flag projection receipt")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=8,classpath=os.pathsep.join(map(str,dependencies)))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={row["class"]+"->"+row["name"]+row["prototype"] for row in report.get("generated_source_helpers",[])}
        expected_helpers={"Lfixture/Counter;->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"}
        if actual_classes!=classes or set(actual_methods)!=set(methods)|expected_helpers or helpers!=expected_helpers: raise RuntimeError("Original scalar entry or helper omitted/invented")
        actual=KotlinFixtureClassFile((rebuilt/"fixture/Counter.class").read_bytes()).facts();after_exceptions=exception_inventory(rebuilt)
        for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
            if facts.get(key)!=actual.get(key): raise RuntimeError("Original scalar class metadata changed: "+key)
        for identity,row in facts["methods"].items():
            wanted=copy.deepcopy(row)
            if identity in bridges: wanted["access"] &= ~0x40
            if actual["methods"].get(identity)!=wanted or after_exceptions[identity]!=exceptions[identity]: raise RuntimeError("Unreported scalar method metadata change: "+identity)
        if execute((rebuilt,harness),True,"rebuilt")!=baseline: raise RuntimeError("Scalar bodies, bits, dispatch or exact failure changed")
        java='''package fixture;
public final class Counter extends java.util.concurrent.atomic.AtomicInteger {
 public Counter(int initial){super(initial);}
 public byte toByte(){return (byte)intValue();}
 public short toShort(){return (short)intValue();}
 public int toInt(){return super.intValue();}
 public long toLong(){return super.longValue();}
 public float toFloat(){return super.floatValue();}
 public double toDouble(){return super.doubleValue();}
 public final byte byteValue(){return toByte();}
 public final short shortValue(){return toShort();}
 public final int intValue(){return toInt();}
 public final long longValue(){return toLong();}
 public final float floatValue(){return toFloat();}
 public final double doubleValue(){return toDouble();}
}
'''
        wrong_programs=(
            ("recursive-alias","return super.intValue();","return intValue();"),
            ("lost-byte-conversion","return (byte)intValue();","return (byte)(intValue() >>> 1);"),
            ("lost-short-sign","return (short)intValue();","return (short)(intValue() & 127);"),
            ("stale-value","return super.intValue();","return 0;"),
            ("wrong-wide-value","return super.longValue();","return super.longValue() << 32;"),
            ("wrong-float-bits","return super.floatValue();","return Float.intBitsToFloat(super.intValue());"),
            ("lost-double-precision","return super.doubleValue();","return (double)super.floatValue();"),
            ("wrong-short-wrapper","return toShort();","return (short)~toInt();"),
        )
        for label,before,changed in wrong_programs:
            if java.count(before)!=1: raise RuntimeError("Owned scalar counterexample anchor changed")
            source=work/label/"fixture/Counter.java";source.parent.mkdir(parents=True);source.open("x").write(java.replace(before,changed))
            compiled=work/(label+"-classes");verify.compile_local([source],compiled,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,harness,*dependencies))),"ScalarBridgeHarness","0","0","true"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned scalar number bridges changed" not in result.stderr: raise RuntimeError("Frozen scalar observer missed "+label)
        if frozen!={"originals":hashes(originals),"harness":hashes(harness)} or input_hashes!={str(p):sha(p) for p in inputs}: raise RuntimeError("Frozen scalar compiler inputs or observers changed")
        evidence={"status":"success","class_count":1,"method_count":13,"bridge_methods":10,"projected_bridge_flags":10,
                  "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"original_metadata_and_exceptions":"unchanged except disclosed bridge flags",
                  "original_primitive_bodies_and_wide_bits":"matched","independent_wrong_programs":len(wrong_programs),"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__": raise SystemExit(main())
