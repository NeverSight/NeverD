#!/usr/bin/env python3
"""Verify fixed input callback and completion bindings retain ordered deferred effects."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
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
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-fixed-input-callback-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"fixed_input_callback/java"
        harness_name="FixedInputCallback"
        originals,harness=work/"originals",work/"harness"
        inputs=[];authored={}
        for source in sorted(fixture.rglob("*.java")):
            text=source.read_text()
            if args.instance_provider and source.name in ("Provider.java","Client.java"):
                if source.name=="Provider.java":
                    before="public static <R> Object work("
                    if text.count(before)!=1 or text.count("context=completion.context();")!=1:
                        raise RuntimeError("Original instance provider source anchor changed")
                    text=text.replace("public final class Provider {","public final class Provider implements Service {\n    public static Object seenReceiver;",1)
                    text=text.replace(before,"public <R> Object work(",1)
                    text=text.replace("context=completion.context();","seenReceiver=this;context=completion.context();",1)
                else:
                    text=text.replace("public final class Client {","public final class Client {\n    public static Service peer;",1)
                    text=text.replace("Provider.work(callback,completion)","peer.work(callback,completion)",1)
                source=work/"instance-java/fixture"/source.name
                source.parent.mkdir(parents=True,exist_ok=True);source.open("x").write(text)
            authored[source.stem]=source;inputs.append(source)
        if args.instance_provider:
            source=work/"instance-java/fixture/Service.java"
            source.open("x").write('''package fixture;
public interface Service {
    <R> Object work(Callback<? super Long,? extends R> callback,Completion<? super R> completion);
}
''')
            inputs.append(source);authored["Service"]=source
        verify.compile_local(inputs,originals,release=8)
        observers=[FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.instance_provider:
            changed=[]
            for source in observers:
                text=source.read_text()
                if source.name.endswith("Caller.java"):
                    if text.count("Provider.work(callback,completion)")!=1:
                        raise RuntimeError("Original instance caller source anchor changed")
                    text=text.replace("Provider.work(callback,completion)","Client.peer.work(callback,completion)",1)
                else:
                    text=text.replace("static long events;","static long events;\n    static int dispatches;",1)
                    before="    static final class Invoke implements Callback {"
                    if text.count(before)!=1:
                        raise RuntimeError("Original instance observer source anchor changed")
                    text=text.replace(before,'''    static final class Alternate implements Service {
        final Provider target=new Provider();
        public <R> Object work(Callback<? super Long,? extends R> callback,Completion<? super R> completion) {
            dispatches++;return target.work(callback,completion);
        }
    }
'''+before,1)
                    text=text.replace('Provider.class.getMethod("work",Callback.class,Completion.class).invoke(null,callback,completion)',
                        'Service.class.getMethod("work",Callback.class,Completion.class).invoke(Client.peer,callback,completion)',1)
                    text=text.replace('findStatic(Provider.class,"work",','findVirtual(Service.class,"work",',1)
                    text=text.replace('.invokeExact(callback,completion);','.invokeExact(Client.peer,callback,completion);',1)
                    text=text.replace('        require(method.getTypeParameters()',
                        '        require(!Modifier.isStatic(method.getModifiers()) && Modifier.isPublic(method.getModifiers()));\n        require(method.getTypeParameters()',1)
                    text=text.replace('int[] failures={0,0,0,0,1,2,3,0,0,2,3,0,0,0};','int[] failures={0,0,0,0,1,2,3,0,0,2,3,0,0,0,0,0};',1)
                    text=text.replace('mode==2||','mode==14||mode==2||')
                    text=text.replace('mode==2?0','(mode==2||mode==14)?0')
                    before='            Object result=null;Throwable caught=null;'
                    if text.count(before)!=1:
                        raise RuntimeError("Original instance observer setup anchor changed")
                    text=text.replace(before,'''            Client.peer=mode==14?null:mode==15?new Alternate():new Provider();
            dispatches=0;Provider.seenReceiver=null;
            Object expectedReceiver=Client.peer instanceof Alternate?((Alternate)Client.peer).target:Client.peer;
'''+before,1)
                    before='            if(accepts!=0)require(accepted==token);'
                    text=text.replace(before,before+'\n            require(Provider.seenReceiver==expectedReceiver && dispatches==(mode==15?1:0));',1)
                dest=work/"instance-observers"/source.name
                dest.parent.mkdir(exist_ok=True);dest.open("x").write(text);changed.append(dest)
            observers=changed
        verify.compile_local(observers,
            harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        authored_hashes={name:hashlib.sha256(source.read_bytes()).hexdigest() for name,source in authored.items()}
        observer_hashes={source.name:hashlib.sha256(source.read_bytes()).hexdigest() for source in observers}
        (work/"frozen-input-hashes.json").open("x").write(json.dumps({"originals":original_hashes,"harness":harness_hashes,
            "authored_sources":authored_hashes,"observer_sources":observer_hashes},indent=2)+"\n")
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(5 if args.instance_provider else 4) or len(all_methods)!=(9 if args.instance_provider else 8):raise RuntimeError("Original fixed input callback inventory changed")
        declarations={"Lfixture/Callback;","Lfixture/Completion;"}
        if args.instance_provider:declarations.add("Lfixture/Service;")
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        factory="Lfixture/Provider;->work(Lfixture/Callback;Lfixture/Completion;)Ljava/lang/Object;"
        signature="<R:Ljava/lang/Object;>(Lfixture/Callback<-Ljava/lang/Long;+TR;>;Lfixture/Completion<-TR;>;)Ljava/lang/Object;"
        if records["Lfixture/Provider;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original complete nested carrier relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Callback","Completion",*(["Service"] if args.instance_provider else [])):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(16 if args.instance_provider else 14):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original fixed input callback matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original fixed input callback DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original fixed input callback entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        if args.instance_provider:
            disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-instance-disassembly")
            if len(re.findall(r"invoke-interface[^\n]*Lfixture/Service;.work:\(Lfixture/Callback;Lfixture/Completion;\)Ljava/lang/Object;",disassembly))!=1:
                raise RuntimeError("Original opaque interface dispatch changed")
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
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Callback;","Lfixture/Completion;","Lfixture/Service;"} }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Bounded converter recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original fixed input callback class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original fixed input callback method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Fixed input callback recovery changed ordered callback/completion effects")
        mutations=(("lost-result","Provider","completion.accept(value);return value;","completion.accept(value);return sentinel;",0),
                   ("lost-wide","Provider","Long.valueOf(clock)","Long.valueOf((int)clock)",0),
                   ("lost-deferred","Provider","saved=callback;","saved=null;",7),
                   ("early-callback","Provider","context=completion.context();","callback.apply(Long.valueOf(clock));context=completion.context();",4))
        if args.instance_provider:
            mutations+=(("changed-provider","Client","peer.work(callback,completion)","new Provider().work(callback,completion)",15),
                        ("lost-receiver","Provider","seenReceiver=this;","seenReceiver=null;",0))
        for label,name,before,after,mode in mutations:
            text=authored[name].read_text()
            if text.count(before)!=(2 if label=="lost-wide" else 1):raise RuntimeError("Original fixed input callback counterexample anchor changed")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_name+"Harness","0" if label=="changed-provider" else "1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            marker="owned fixed input callback"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect fixed input callback: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or any(hashlib.sha256(authored[name].read_bytes()).hexdigest()!=value for name,value in authored_hashes.items())
                or any(hashlib.sha256(source.read_bytes()).hexdigest()!=observer_hashes[source.name] for source in observers)
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
            "instance_provider":args.instance_provider,
            "closed_callback_input_and_shared_completion_formal":"matched","all_original_entries_and_metadata":"unchanged",
            "deferred_capture_sentinel_wide_values_identity_order_and_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
