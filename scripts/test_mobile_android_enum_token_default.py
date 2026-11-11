#!/usr/bin/env python3
"""Verify recursive Enum method formals retain token/default inputs, opaque results and original effects."""
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
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--string-context",action="store_true")
    parser.add_argument("--instance-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-enum-token-default-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"enum_token_default/java"
        harness_prefix="EnumTokenDefault"
        harness_root=FIXTURES/"harness"
        if args.string_context or args.instance_provider:
            staged=work/"staged-java";shutil.copytree(fixture,staged);fixture=staged
            harness_root=work/"staged-harness";harness_root.mkdir()
            for suffix in ("Caller","Harness"):
                source=FIXTURES/"harness"/(harness_prefix+suffix+".java")
                shutil.copy2(source,harness_root/source.name)
            if args.string_context:
                for name in ("Provider","Client"):
                    source=fixture/"fixture"/(name+".java")
                    source.write_text(source.read_text().replace("Object value","String value"))
                for source in harness_root.glob("*.java"):
                    text=source.read_text().replace("Class type,Object value","Class type,String value").replace("Class.class,Object.class","Class.class,String.class")
                    text=text.replace("final Object value=mode==3?fallback:new Trap(),payload=new Object();",
                        'final String value=mode==3?null:new String("owned-context");final Object payload=new Object();')
                    source.write_text(text)
            if args.instance_provider:
                source=fixture/"fixture/Provider.java";source.write_text(source.read_text().replace("public static <E","public <E"))
                source=fixture/"fixture/Client.java";source.write_text(source.read_text().replace("Provider.convert(","Trace.provider.convert("))
                source=fixture/"fixture/Trace.java";source.write_text(source.read_text().replace("public class Trace {","public class Trace {\n    public static Provider provider;"))
                source=harness_root/(harness_prefix+"Harness.java");text=source.read_text()
                context="String" if args.string_context else "Object"
                extra=("    private static final class FutureProvider extends fixture.Provider {\n"
                    "        static int dispatch;\n"
                    "        public <E extends Enum<E>> E convert(Class<E> type,"+context+" value,E fallback) {\n"
                    "            ++dispatch;return super.convert(type,value,fallback);\n"
                    "        }\n    }\n")
                text=text.replace("    private static final class Trap {",extra+"    private static final class Trap {")
                text=text.replace('        Trace.failure=new RuntimeException',
                    '        Trace.provider=mode==12?null:new FutureProvider();\n        Trace.failure=new RuntimeException')
                text=text.replace("            final Object sentinel=new Object();","            int dispatch=FutureProvider.dispatch;\n            final Object sentinel=new Object();")
                special=("            if(mode==12){Throwable failure=null;try{call(route,kind,type,value,fallback);}catch(Throwable caught){failure=caught;}\n"
                    "                require(failure instanceof NullPointerException&&Trace.calls==calls&&Trace.events==0&&Trace.value==sentinel&&Trace.payload==sentinel&&Trace.fallback==TimeUnit.HOURS&&Trace.result==TimeUnit.HOURS&&FutureProvider.dispatch==dispatch);\n"
                    '                System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace=0");continue;}\n')
                text=text.replace("            Enum<?> actual=TimeUnit.HOURS;",special+"            Enum<?> actual=TimeUnit.HOURS;")
                text=text.replace("            require(Trace.calls==calls+1", "            require(FutureProvider.dispatch==dispatch+1);\n            require(Trace.calls==calls+1")
                source.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([harness_root/(harness_prefix+n+".java") for n in ("Caller","Harness")],
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=3 or len(all_methods)!=8:raise RuntimeError("Original enum token default inventory changed")
        declarations={"Lfixture/Provider;"} if args.dependency_provider else set()
        classes=all_classes-declarations
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        signature="<E:Ljava/lang/Enum<TE;>;>(Ljava/lang/Class<TE;>;Ljava/lang/Object;TE;)TE;"
        identity="Lfixture/Provider;->convert(Ljava/lang/Class;Ljava/lang/Object;Ljava/lang/Enum;)Ljava/lang/Enum;"
        if args.string_context:
            signature=signature.replace("Ljava/lang/Object;","Ljava/lang/String;")
            identity=identity.replace("Ljava/lang/Object;","Ljava/lang/String;")
        if records["Lfixture/Provider;"]["methods"][identity]["signature"]!=signature:
            raise RuntimeError("Original enum token default Signature changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for owner in sorted(declarations):
                name=owner[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if declarations:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for operation in range(3):
                    for mode in range(13 if args.instance_provider else 12):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_prefix+"Harness",
                            str(route),str(operation),str(mode)],f"{label}-{route}-{operation}-{mode}")
                        pattern=f"route={route},operation={operation},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                            raise RuntimeError("Original enum token default matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        mutations=(
            ('lost-token', 'Provider.java', 'Trace.type=type;', 'Trace.type=null;', 0, 0),
            ('lost-value', 'Provider.java', 'Trace.value=value;', 'Trace.value=null;', 0, 0),
            ('lost-default', 'Provider.java', 'Trace.fallback=fallback;', 'Trace.fallback=null;', 2, 0),
            ('returned-default', 'Provider.java', 'return (E)Trace.result;', 'return fallback;', 0, 0),
            ('lost-action', 'Provider.java', 'Trace.action.run();', ';', 0, 0),
            ('replaced-failure', 'Provider.java', 'Trace.action.run();', 'try {Trace.action.run();} catch(RuntimeException failure) {throw new IllegalArgumentException(failure);}', 0, 4),
        )
        if args.instance_provider:
            mutations += (
                ('lost-instance-dispatch','Client.java','return Trace.provider.convert(type,value);','return new Provider().convert(type,value);',0,0),
                ('lost-null-receiver','Client.java','return Trace.provider.convert(type,value);','return new Provider().convert(type,value);',0,12),
            )
        for label,filename,before,after,operation,mode in mutations:
            original=(fixture/"fixture"/filename).read_text()
            if original.count(before)!=1:raise RuntimeError("Original enum token default counterexample anchor changed")
            changed=work/label/filename;changed.parent.mkdir();changed.open("x").write(original.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_prefix+"Harness","1",str(operation),str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned enum token default" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect enum token default behavior: "+label)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original enum token default DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original enum token default entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Enum token default recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if declarations else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or not helpers<=expected_helpers:
            raise RuntimeError("Enum token default recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original enum token default class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original enum token default method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if declarations else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Enum token default recovery changed input identities, configuration, integer results or effects")
        original_signature=signature.encode()
        context_descriptor=("Ljava/lang/String;" if args.string_context else "Ljava/lang/Object;").encode()
        negatives=(
            ("missing-self-bound",original_signature.replace(b"Enum<TE;>",b"Enum<TMissing;>")),
            ("variant-self-bound",original_signature.replace(b"Enum<TE;>",b"Enum<+TE;>")),
            ("variant-type-token",original_signature.replace(b"Class<TE;>",b"Class<+TE;>")),
            ("missing-default-formal",original_signature.replace(context_descriptor+b"TE;)TE;",context_descriptor+b"TMissing;)TE;")),
            ("missing-result-formal",original_signature[:-3]+b"TMissing;"),
            ("wrong-object-context",original_signature.replace(context_descriptor+b"TE;)",b"TE;TE;)")),
        )
        for label,changed in negatives:
            negative=work/(label+"-originals");shutil.copytree(originals,negative)
            if original_signature==changed:raise RuntimeError("Signature control did not change metadata: "+label)
            target=negative/"fixture/Provider.class";code_before=method_code(target.read_bytes())
            receipt=change_signature(target,original_signature,changed)
            if method_code(target.read_bytes())!=code_before:raise RuntimeError("Signature control changed Code")
            negative_classpath=classpath
            if declarations:
                changed_dependency=work/(label+"-provider.jar")
                with zipfile.ZipFile(changed_dependency,"x") as jar:
                    for owner in sorted(declarations):
                        entry=owner[1:-1]+".class";jar.writestr(entry,(negative/entry).read_bytes())
                negative_classpath=["--classpath",args.android_jar.resolve(),"--classpath",changed_dependency]
            (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
            negative_dex=work/(label+"-dex");negative_dex.mkdir()
            verify.run([*verify.d8,"--debug","--min-api","24",*negative_classpath,"--output",negative_dex,*[negative/path.relative_to(originals) for path in paths]],label+"-d8")
            rejected=work/(label+"-rejected")
            result=subprocess.run([args.neverd.resolve(),"mobile",negative_dex/"classes.dex",*negative_classpath,"-o",rejected],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or rejected.exists():raise RuntimeError("Unproved enum token default metadata accepted: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"string_context":args.string_context,"instance_provider":args.instance_provider,"attempts_per_case":2,"dependency_provider":args.dependency_provider,
            "recursive_enum_token_default_result":"matched","all_original_entries_and_metadata":"unchanged",
            "input_identity_configuration_partial_writes_callbacks_results_and_exact_failures":"matched",
            "independent_wrong_programs":len(mutations),"same_code_signature_mutations":len(negatives),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
