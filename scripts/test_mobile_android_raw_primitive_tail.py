#!/usr/bin/env python3
"""Verify exact primitive unboxing bridges with unchanged raw parents and closed tails."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
from copy import deepcopy
import hashlib
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_plain_integer_bridge import bridge_flag


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--"+name,type=Path)
    parser.add_argument("--wide",action="store_true")
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-raw-primitive-tail-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"raw_primitive_tail/java"
        sources=sorted(fixture.rglob("*.java"))
        core_text=(fixture/"fixture/Core.java").read_text()
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("RawPrimitiveTailCaller","RawPrimitiveTailHarness")]
        if args.wide:
            core_text=core_text.replace("((Boolean) value).booleanValue()","((Number) value).longValue()")
            core_text=core_text.replace("emit(boolean value,","emit(long value,").replace("value ? 1 : 0","value")
            authored=work/"authored/fixture/Core.java";authored.parent.mkdir(parents=True);authored.write_text(core_text)
            sources=[authored if p.name=="Core.java" else p for p in sources]
            for i,p in enumerate(harness_sources):
                text=p.read_text().replace("boolean value,","long value,").replace("primitive(core, expected != 0, tail)","primitive(core, expected, tail)")
                target=work/"authored-harness"/p.name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text)
                harness_sources[i]=target
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sources,originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        harness_hashes=hashes(harness);sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        def execute(runtime,state,label):
            outputs=[]
            for route in range(4):
                for mode in range(13):
                    out=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"RawPrimitiveTailHarness",
                        str(route),str(mode),str(state),str(args.wide).lower()],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],targets=[01],callbacks=[01],unbox=[01]"
                    if len(out.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in out.splitlines()):
                        raise RuntimeError("Original primitive/tail behavior matrix changed: "+out)
                    outputs.append(out)
            return outputs
        authored=execute((originals,harness),0,"authored")
        proto="(Ljava/lang/Object;Lfixture/Sink;)Ljava/lang/Object;"
        bridge="Lfixture/Core;->emit"+proto
        receipt=bridge_flag(originals/"fixture/Core.class","emit",proto,0x1,0x1041)
        (work/"bridge-encoding.json").write_text(json.dumps(receipt,indent=2)+"\n")
        original_hashes=hashes(originals)
        baseline=execute((originals,harness),1,"encoded")
        if baseline!=authored:raise RuntimeError("Exact bridge flags changed original execution")
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=5 or len(all_methods)!=7:raise RuntimeError("Original primitive/tail inventory changed")
        providers={"Lfixture/Api;","Lfixture/Sink;"}
        classes=all_classes-(providers if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for c in sorted(providers):
                name=c[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original primitive/tail DEX count changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed original primitive/tail entries")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        rows={r["identity"]:r for r in coverage["methods"]}
        if (set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods)
                or coverage["unrecovered_method_count"] or coverage["projected_method_count"]!=1 or coverage["status"]!="partial"
                or coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")
                or rows[bridge].get("projection_kind")!="raw-parent-parameter-forwarding-bridge"):
            raise RuntimeError("Raw primitive/tail recovery changed entries or source contracts")
        target="Lfixture/Core;->emit("+("J" if args.wide else "Z")+"Lfixture/Sink;)Ljava/lang/Object;"
        if rows[bridge].get("bridge_target")!=target or rows[bridge].get("inherited_class_contract")!="Lfixture/Api;->emit"+proto:
            raise RuntimeError("Missing exact primitive/tail dispatch receipt")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_provider else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes-providers}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Recompiled primitive/tail entries or helpers changed")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Undisclosed primitive/tail class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                expected=deepcopy(metadata)
                if identity==bridge:expected["access"]=0x1
                if after[c]["methods"].get(identity)!=expected or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Undisclosed primitive/tail method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,2,"rebuilt")!=baseline:raise RuntimeError("Primitive conversion changed closed tail, effects, exceptions or returned identity")
        mutations=(("lost-tail","return tail.resume(Trace.token);","return Trace.result;",0),
                   ("changed-result","return tail.resume(Trace.token);","tail.resume(Trace.token); return new Object();",8),
                   ("early-resume","if (Trace.targetFailure != null) throw Trace.targetFailure;",
                    "tail.resume(Trace.token); if (Trace.targetFailure != null) throw Trace.targetFailure;",4))
        for label,old,new,mode in mutations:
            if core_text.count(old)!=1:raise RuntimeError("Primitive/tail counterexample anchor changed")
            wrong=work/label/"fixture/Core.java";wrong.parent.mkdir(parents=True);wrong.write_text(core_text.replace(old,new))
            variant=work/(label+"-classes");verify.compile_local([wrong],variant,classpath=originals,release=8)
            bridge_flag(variant/"fixture/Core.class","emit",proto,0x1,0x1041)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness))),
                "RawPrimitiveTailHarness","1",str(mode),"1",str(args.wide).lower()],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned raw primitive tail" not in result.stderr:raise RuntimeError("Frozen observer missed "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):
            raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"wide":args.wide,
            "dependency_provider":args.dependency_provider,"fresh_jvm_cases_per_stage":len(baseline),"stages":3,"attempts_per_case":2,
            "input_cast_unboxing_dispatch_and_order":"matched","closed_tail_and_return_identity":"matched",
            "raw_owner_and_typed_generic_metadata":"unchanged","bridge_flags":"disclosed","exceptions":"unchanged",
            "independent_counterexamples":len(mutations),"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
