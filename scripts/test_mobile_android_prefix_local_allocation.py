#!/usr/bin/env python3
"""Verify genuine constructor-local argument allocations before this delegation."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
import hashlib
from copy import deepcopy
import shutil
from collections import Counter
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-contracts",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-prefix-local-allocation-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"prefix_local_allocation";kotlin=args.kotlin_lib.resolve()
        inputs=[args.android_jar.resolve(),*sorted(kotlin.glob("*.jar"))]
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
        java_classes,originals,harness=work/"original-java",work/"originals",work/"harness"
        verify.compile_local(sorted((fixture/"java").rglob("*.java")),java_classes,release=8)
        compiler=[choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect","-classpath",os.pathsep.join(map(str,(kotlin/"kotlin-stdlib.jar",java_classes))),"-jvm-target","1.8"]
        verify.run([*compiler,"-d",originals,*sorted((fixture/"kotlin").rglob("*.kt"))],"kotlinc-original")
        for p in java_classes.rglob("*.class"):
            dest=originals/p.relative_to(java_classes);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in ("PrefixLocalAllocationCaller","PrefixLocalAllocationHarness")],harness,classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        all_classes,all_methods=compiler_inventory(verify,originals);all_methods=set(all_methods)
        def facts(root):return {r["name"]:r for p in sorted(root.rglob("*.class")) for r in [KotlinFixtureClassFile(p.read_bytes()).facts()]}
        records,exceptions=facts(originals),exception_inventory(originals)
        anonymous=next((c for c in all_classes if c.startswith("Lfixture/Core$") and records[c]["enclosing_method"]),None)
        if len(all_classes)!=4 or len(all_methods)!=7 or not anonymous:raise RuntimeError("Original constructor local inventory changed")
        borrowed={"Lfixture/Api;"} if args.dependency_contracts else set();classes=all_classes-borrowed;methods={m for m in all_methods if m.split("->",1)[0] in classes}
        if borrowed:
            dependency=work/"owned-contracts.jar"
            with zipfile.ZipFile(dependency,"x") as jar:jar.writestr("fixture/Api.class",(originals/"fixture/Api.class").read_bytes())
            dependencies.append(dependency)
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(10):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),"PrefixLocalAllocationHarness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in output.splitlines()):raise RuntimeError("Original constructor local observer changed")
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        classpath=["--classpath",args.android_jar.resolve()]
        for p in dependencies:classpath += ["--classpath",p]
        dex=work/"dex";dex.mkdir();paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original constructor local DEX count changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory");inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original constructor local definition")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations))!=Counter({"Lkotlin/Metadata;":2}):raise RuntimeError("D8 changed original runtime metadata retention")
        expected=deepcopy(records);dropped={}
        for c in classes:
            for identity,row in [(c,expected[c]),*expected[c]["fields"].items(),*expected[c]["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    if row.get(key):dropped.setdefault(identity,{})[key]=row[key]
                    row[key]=[]
        (work/"d8-class-annotation-loss.json").write_text(json.dumps(dropped,indent=2)+"\n")
        output=work/"recovered";verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if {r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["unrecovered_method_count"] or coverage["minimum_java_release"]!=25 or coverage.get("generic_signature_projections"):raise RuntimeError("Constructor local recovery omitted definitions or changed original signatures")
        bindings=coverage.get("class_source_bindings",[])
        if len(bindings)!=1 or bindings[0]["class"]!=anonymous or bindings[0]["binding_kind"]!="anonymous-to-named-local" or not bindings[0].get("metadata_differences"):raise RuntimeError("Missing original constructor local source receipt")
        binding=bindings[0];rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)),release=25)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt);after,after_exceptions=facts(rebuilt),exception_inventory(rebuilt)
        candidates=[c for c,row in after.items() if (row.get("inner_class") or {}).get("name")==binding["source_name"] and row["enclosing_method"]==records[anonymous]["enclosing_method"]]
        if len(candidates)!=1:raise RuntimeError("Reported early local class is not unique")
        renamed=candidates[0]
        def mapped(s):return s.replace(anonymous,renamed)
        helpers={mapped(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={mapped(c)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c!="Lfixture/Api;"}
        if actual_classes!={mapped(c) for c in classes} or set(actual_methods)!={mapped(i) for i in methods}|expected_helpers or helpers!=expected_helpers:raise RuntimeError("Constructor local recovery invented a runtime entry")
        for c in classes:
            for key in ("access","signature","superclass","interfaces","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                wanted=expected[c].get(key)
                if c==anonymous and key=="access":wanted &= ~0x000f
                if after[mapped(c)].get(key)!=wanted:raise RuntimeError("Undisclosed early local class metadata change: "+c+":"+key)
            inner=deepcopy(expected[c]["inner_class"])
            if c==anonymous:inner["inner"],inner["name"],inner["access"]=renamed,binding["source_name"],inner["access"]&~0x000f
            if after[mapped(c)]["inner_class"]!=inner:raise RuntimeError("Undisclosed early local lexical change")
            fields={mapped(i):dict(row) for i,row in expected[c]["fields"].items()}
            for row in fields.values():
                if row["access"]&0x1000:
                    if not any("explicit captured fields" in d for d in binding["metadata_differences"]):raise RuntimeError("Missing explicit capture flag receipt")
                    row["access"] &= ~0x1000
            if after[mapped(c)]["fields"]!=fields:raise RuntimeError("Undisclosed early local capture fields change")
            for identity,row in expected[c]["methods"].items():
                if after[mapped(c)]["methods"].get(mapped(identity))!=row or after_exceptions[mapped(identity)]!=exceptions[identity]:raise RuntimeError("Original early local method metadata or Exceptions changed: "+identity)
        if execute((rebuilt,harness),"rebuilt")!=baseline:raise RuntimeError("Early local recovery changed capture, wide bits, partial state or failures")
        source=(fixture/"kotlin/fixture/Core.kt").read_text()
        mutations=(("lost-input","captured=input","captured=null",0),("lost-wide","bits=word","bits=word.toInt().toLong()",0),("early-field","Trace.partial=this;Trace.hit(3)\n        this.value=value;","Trace.partial=this;this.value=value;Trace.hit(3)\n        ",5),("lost-label","this.label=label","this.label=null",0))
        for label,old,new,mode in mutations:
            if source.count(old)!=1:raise RuntimeError("Early local counterexample anchor changed")
            changed=work/label/"Core.kt";changed.parent.mkdir();changed.write_text(source.replace(old,new));variant=work/(label+"-classes")
            verify.run([*compiler,"-d",variant,changed],"kotlinc-"+label)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),"PrefixLocalAllocationHarness","1",str(mode)],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned prefix local allocation" not in result.stderr:raise RuntimeError("Observer missed "+label)
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}!=input_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}!=dependency_hashes:raise RuntimeError("Original inputs or frozen observer changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases_per_stage":len(baseline),"attempts_per_case":2,"stages":2,"independent_wrong_programs":4,"dependency_contracts":args.dependency_contracts,"metadata_Exceptions_after_reported_local_name_flags":"matched","input_capture_wide_partial_final_state_and_failure_order":"matched","original_input_and_oracle_bytes":"unchanged","compiler_CLASS_annotation_loss_sites":len(dropped)}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
