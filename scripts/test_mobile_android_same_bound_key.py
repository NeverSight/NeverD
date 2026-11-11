#!/usr/bin/env python3
"""Verify invariant keys retain the method result's single interface bound."""
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
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--interface-provider",action="store_true")
    parser.add_argument("--static-context",action="store_true")
    args=parser.parse_args()
    if args.static_context and args.interface_provider:parser.error("Choose one key provider variant")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-same-bound-key-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"same_bound_key"/("java_interface" if args.interface_provider else "java")
        harness_name="SameBoundKey"
        harness_sources=[FIXTURES/("same_bound_key/harness_interface" if args.interface_provider else "harness")/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.static_context:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            def replace_once(path,old,new):
                text=path.read_text()
                if text.count(old)!=1:raise RuntimeError("Original static context witness anchor changed")
                path.write_text(text.replace(old,new))
            provider=fixture/"fixture/Provider.java"
            replace_once(provider,"public <E extends Element> E get(Key<E> key)","public static <E extends Element> E get(Provider context,Key<E> key)")
            for old,new in (("visits++;seen=key;","context.visits++;context.seen=key;"),("if(skip)","if(context.skip)"),("returned=value;return value;","context.returned=value;return value;")):
                replace_once(provider,old,new)
            replace_once(fixture/"fixture/Client.java","provider.get(key)","Provider.get(provider,key)")
            sources=work/"harness-source";sources.mkdir();copied_sources=[]
            for source in harness_sources:
                target=sources/source.name;target.open("x").write(source.read_text());copied_sources.append(target)
                if source.name.endswith("Caller.java"):
                    replace_once(target,"provider.get(key)","Provider.get(provider,key)")
                else:
                    for old,new in (
                        ('getMethod("get",Key.class).invoke(provider,key)','getMethod("get",Provider.class,Key.class).invoke(null,provider,key)'),
                        ('findVirtual(Provider.class,"get",MethodType.methodType(Element.class,Key.class))','findStatic(Provider.class,"get",MethodType.methodType(Element.class,Provider.class,Key.class))'),
                        ('getDeclaredMethod("get",Key.class)','getDeclaredMethod("get",Provider.class,Key.class)'),
                        ('getGenericParameterTypes()[0]','getGenericParameterTypes()[1]')):
                        replace_once(target,old,new)
            harness_sources=copied_sources
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(5 if args.interface_provider else 4) or len(all_methods)!=(7 if args.interface_provider else 6):raise RuntimeError("Original same bound key inventory changed")
        declarations={"Lfixture/Key;","Lfixture/Element;"}|({"Lfixture/Provider;"} if args.interface_provider else set())
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        context_type="Lfixture/Provider;" if args.static_context else ""
        factory="Lfixture/Provider;->get("+context_type+"Lfixture/Key;)Lfixture/Element;"
        signature="<E::Lfixture/Element;>("+context_type+"Lfixture/Key<TE;>;)TE;"
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete invariant key relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in (["Key","Element","Provider"] if args.interface_provider else ["Key","Element"]):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(10):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original same bound key matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original same bound key DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original same bound key entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Same bound key recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Key;","Lfixture/Element;"}|({"Lfixture/Provider;"} if args.interface_provider else set()) }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Bounded converter recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original same bound key class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original same bound key method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Same bound key recovery changed lookup identity, bound casts or failure order")
        mutations=(("lost-result","Provider","returned=value;return value;","returned=value;return null;",0),
                   ("double-read","Provider","E value=key.read();","key.read();E value=key.read();",0),
                   ("lost-skip","Provider","if(skip)return null;","",7),
                   ("late-seen","Provider","visits++;seen=key;","visits++;",4))
        if args.interface_provider:mutations=tuple((a,"Store",b,c,d) for a,n,b,c,d in mutations)
        if args.static_context:
            def qualify(text):
                return text.replace("visits++","context.visits++").replace("seen=","context.seen=").replace("returned=","context.returned=").replace("if(skip)","if(context.skip)")
            mutations=tuple((a,n,qualify(b),qualify(c),d) for a,n,b,c,d in mutations)
        for label,name,before,after,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original same bound key counterexample anchor changed")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            marker="owned same bound key"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect same bound key: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,"interface_provider":args.interface_provider,
            "static_context":args.static_context,
            "invariant_key_and_shared_interface_bound":"matched","all_original_entries_and_metadata":"unchanged",
            "lookup_identity_partial_provider_state_nulls_bound_cast_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
