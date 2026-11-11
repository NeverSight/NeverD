#!/usr/bin/env python3
"""Verify stateless Kotlin covariant bridge names share one complete source family."""
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
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-stateless-covariant-names-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"stateless_covariant_names"
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
            "-jvm-target","1.8","-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        for path in sorted(java.rglob("*.class")):
            target=originals/path.relative_to(java);target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,target)
        verify.compile_local([FIXTURES/"harness"/(name+".java") for name in ("StatelessCovariantNamesCaller","StatelessCovariantNamesHarness")],harness,
            classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=8 or len(all_methods)!=24: raise RuntimeError("Original stateless covariance inventory changed: "+repr((len(all_classes),len(all_methods))))
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
                for mode in range(6):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),"StatelessCovariantNamesHarness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in output.splitlines()):
                        raise RuntimeError("Original stateless covarianced-field observer changed: "+output)
                    results.append(output)
            return results
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]: raise RuntimeError("Owned stateless covariance changed DEX inventory")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods):
            raise RuntimeError("D8 changed an original covariant runtime entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        verify.run([verify.dexdump,"-d",source_dex],"original-code")
        annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
        visible=re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations)
        if "VISIBILITY_BUILD" in annotations or visible.count("Lkotlin/Metadata;")!=5 or visible.count("Lkotlin/jvm/JvmInline;")!=1 or len(visible)!=6:
            raise RuntimeError("D8 changed original typed Kotlin annotation retention")
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
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=coverage.get("minimum_java_release",8),classpath=os.pathsep.join(map(str,dependencies)))
        actual=facts(rebuilt)
        bindings=coverage.get("class_source_bindings",[])
        if len(bindings)!=1 or bindings[0]["class"]!="Lfixture/Factory$shared$1;" or bindings[0]["binding_kind"]!="anonymous-to-named-member" or not bindings[0].get("metadata_differences"):
            raise RuntimeError("Missing exact stateless anonymous source ownership")
        binding=bindings[0];original_class=binding["class"]
        mapped=binding["source_enclosing_class"][:-1]+"$"+binding["source_name"]+";"
        def class_map(text): return text.replace(original_class,mapped)
        rows={r["identity"]:r for r in coverage["methods"]}
        def method_map(identity):
            row=rows[identity];name=row.get("source_name",row["name"])
            return class_map(identity.split("->",1)[0]+"->"+name+identity[identity.index("("):])
        family={identity for identity in methods if "->make-" in identity}
        if len(family)!=3 or len({rows[i].get("source_name") for i in family})!=1 or any(set(rows[i].get("source_name_family",[]))!=family or not rows[i].get("metadata_differences") for i in family):
            raise RuntimeError("Interface, typed implementation and original bridge lost their complete shared name")
        bridge=next(i for i in family if rows[i].get("bridge_target"))
        if rows[bridge].get("bridge_dispatch") not in family or rows[bridge]["bridge_target"] not in family:
            raise RuntimeError("Source name family lacks the original covariant dispatch proof")
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner!="Lfixture/Api;"}
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        if helpers!=expected_helpers or actual_classes!={class_map(c) for c in classes} or set(actual_methods)!={method_map(i) for i in methods}|{class_map(h) for h in helpers}:
            raise RuntimeError("Covariant family omitted an original runtime entry or helper accounting")
        actual_exceptions=exception_inventory(rebuilt)
        for owner in classes:
            wanted=copy.deepcopy(expected[owner]);after=copy.deepcopy(actual[class_map(owner)])
            wanted["name"]=class_map(owner)
            wanted["source_file"]=owner[1:-1].split("$")[0].split("/")[-1]+".java"
            wanted["major"]=44+coverage.get("minimum_java_release",8)
            if owner==original_class:
                wanted["inner_class"].update(inner=mapped,outer=binding["source_enclosing_class"],name=binding["source_name"])
                if "EnclosingMethod" not in " ".join(binding["metadata_differences"]):
                    raise RuntimeError("Class-only enclosing attribute loss lacks source disclosure")
                wanted["enclosing_method"]=None
            wanted["fields"]={class_map(k):v for k,v in wanted["fields"].items()}
            rebuilt_methods={}
            for identity,metadata in wanted["methods"].items():
                key=method_map(identity);metadata["name"]=rows[identity].get("source_name",metadata["name"])
                if metadata["access"]&0x1000 and not metadata["access"]&0x40:
                    if rows[identity]["status"]!="source-projected" or not rows[identity].get("metadata_differences"):
                        raise RuntimeError("Original synthetic flag loss lacks exact source disclosure")
                    metadata["access"] &= ~0x1000
                rebuilt_methods[key]=metadata
                if actual_exceptions.get(key)!=exceptions[identity]: raise RuntimeError("Original covariant method Exceptions changed")
            wanted["methods"]=rebuilt_methods
            for key in list(after["methods"]):
                if key in {class_map(h) for h in helpers}: after["methods"].pop(key)
            if after!=wanted: raise RuntimeError("Complete stateless covariant metadata mismatch: "+owner)
        (work/"metadata-before.json").write_text(json.dumps(expected,indent=2)+"\n")
        (work/"metadata-after.json").write_text(json.dumps(actual,indent=2)+"\n")
        if execute((rebuilt,harness),"rebuilt")!=baseline: raise RuntimeError("Covariant source family changed the original callback/return behavior")
        source_paths=sorted((output/"sources").rglob("*.java"))
        source_text={p.relative_to(output/"sources"):p.read_text() for p in source_paths}
        factory=Path("fixture/Factory.java");text=source_text[factory]
        pattern=r"(?m)^(\s*o\d+ = \(\(fixture\.Trace\) null\)\.reply);$"
        match=re.search(pattern,text)
        if not match: raise RuntimeError("Missing exact original covariant reply read")
        mutations={"lost-result":(match[0],match[0].replace("((fixture.Trace) null).reply","null")),
                   "lost-wide":(None,None),"lost-receiver":(None,None)}
        for label,(old,new) in list(mutations.items()):
            if old is None:
                field="wide" if label=="lost-wide" else "receiver"
                match=re.search(r"(?m)^(\s*\(\(fixture\.Trace\) null\)\."+field+r" = )([^;]+);$",text)
                if not match: raise RuntimeError("Missing exact original covariant write: "+field)
                old=match[0];new=match[1]+("0L" if field=="wide" else "null")+";"
            if text.count(old)!=1: raise RuntimeError("Ambiguous covariant counterexample anchor")
            authored=work/("wrong-"+label);authored.mkdir();inputs=[]
            for relative,body in source_text.items():
                dest=authored/relative;dest.parent.mkdir(parents=True,exist_ok=True)
                dest.write_text(body.replace(old,new,1) if relative==factory else body);inputs.append(dest)
            wrong=work/("wrong-"+label+"-classes");verify.compile_local(inputs,wrong,release=coverage.get("minimum_java_release",8),classpath=os.pathsep.join(map(str,dependencies)))
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,harness,*dependencies))),"StatelessCovariantNamesHarness","1","0"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/("wrong-"+label+"-observer.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned covariant names" not in result.stderr: raise RuntimeError("Frozen caller accepted an altered covariant implementation: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or any(hashlib.sha256(Path(p).read_bytes()).hexdigest()!=v for p,v in input_hashes.items()) or any(hashlib.sha256(Path(p).read_bytes()).hexdigest()!=v for p,v in dependency_hashes.items())):
            raise RuntimeError("Original inputs, compiler or frozen callers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,"original_metadata":"complete after disclosed names and synthetic flags","original_covariant_bridge_flags":"retained","counterexamples":len(mutations)}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0
if __name__=="__main__": raise SystemExit(main())
