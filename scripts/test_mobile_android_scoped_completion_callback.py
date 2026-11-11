#!/usr/bin/env python3
"""Verify scoped completion callback and completion bindings retain ordered deferred effects."""
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
    parser.add_argument("--instance-provider",action="store_true")
    parser.add_argument("--leading-word",action="store_true")
    parser.add_argument("--leading-context",action="store_true")
    parser.add_argument("--private-provider",choices=("static","instance"))
    args=parser.parse_args()
    if sum((args.instance_provider,args.leading_word,args.leading_context,bool(args.private_provider)))>1:parser.error("Choose one scoped completion variant")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-scoped-completion-callback-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"scoped_completion_callback/java"
        harness_name="ScopedCompletionCallback"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.instance_provider:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            provider=fixture/"fixture/Provider.java";text=provider.read_text()
            header="public final class Provider"
            method="public static <R> Object work"
            body="context=completion.context();seenScope=scope;"
            if text.count(header)!=1 or text.count(method)!=1 or text.count(body)!=1:
                raise RuntimeError("Original instance provider witness anchors changed")
            engine=text.replace(header,"public final class Engine implements Provider").replace(method,"public <R> Object work")
            engine=engine.replace(body,"seenReceiver=this;"+body).replace("    public static Object sentinel", "    public static Provider seenReceiver;\n    public static Object sentinel")
            (fixture/"fixture/Engine.java").open("x").write(engine)
            provider.write_text("package fixture;\npublic interface Provider { <R> Object work(Callback<? super Scope,? super Completion<? super R>,? extends Object> callback,Completion<? super R> completion); }\n")
            client=fixture/"fixture/Client.java";text=client.read_text()
            if text.count("public final class Client {")!=1 or text.count("Provider.work(callback,completion)")!=1:
                raise RuntimeError("Original client instance receiver anchors changed")
            client.write_text(text.replace("public final class Client {","public final class Client { public static Provider receiver;").replace("Provider.work(callback,completion)","receiver.work(callback,completion)"))
            sources=work/"harness-source";sources.mkdir();copied_sources=[]
            for source in harness_sources:
                text=source.read_text()
                if source.name.endswith("Caller.java"):
                    if text.count("Provider.work(callback,completion)")!=1:raise RuntimeError("Original frozen caller anchor changed")
                    text=text.replace("Provider.work(callback,completion)","Client.receiver.work(callback,completion)")
                else:
                    text=text.replace("Provider.","Engine.")
                    replacements=(
                        ('Engine.class.getMethod("work",Callback.class,Completion.class).invoke(null,callback,completion)', 'Provider.class.getMethod("work",Callback.class,Completion.class).invoke(Client.receiver,callback,completion)'),
                        ('findStatic(Engine.class,"work"', 'findVirtual(Provider.class,"work"'),
                        ('.invokeExact(callback,completion)', '.invokeExact(Client.receiver,callback,completion)'),
                        ('token=mode==1?', 'Client.receiver=new Engine();Engine.seenReceiver=null;\n            token=mode==1?'),
                        ('boolean initialFail=', 'require(Engine.seenReceiver==Client.receiver);\n            boolean initialFail='),
                    )
                    for old,new in replacements:
                        if text.count(old)!=1:raise RuntimeError("Original frozen instance observer anchor changed")
                        text=text.replace(old,new)
                path=sources/source.name;path.open("x").write(text);copied_sources.append(path)
            harness_sources=copied_sources+[FIXTURES/"harness/ScopedCompletionNullReceiverHarness.java"]
        if args.leading_word or args.leading_context:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            def replace_once(path,old,new):
                text=path.read_text()
                if text.count(old)!=1:raise RuntimeError("Original leading parameter witness anchor changed")
                path.write_text(text.replace(old,new))
            leading_type="Scope" if args.leading_context else "long"
            leading_name="tag" if args.leading_context else "word"
            seen_name="seenTag" if args.leading_context else "seenWord"
            provider=fixture/"fixture/Provider.java"
            replace_once(provider,"    public static Object sentinel", "    public static "+leading_type+" "+seen_name+";\n    public static Object sentinel")
            replace_once(provider,"Object work(Callback", "Object work("+leading_type+" "+leading_name+",Callback")
            replace_once(provider,"context=completion.context();seenScope=scope;", seen_name+"="+leading_name+";context=completion.context();seenScope=scope;")
            client=fixture/"fixture/Client.java"
            replace_once(client,"public final class Client {", "public final class Client { public static "+leading_type+" "+leading_name+";")
            replace_once(client,"Provider.work(callback,completion)", "Provider.work("+leading_name+",callback,completion)")
            sources=work/"harness-source";sources.mkdir();copied_sources=[]
            for source in harness_sources:
                path=sources/source.name;path.open("x").write(source.read_text());copied_sources.append(path)
                if source.name.endswith("Caller.java"):
                    replace_once(path,"Provider.work(callback,completion)", "Provider.work(Client."+leading_name+",callback,completion)")
                else:
                    text=path.read_text()
                    old='"work",Callback.class,Completion.class'
                    if text.count(old)!=2:raise RuntimeError("Original leading parameter reflection anchors changed")
                    path.write_text(text.replace(old,'"work",'+leading_type+'.class,Callback.class,Completion.class'))
                    setup=("Client.tag=mode==12?null:new Scope(mode==11?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x8123456789abcdefL+attempt);Provider.seenTag=null;\n            " if args.leading_context else "Client.word=mode==11?Long.MIN_VALUE:mode==13?Long.MAX_VALUE:0x8123456789abcdefL+attempt;Provider.seenWord=0;\n            ")
                    for old,new in (
                        (".invoke(null,callback,completion)", ".invoke(null,Client."+leading_name+",callback,completion)"),
                        ("MethodType.methodType(Object.class,Callback.class,Completion.class)", "MethodType.methodType(Object.class,"+leading_type+".class,Callback.class,Completion.class)"),
                        (".invokeExact(callback,completion)", ".invokeExact(Client."+leading_name+",callback,completion)"),
                        ("method.getGenericParameterTypes()[0],out=(ParameterizedType)method.getGenericParameterTypes()[1]", "method.getGenericParameterTypes()[1],out=(ParameterizedType)method.getGenericParameterTypes()[2]"),
                        ("token=mode==1?", setup+"token=mode==1?"),
                        ("boolean initialFail=", "require(Provider."+seen_name+"==Client."+leading_name+");\n            boolean initialFail="),
                    ):replace_once(path,old,new)
            harness_sources=copied_sources
        if args.private_provider:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            provider=fixture/"fixture/Provider.java";text=provider.read_text()
            method="public static <R> Object work"
            if text.count(method)!=1:raise RuntimeError("Original private completion declaration anchor changed")
            instance=args.private_provider=="instance"
            text=text.replace(method,"private "+("" if instance else "static ")+"<R> Object work")
            wrapper="    public static Object invoke(Callback callback,Completion completion){return "+("receiver." if instance else "")+"work(callback,completion); }\n"
            if instance:
                text=text.replace("    public static Object sentinel", "    public static Provider receiver,seenReceiver;\n    public static Object sentinel")
                text=text.replace("context=completion.context();seenScope=scope;","seenReceiver=this;context=completion.context();seenScope=scope;")
            text=text.replace("    public static Object resume()",wrapper+"    public static Object resume()")
            provider.write_text(text)
            client=fixture/"fixture/Client.java";text=client.read_text()
            if text.count("Provider.work(callback,completion)")!=1:raise RuntimeError("Original private completion client anchor changed")
            client.write_text(text.replace("Provider.work(callback,completion)","Provider.invoke(callback,completion)"))
            sources=work/"harness-source";sources.mkdir();copied_sources=[]
            for original in harness_sources:
                text=original.read_text()
                if original.name.endswith("Caller.java"):
                    if text.count("Provider.work(callback,completion)")!=1:raise RuntimeError("Original frozen private caller anchor changed")
                    text=text.replace("Provider.work(callback,completion)","Provider.invoke(callback,completion)")
                else:
                    for before,after in (('getMethod("work",Callback.class,Completion.class)','getMethod("invoke",Callback.class,Completion.class)'),
                                         ('findStatic(Provider.class,"work"','findStatic(Provider.class,"invoke"')):
                        if text.count(before)!=1:raise RuntimeError("Original private wrapper route anchor changed")
                        text=text.replace(before,after)
                    anchor='Method method=Provider.class.getDeclaredMethod("work",Callback.class,Completion.class);'
                    if text.count(anchor)!=1:raise RuntimeError("Original private method reflection anchor changed")
                    text=text.replace(anchor,anchor+'\n        require(Modifier.isPrivate(method.getModifiers()) && Modifier.isStatic(method.getModifiers())=='+str(not instance).lower()+');')
                    if instance:
                        for before,after in (("token=mode==1?","Provider.receiver=new Provider();Provider.seenReceiver=null;\n            token=mode==1?"),
                                             ("boolean initialFail=","require(Provider.seenReceiver==Provider.receiver);\n            boolean initialFail=")):
                            if text.count(before)!=1:raise RuntimeError("Original private instance observer anchor changed")
                            text=text.replace(before,after)
                destination=sources/original.name;destination.open("x").write(text);copied_sources.append(destination)
            if instance:
                original=FIXTURES/"harness/ScopedCompletionNullReceiverHarness.java"
                text=original.read_text().replace("Client.receiver","Provider.receiver").replace("Engine.","Provider.").replace("new Engine()","new Provider()")
                destination=sources/original.name;destination.open("x").write(text);copied_sources.append(destination)
            harness_sources=copied_sources
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(6 if args.instance_provider else 5) or len(all_methods)!=(10 if args.instance_provider or args.private_provider else 9):raise RuntimeError("Original scoped completion callback inventory changed")
        declarations={"Lfixture/Callback;","Lfixture/Completion;","Lfixture/Scope;"}
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        leading_descriptor="J" if args.leading_word else "Lfixture/Scope;" if args.leading_context else ""
        factory="Lfixture/Provider;->work("+leading_descriptor+"Lfixture/Callback;Lfixture/Completion;)Ljava/lang/Object;"
        signature="<R:Ljava/lang/Object;>(Lfixture/Callback<-Lfixture/Scope;-Lfixture/Completion<-TR;>;+Ljava/lang/Object;>;Lfixture/Completion<-TR;>;)Ljava/lang/Object;"
        if leading_descriptor:signature=signature.replace(">(",">("+leading_descriptor,1)
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete nested carrier relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Callback","Completion","Scope"):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(14):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original scoped completion callback matrix changed: "+output)
                    outputs.append(output)
            if args.instance_provider or args.private_provider=="instance":
                null=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"ScopedCompletionNullReceiverHarness"],label+"-null-receiver")
                if null!="null-receiver-before-provider-body=retained\n":raise RuntimeError("Original null receiver boundary changed")
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original scoped completion callback DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original scoped completion callback entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        if args.private_provider:
            entry=next(row for row in inventory["methods"] if row["identity"]==factory)
            if entry["access_flags"]!=(0xA if args.private_provider=="static" else 0x2) or not entry["has_code"]:
                raise RuntimeError("D8 changed the original private completion declaration")
            disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-private-disassembly")
            witnessed=[]
            for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
                name=re.search(r"(?m)^\s+name\s+: '([^']+)'",block[2])
                if block[1]!="Lfixture/Provider;" or not name or name[1]!="invoke":continue
                ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",block[2])
                calls=[(op,ref) for op,ref in ops if op.startswith("invoke-")]
                expected="invoke-static" if args.private_provider=="static" else "invoke-direct"
                if (len(calls)!=1 or calls[0][0].split("/")[0]!=expected
                        or "Lfixture/Provider;.work:(Lfixture/Callback;Lfixture/Completion;)Ljava/lang/Object;" not in calls[0][1]):
                    raise RuntimeError("D8 changed the original same-owner private call")
                witnessed.append(ops)
            if len(witnessed)!=1:raise RuntimeError("Missing original private completion invocation witness")
            (work/"original-private-invocation.json").open("x").write(json.dumps(witnessed,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Bounded converter recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        abstract={"Lfixture/Callback;","Lfixture/Completion;"}|({"Lfixture/Provider;"} if args.instance_provider else set())
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in abstract }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Bounded converter recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original scoped completion callback class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original scoped completion callback method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Fixed input callback recovery changed ordered callback/completion effects")
        mutations=(("lost-result","Provider","seenResult=value;return value;","seenResult=value;return sentinel;",0),
                   ("wrong-scope","Provider","callback.apply(scope,completion)","callback.apply(null,completion)",0),
                   ("lost-deferred","Provider","saved=callback;","saved=null;",7),
                   ("early-callback","Provider","context=completion.context();","callback.apply(scope,completion);context=completion.context();",4))
        if args.private_provider=="instance":mutations += (("lost-receiver","Provider","seenReceiver=this;","seenReceiver=null;",0),)
        if args.leading_word:mutations += (("lost-word","Provider","seenWord=word;","seenWord=(int)word;",0),)
        if args.leading_context:mutations += (("lost-context","Provider","seenTag=tag;","seenTag=null;",0),)
        for label,name,before,after,mode in mutations:
            if args.instance_provider and name=="Provider":name="Engine"
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=(2 if label=="lost-result" else 1):raise RuntimeError("Original scoped completion callback counterexample anchor changed")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            marker="owned scoped completion callback"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect scoped completion callback: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
            "instance_provider":args.instance_provider,
            "private_provider":args.private_provider,
            "leading_word":args.leading_word,
            "leading_context":args.leading_context,
            "closed_scope_nested_callback_and_final_completion_formal":"matched","all_original_entries_and_metadata":"unchanged",
            "scope_completion_identity_deferred_capture_sentinel_order_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
