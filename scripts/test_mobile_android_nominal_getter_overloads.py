#!/usr/bin/env python3
"""Verify distinct generic and nominal map getters from genuine Kotlin output."""
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
import struct
import subprocess
import tempfile
import zipfile
try:
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):
        parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):
        parser.add_argument("--"+name,type=Path)
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-nominal-getter-overloads-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"nominal_getter_overloads";kotlin=args.kotlin_lib.resolve()
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
        originals=work/"originals";harness=work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")), originals, release=8)
        verify.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(originals,kotlin/"kotlin-stdlib.jar"))),"-jvm-target","1.8","-d",originals,
                    *sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        # Reduce the compiler corpus to this getter boundary before freezing or
        # executing it. Keep the entire unmodified compiler output separately.
        # Every retained method_info byte, including Code, Signature and flags,
        # and all fields/class attributes/constant-pool bytes stay identical.
        shutil.copytree(originals,work/"unmodified-compiler-originals")
        core=originals/"fixture/Core.class";data=core.read_bytes()
        shape=class_records(data);strings=shape["strings"];at=shape["pool_end"]+6
        at+=2+2*int.from_bytes(data[at:at+2],"big")
        def skip(offset,count):
            for _ in range(count):offset+=6+int.from_bytes(data[offset+2:offset+6],"big")
            return offset
        fields=int.from_bytes(data[at:at+2],"big");at+=2
        for _ in range(fields):at=skip(at+8,int.from_bytes(data[at+6:at+8],"big"))
        count_at=at;count=int.from_bytes(data[at:at+2],"big");at+=2;retained=[];removed=[]
        for _ in range(count):
            start=at;flags,name,descriptor,attrs=struct.unpack_from(">HHHH",data,at)
            at=skip(at+8,attrs)
            if strings[name] in (b"get",b"<init>"):retained.append(data[start:at])
            else:removed.append((strings[name]+strings[descriptor]).decode())
        if len(retained)!=5 or len(removed)!=20:raise RuntimeError("Unmodified Kotlin getter corpus changed")
        core.write_bytes(data[:count_at]+len(retained).to_bytes(2,"big")+b"".join(retained)+data[at:])
        before=method_code(data);after=method_code(core.read_bytes())
        if after!={k:v for k,v in before.items() if k.decode() not in removed}:raise RuntimeError("Getter corpus selection changed an original Code attribute")
        (work/"getter-corpus-selection.json").open("x").write(json.dumps({"retained_methods":[k.decode() for k in after],"omitted_unrelated_map_methods":removed,
            "all_retained_method_info_bytes":"identical to unmodified Kotlin compiler","constant_pool_fields_and_class_attributes":"identical"},indent=2)+"\n")
        verify.compile_local(sorted((fixture/"harness").glob("*.java")),harness,
                             classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        frozen={"originals":hashes(originals),"harness":hashes(harness)}
        (work/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n")
        classes,methods=compiler_inventory(verify,originals)
        records={"L"+p.relative_to(originals).as_posix()[:-6]+";":KotlinFixtureClassFile(p.read_bytes()).facts()
                 for p in originals.rglob("*.class")}
        facts=records["Lfixture/Core;"]
        bridges={identity for record in records.values() for identity,row in record["methods"].items() if row["access"]&0x40}
        nominal="Lfixture/Core;->get(Lfixture/Key;)Lfixture/Box;"
        generic="Lfixture/Core;->get(Lfixture/Key;)Ljava/lang/Object;"
        if len(classes)!=5 or len(methods)!=11 or len(bridges)!=3 or nominal not in bridges or generic in bridges:
            raise RuntimeError("Kotlin did not produce the exact four original getter slots")
        exceptions=exception_inventory(originals)
        def execute(runtime,alias,label):
            rows=[]
            for route in range(4):
                for mode in range(13):
                    text=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),
                                     "NominalGetterHarness",str(route),str(mode),alias],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],operations=4"
                    if len(text.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in text.splitlines()): raise RuntimeError("Frozen nominal getter observer changed")
                    rows.append(text)
            return rows
        baseline=execute((originals,harness),"get","baseline")
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
        if "invoke-super" not in disassembly or "Lfixture/Core;.get:(Lfixture/Key;)Lfixture/Box;" not in disassembly:
            raise RuntimeError("D8 lost the distinct original nominal getter bridge")
        annotation_dump=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotation-boundary")
        if "Lorg/jetbrains/annotations/NotNull;" in annotation_dump:
            raise RuntimeError("D8 parameter annotation boundary changed")
        dropped_parameter_annotations={generic,"Lfixture/Reader;->get(Lfixture/Key;)Ljava/lang/Object;"}
        (work/"d8-annotation-boundary.json").open("x").write(json.dumps({
            "independent_sdk_dexdump_NotNull_uses":0,"original_classfile_NotNull_parameters":sorted(dropped_parameter_annotations),
            "scope":"D8 dropped CLASS parameter annotations before NeverD input"},indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        report=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in report["methods"]}
        projected={identity for identity,row in rows.items() if row["status"]=="source-projected"}
        named=[row for row in rows.values() if row.get("source_name")]
        if (set(rows)!=set(methods) or report["class_count"]!=5 or report["method_count"]!=11 or report["unrecovered_method_count"]
                or report.get("projected_method_count")!=3 or report["status"]!="partial" or projected!=bridges
                or report.get("generic_signature_projections") or report.get("class_source_bindings") or report.get("minimum_java_release",8)!=8
                or len(named)!=1 or named[0]["identity"]!=nominal
                or named[0].get("projection_kind")!="incompatible-local-reference-return-slot"
                or named[0].get("conflicting_local_contracts")!=[generic]):
            raise RuntimeError("Nominal getter projection changed inventory or unrelated contracts")
        alias=named[0]["source_name"]
        if not named[0].get("metadata_differences"):
            raise RuntimeError("Missing explicit nominal getter name/ABI reflection disclosure")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=8,classpath=os.pathsep.join(map(str,dependencies)))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={row["class"]+"->"+row["name"]+row["prototype"] for row in report.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner!="Lfixture/Reader;"}
        renamed=nominal.replace("->get(","->"+alias+"(")
        if actual_classes!=classes or set(actual_methods)!=(set(methods)-{nominal})|{renamed}|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("An original getter entry or helper was omitted or invented")
        after_exceptions=exception_inventory(rebuilt)
        for path in rebuilt.rglob("*.class"):
            owner="L"+path.relative_to(rebuilt).as_posix()[:-6]+";"
            before=records[owner];after=KotlinFixtureClassFile(path.read_bytes()).facts()
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if before.get(key)!=after.get(key):raise RuntimeError("Unreported original nominal getter class metadata change: "+key)
            for identity,row in before["methods"].items():
                mapped=renamed if identity==nominal else identity
                actual=copy.deepcopy(after["methods"][mapped]);wanted=copy.deepcopy(row)
                if identity==nominal:wanted["name"]=alias
                if identity in dropped_parameter_annotations:
                    expected=[[{"type":"Lorg/jetbrains/annotations/NotNull;","elements":{}}]]
                    if wanted["runtime_invisible_parameter_annotations"]!=expected:
                        raise RuntimeError("Original compiler parameter annotation changed")
                    wanted["runtime_invisible_parameter_annotations"]=[]
                if identity in bridges:
                    if not rows[identity].get("metadata_differences"):
                        raise RuntimeError("Bridge metadata change lacks its original receipt")
                    if rows[identity].get("projection_kind")=="guarded-erased-reference-return-bridge":
                        if not any("original final flag may differ" in text for text in rows[identity]["metadata_differences"]):
                            raise RuntimeError("Missing regenerated bridge final-flag disclosure")
                        actual["access"] &= ~0x10;wanted["access"] &= ~0x10
                    else:wanted["access"] &= ~0x1040
                if actual!=wanted or after_exceptions[mapped]!=exceptions[identity]:
                    raise RuntimeError("Unreported original nominal getter method metadata change: "+identity)
        if execute((rebuilt,harness),alias,"rebuilt")!=baseline:
            raise RuntimeError("Original nominal/generic getter dispatch, casts, identity or failure changed")
        java="""package fixture;
public final class Core extends java.util.HashMap<Key<Object>,Box<Object>> implements Reader {
 public <T> T get(Key<T> key) {java.util.Objects.requireNonNull(key);Trace.hits++;Trace.seen=key;if(Trace.failure!=null)throw Trace.failure;return (T)Trace.result;}
 public Box<Object> ALIAS(Key<Object> key) {return (Box<Object>)super.get(key);}
 public final Box<Object> get(Object key) {if(!(key instanceof Key))return null;return ALIAS((Key<Object>)key);}
}
""".replace("ALIAS",alias)
        wrong_programs=(
            ("wrong-dispatch","return (Box<Object>)super.get(key);","return (Box<Object>)get((Key)key);",0),
            ("lost-guard","if(!(key instanceof Key))return null;","",4),
            ("copied-result","return (Box<Object>)super.get(key);","Box<Object> found=(Box<Object>)super.get(key);return found==null?null:new Box<Object>(found.getValue());",0),
            ("early-null-guard","return (Box<Object>)super.get(key);","if(key==null)return null;return (Box<Object>)super.get(key);",3),
            ("lost-failure","if(Trace.failure!=null)throw Trace.failure;","",1),
            ("lost-generic-result","return (T)Trace.result;","return null;",0),
        )
        for label,before,changed,mode in wrong_programs:
            if java.count(before)!=1:raise RuntimeError("Owned nominal getter counterexample anchor changed")
            source=work/label/"fixture/Core.java";source.parent.mkdir(parents=True);source.open("x").write(java.replace(before,changed))
            compiled=work/(label+"-classes")
            verify.compile_local([source],compiled,release=8,classpath=os.pathsep.join(map(str,(originals,*dependencies))))
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness,*dependencies))),
                                   "NominalGetterHarness","3",str(mode),alias],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned nominal getter overload changed" not in result.stderr:
                raise RuntimeError("Frozen nominal getter observer missed "+label)
        if frozen!={"originals":hashes(originals),"harness":hashes(harness)} or input_hashes!={str(p):sha(p) for p in inputs}: raise RuntimeError("Frozen scalar compiler inputs or observers changed")
        evidence={"status":"success","class_count":5,"method_count":11,"bridge_methods":3,
                  "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"projected_local_name_count":1,
                  "original_metadata_and_exceptions":"unchanged except D8-dropped CLASS parameter annotations, disclosed bridge flags and one local bridge name",
                  "original_generic_nominal_guarded_and_erased_getters":"matched",
                  "independent_wrong_programs":len(wrong_programs),"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__": raise SystemExit(main())
