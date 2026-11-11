#!/usr/bin/env python3
"""Verify allocation ownership follows reachable nodes across interleaved siblings."""
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
    parser.add_argument("--dependency-target",action="store_true")
    parser.add_argument("--nested-parent",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interleaved-allocation-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"interleaved_allocation"
        reference,originals,harness,builder=(work/name for name in ("reference","originals","harness","builder"))
        observers=[FIXTURES/"harness"/(name+".java") for name in ("InterleavedAllocationCaller","InterleavedAllocationHarness")]
        condition="provider.flag==0?provider.first:provider.alternate"
        default_expression="new Target("+condition+",provider.finish(bits))"
        if args.nested_parent:
            copied=work/"nested-fixture";shutil.copytree(fixture,copied);fixture=copied
            core=fixture/"java/fixture/Core.java";text=core.read_text()
            before="        case 1:return new Target(provider.alternate,provider.finish(bits));\n        default:return "+default_expression+";"
            default_expression="new Envelope(provider.branch==1?new Target(provider.alternate,provider.finish(bits)):"+default_expression+").child"
            if text.count(before)!=1:raise RuntimeError("Original nested sibling reference anchor changed")
            core.write_text(text.replace(before,"        default:return "+default_expression+";"))
            copied_observers=[]
            for original in observers:
                text=original.read_text()
                if original.name.endswith("Harness.java"):
                    if text.count("static final boolean NESTED=false;")!=1:raise RuntimeError("Original nested observer anchor changed")
                    text=text.replace("static final boolean NESTED=false;","static final boolean NESTED=true;")
                dest=work/original.name;dest.open("x").write(text);copied_observers.append(dest)
            observers=copied_observers
        sources=[p for p in sorted((fixture/"java").rglob("*.java")) if args.nested_parent or p.name!="Envelope.java"]
        verify.compile_local(sources,reference,release=8)
        verify.compile_local(observers,harness,classpath=reference,release=8)
        exports=["--add-exports","java.base/jdk.internal.org.objectweb.asm=ALL-UNNAMED"]
        builder.mkdir()
        verify.run([verify.javac,*exports,"-d",builder,fixture/"builder/InterleavedAllocationInputBuilder.java"],"build-input-builder")
        originals.mkdir()
        for path in reference.rglob("*.class"):
            if path.name=="Core.class":continue
            dest=originals/path.relative_to(reference);dest.parent.mkdir(parents=True,exist_ok=True);dest.open("xb").write(path.read_bytes())
        verify.run([verify.java,*exports,"-cp",builder,"InterleavedAllocationInputBuilder",originals,str(args.nested_parent).lower()],"build-bytecode-input")
        frozen={name:hashes(path) for name,path in (("reference",reference),("originals",originals),("harness",harness),("builder",builder))}
        if frozen["reference"]["fixture/Core.class"]==frozen["originals"]["fixture/Core.class"]:
            raise RuntimeError("Independent original allocation layout was not authored")
        for name in ("Provider","Target","Trace")+(("Envelope",) if args.nested_parent else ()):
            key="fixture/"+name+".class"
            if frozen["reference"][key]!=frozen["originals"][key]:raise RuntimeError("Builder changed original support classes")
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        def metadata(classes):
            return {owner:{k:v for k,v in record.items() if k not in ("size","sha256")} for owner,record in classes.items()}
        if metadata(records)!=metadata(class_identity.compiler_classes(reference)) or exceptions!=exception_inventory(reference):
            raise RuntimeError("Independent bytecode changed original Java metadata")
        (work/"frozen-input-hashes.json").open("x").write(json.dumps(frozen,indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(5 if args.nested_parent else 4) or len(all_methods)!=(10 if args.nested_parent else 8):raise RuntimeError("Original interleaved inventory changed")
        providers={"Lfixture/Target;"}|({"Lfixture/Envelope;"} if args.nested_parent else set())
        classes=all_classes-(providers if args.dependency_target else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        dependency=work/"owned-target.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for owner in sorted(providers):
                name=owner[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]+(["--classpath",dependency] if args.dependency_target else [])
        def command(runtime,route,mode):
            return [verify.java,"-cp",os.pathsep.join(map(str,runtime)),"InterleavedAllocationHarness",str(route),str(mode)]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(28 if args.nested_parent else 24):
                    output=verify.run(command(runtime,route,mode),f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original interleaved observer output changed")
                    outputs.append(output)
            return outputs
        baseline=execute((reference,harness),"reference")
        if execute((originals,harness),"bytecode-original")!=baseline:raise RuntimeError("Independent bytecode differs from Java reference")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original DEX partition changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original interleaved entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-interleaved-disassembly")
        match=re.search(r"name\s*:\s*'make'([\s\S]*?)(?:catches\s*:)",disassembly)
        if not match:raise RuntimeError("Missing original interleaved DEX body")
        ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",match[1])
        news=[i for i,(op,ref) in enumerate(ops) if op=="new-instance" and "Lfixture/Target;" in ref]
        inits=[i for i,(op,ref) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Target;.<init>:" in ref]
        layout=(len(news)==2 and len(inits)==2 and
                (news[0]<inits[0]<news[1]<inits[1] and
                 any(op=="return-object" for op,_ in ops[news[0]:inits[0]]) if args.nested_parent else
                 news[0]<news[1]<inits[0]<inits[1]))
        if (not layout
                or not any(op.startswith("if-") for op,_ in ops[news[0]:inits[0]])
                or not any(op in ("packed-switch","sparse-switch") or args.nested_parent and op.startswith("if-") for op,_ in ops[:news[0]])):
            raise RuntimeError("D8 did not retain original interleaved sibling ownership")
        if args.nested_parent:
            parents=[i for i,(op,ref) in enumerate(ops) if op=="new-instance" and "Lfixture/Envelope;" in ref]
            parent_inits=[i for i,(op,ref) in enumerate(ops) if op.startswith("invoke-direct") and "Lfixture/Envelope;.<init>:" in ref]
            if len(parents)!=1 or len(parent_inits)!=1 or not parents[0]<news[0]<inits[1]<parent_inits[0]:
                raise RuntimeError("D8 changed original parent/child allocation ownership")
        (work/"original-interleaved-opcodes.json").open("x").write(json.dumps(ops,indent=2)+"\n")
        output=work/"recovered";verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release")!=14):raise RuntimeError("Interleaved recovery changed original contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_target else None,release=14)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Interleaved recovery invented or omitted entries")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original interleaved class metadata changed")
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original interleaved method metadata or Exceptions changed")
        runtime=(rebuilt,harness,dependency) if args.dependency_target else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Interleaved recovery changed effects or failures")
        mutations=(
            ("lost-tail","Core",(("provider.finish(bits)","bits"),),0),
            ("duplicate-tail","Core",(("provider.finish(bits)","(provider.finish(bits)+provider.finish(bits))"),),0),
            ("wrong-sibling","Core",((("provider.branch==1?","provider.branch!=1?") if args.nested_parent else
                ("case 1:return new Target(provider.alternate,provider.finish(bits));","case 1:return null;")),),1),
            ("wrong-null","Core",(("case 0:return null;","case 0:return new Target(provider.alternate,provider.finish(bits));"),),2),
            ("wrong-selection","Core",(("provider.flag==0?provider.first:provider.alternate","provider.flag==0?provider.alternate:provider.first"),),0),
            ("early-read","Core",(("default:return "+default_expression+";",
                "default:Object saved="+condition+";return "+default_expression.replace(condition,"saved")+";"),),0),
            ("early-tail","Core",(("default:return "+default_expression+";",
                "default:long saved=provider.finish(bits);return "+default_expression.replace("provider.finish(bits)","saved")+";"),),0),
            ("late-final-store","Target",(("value=selected;Trace.step(5);","Trace.step(5);value=selected;"),),0),
        )
        if args.nested_parent:
            mutations += (("lost-parent-child","Core",(("provider.branch==1?new Target(provider.alternate,provider.finish(bits)):","provider.branch==1?null:"),),1),
                          ("late-parent-store","Envelope",(("child=value;Trace.step(9);","Trace.step(9);child=value;"),),1))
        for label,owner,changes,mode in mutations:
            text=(fixture/"java/fixture"/(owner+".java")).read_text()
            for before,after_text in changes:
                if before not in text:raise RuntimeError("Independent wrong program anchor changed")
                text=text.replace(before,after_text)
            changed=work/label/(owner+".java");changed.parent.mkdir();changed.open("x").write(text)
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run(command((wrong,originals,harness),1,mode),text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned interleaved allocation" not in result.stderr:raise RuntimeError("Observer missed wrong program: "+label)
        for name,path in (("reference",reference),("originals",originals),("harness",harness),("builder",builder)):
            if hashes(path)!=frozen[name]:raise RuntimeError("Original inputs or frozen observers changed")
        if hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash:
            raise RuntimeError("Original dependencies changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),
            "attempts_per_case":2,"nested_parent":args.nested_parent,"reference_and_bytecode_originals":"matched","dependency_target":args.dependency_target,
            "all_java":"compiled","all_original_entries_metadata_and_exceptions":"unchanged","independent_wrong_programs":len(mutations),
            "interleaved_ownership_initialization_partial_stores_opaque_results_and_failures":"matched","original_inputs_and_frozen_observers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=="__main__":raise SystemExit(main())
