#!/usr/bin/env python3
"""Verify one illegal Java capture identifier without changing original Kotlin Code."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.test_mobile_android_nested_return import method_code, class_records
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from test_mobile_android_nested_return import method_code, class_records


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd",type=Path,required=True)
    parser.add_argument("--android-jar",type=Path,required=True)
    parser.add_argument("--kotlin-lib",type=Path,required=True)
    parser.add_argument("--java-home",type=Path)
    parser.add_argument("--kotlin-java-home",type=Path,required=True)
    parser.add_argument("--d8",type=Path)
    parser.add_argument("--work-dir",type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-field-names-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"captured_field_names"
        kotlin=args.kotlin_lib.resolve()
        if not (kotlin/"kotlin-compiler.jar").is_file() or not (kotlin/"kotlin-stdlib.jar").is_file():
            raise RuntimeError("The original capture witness requires the unmodified Kotlin compiler distribution")
        inputs=[args.android_jar.resolve(),*sorted(kotlin.glob("*.jar"))]
        input_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        dependencies=[]
        for jar in (kotlin/"kotlin-stdlib.jar",kotlin/"annotations-13.0.jar"):
            output=work/jar.name
            with zipfile.ZipFile(jar) as source,zipfile.ZipFile(output,"x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename,source.read(entry))
            dependencies.append(output)
        java,originals,harness=work/"original-java",work/"originals",work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),java,release=8)
        originals.mkdir()
        verify.run([choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
            "-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(kotlin/"kotlin-stdlib.jar",java))),
            "-Xsam-conversions=class","-jvm-target","1.8","-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        for path in sorted(java.rglob("*.class")):
            target=originals/path.relative_to(java);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
        capture="Lfixture/Core$make$1;"
        target=originals/"fixture/Core$make$1.class"
        before=target.read_bytes();old=b"$wide";new=b"$$v$c$fixture-Wide$-wide$0"
        pattern=b"\x01"+len(old).to_bytes(2,"big")+old
        replacement=b"\x01"+len(new).to_bytes(2,"big")+new
        if before.count(pattern)!=1: raise RuntimeError("Missing exact original Kotlin capture name")
        after=before.replace(pattern,replacement,1)
        if (method_code(before)!=method_code(after) or before[class_records(before)["pool_end"]:]!=after[class_records(after)["pool_end"]:]):
            raise RuntimeError("Capture name edit changed Code or descriptors")
        target.write_bytes(after)
        (work/"capture-name-encoding.json").write_text(json.dumps({"original_name":old.decode(),"input_name":new.decode(),
            "code_and_descriptors":"unchanged","before_sha256":hashlib.sha256(before).hexdigest(),"after_sha256":hashlib.sha256(after).hexdigest()},indent=2)+"\n")
        verify.compile_local([FIXTURES/"harness"/(name+".java") for name in ("CapturedFieldNamesCaller","CapturedFieldNamesHarness")],harness,
            classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=4 or len(all_methods)!=8: raise RuntimeError("Original capture inventory changed: "+repr((len(all_classes),len(all_methods))))
        classes=all_classes-({"Lfixture/Trace;"} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        def facts(root):
            return {r["name"]:r for path in sorted(root.rglob("*.class")) for r in [KotlinFixtureClassFile(path.read_bytes()).facts()]}
        records,exceptions=facts(originals),exception_inventory(originals)
        provider=work/"owned-provider.jar"
        with zipfile.ZipFile(provider,"x") as jar: jar.writestr("fixture/Trace.class",(originals/"fixture/Trace.class").read_bytes())
        if args.dependency_provider: dependencies.append(provider)
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath=["--classpath",args.android_jar.resolve()]
        for path in dependencies: classpath += ["--classpath",path]
        def execute(runtime,label):
            results=[]
            for route in range(4):
                for mode in range(10):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),"CapturedFieldNamesHarness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in output.splitlines()):
                        raise RuntimeError("Original captured-field observer changed: "+output)
                    results.append(output)
            return results
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]: raise RuntimeError("Owned capture changed DEX inventory")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods):
            raise RuntimeError("D8 changed an original captured-field runtime entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        verify.run([verify.dexdump,"-d",source_dex],"original-code")
        annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
        visible=re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations)
        if "VISIBILITY_BUILD" in annotations or visible.count("Lkotlin/Metadata;")!=2 or len(visible)!=2:
            raise RuntimeError("D8 changed original capture annotation retention")
        expected=copy.deepcopy(records);dropped={}
        for owner in classes:
            record=expected[owner]
            for identity,row in [(owner,record),*record["fields"].items(),*record["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    if row.get(key): dropped.setdefault(identity,{})[key]=row[key]
                    row[key]=[]
        (work/"d8-annotation-differences.json").write_text(json.dumps(dropped,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]):
            raise RuntimeError("Capture naming omitted an original class or method")
        bindings=coverage.get("class_source_bindings",[]);fields=coverage.get("field_source_bindings",[])
        if len(bindings)!=1 or bindings[0]["class"]!=capture or len(fields)!=1:
            raise RuntimeError("Capture name lacks exact class and field accounting")
        if fields[0]["class"]!=capture or fields[0]["original_name"]!=new.decode() or fields[0]["type"]!="J" or fields[0]["binding_kind"]!="captured-field-identifier" or not fields[0].get("metadata_differences"):
            raise RuntimeError("Capture name projection changed its descriptor or lacks disclosure")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=coverage.get("minimum_java_release",8),classpath=os.pathsep.join(map(str,dependencies)))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        binding=bindings[0]
        candidates=[c for c in actual_classes if c.endswith(binding["source_name"]+";")]
        if len(candidates)!=1 or binding["binding_kind"]!="anonymous-to-named-local" or not binding.get("metadata_differences"):
            raise RuntimeError("Missing complete named-local source binding")
        mapped=candidates[0]
        def remap(value):
            value=value.replace(capture,mapped)
            if value.startswith(mapped+"-><init>("):
                value=value.replace("-><init>(","-><init>(Lfixture/Core;",1)
            return value
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner!="Lfixture/Hook;"}
        if helpers!=expected_helpers: raise RuntimeError("Capture helper inventory changed")
        if actual_classes!={remap(c) for c in classes} or set(actual_methods)!={remap(m) for m in methods|helpers}:
            raise RuntimeError("Capture naming lost a runtime entry or helper accounting")
        actual=facts(rebuilt);actual_exceptions=exception_inventory(rebuilt)
        (work/"metadata-before.json").write_text(json.dumps(expected,indent=2)+"\n")
        (work/"metadata-after.json").write_text(json.dumps(actual,indent=2)+"\n")
        # Reconcile every typed class/field/member metadata record below.
        for owner in classes:
            wanted=copy.deepcopy(expected[owner]);rebuilt_owner=remap(owner)
            wanted["name"]=rebuilt_owner
            wanted["source_file"]=owner[1:-1].split("$")[0].split("/")[-1]+".java"
            wanted["major"]=44+coverage.get("minimum_java_release",8)
            if owner==capture:
                wanted["fields"]={remap(k).replace("->"+new.decode()+":","->"+fields[0]["source_name"]+":"):v for k,v in wanted["fields"].items()}
                wide=wanted["fields"][mapped+"->"+fields[0]["source_name"]+":J"]
                wide["name"]=fields[0]["source_name"]
                for field in wanted["fields"].values(): field["access"] &= ~0x1000
                wanted["inner_class"].update(inner=mapped,name=binding["source_name"],access=0x10)
            wanted["methods"]={remap(k):v for k,v in wanted["methods"].items()}
            if owner==capture:
                constructor=wanted["methods"][mapped+"-><init>(Lfixture/Core;JLjava/lang/Object;Lfixture/Hook;)V"]
                constructor["prototype"]="(Lfixture/Core;JLjava/lang/Object;Lfixture/Hook;)V"
            observed=copy.deepcopy(actual[rebuilt_owner])
            for identity in list(observed["methods"]):
                if identity in {remap(h) for h in helpers}: observed["methods"].pop(identity)
            if observed!=wanted: raise RuntimeError("Captured-field metadata mismatch on "+owner)
        for identity,row in exceptions.items():
            if identity.split("->",1)[0] in classes and actual_exceptions.get(remap(identity))!=row:
                raise RuntimeError("Capture naming changed original Exceptions")
        if execute((rebuilt,harness),"rebuilt")!=baseline: raise RuntimeError("Captured-field behavior changed after source rebuilding")
        source_paths=sorted((output/"sources").rglob("*.java"))
        source_text={p.relative_to(output/"sources"):p.read_text() for p in source_paths}
        core=Path("fixture/Core.java");name=fields[0]["source_name"]
        mutations={
            "lost-wide": ("this."+name+" = arg0;", "this."+name+" = 0L;"),
            "changed-wide": ("o4)."+name+";", "o4)."+name+" ^ 1L;"),
            "lost-token": ("o4).$token;", "o4).$hook;")}
        for label,(old,new_text) in mutations.items():
            if source_text[core].count(old)!=1: raise RuntimeError("Missing exact owned counterexample anchor")
            authored=work/("wrong-"+label);authored.mkdir()
            altered=[]
            for relative,text in source_text.items():
                path=authored/relative;path.parent.mkdir(parents=True,exist_ok=True)
                path.write_text(text.replace(old,new_text,1) if relative==core else text);altered.append(path)
            wrong=work/("wrong-"+label+"-classes")
            verify.compile_local(altered,wrong,release=coverage.get("minimum_java_release",8),classpath=os.pathsep.join(map(str,dependencies)))
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,harness,*dependencies))),"CapturedFieldNamesHarness","1","5"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/("wrong-"+label+"-observer.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned captured field" not in result.stderr:
                raise RuntimeError("Frozen caller accepted the changed captured field: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(hashlib.sha256(Path(p).read_bytes()).hexdigest()!=v for p,v in input_hashes.items()) or any(hashlib.sha256(Path(p).read_bytes()).hexdigest()!=v for p,v in dependency_hashes.items())):
            raise RuntimeError("Original inputs, compiler or frozen callers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,"code_and_descriptors":"unchanged name-only encoding","wide_capture_and_callback_identity":"matched","original_metadata":"reconciled with disclosed class/field bindings","immutable_callers":"unchanged","counterexamples":len(mutations)}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0
if __name__=="__main__": raise SystemExit(main())
