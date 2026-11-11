#!/usr/bin/env python3
"""Verify private generic anchors retain fixed interface results and exact source scope."""
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
except ModuleNotFoundError as error:
    if error.name!="scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def prepare_nested_scalar(fixture, harness_sources, work, static):
    authored=work/"nested-source";shutil.copytree(fixture,authored)
    observers=work/"nested-harness";observers.mkdir()
    for path in harness_sources:shutil.copy2(path,observers/path.name)
    def change(path,before,after):
        text=path.read_text()
        if before not in text:raise RuntimeError("Nested scalar source anchor changed: "+path.name)
        path.write_text(text.replace(before,after))
    (authored/"fixture/Inner.java").open("x").write(
        "package fixture; public final class Inner<E> { public E value; public Inner(E value) { this.value=value; } }\n")
    provider=authored/"fixture/Provider.java"
    for before,after in (("Cell<T> cell","Cell<Inner<T>> cell"),
                         ("T input=cell.read();","T input=cell.read().value;"),
                         ("Reply dispatch(","int dispatch("),("Reply entry(","int entry("),
                         ("Reply result=","int result="),("return Trace.reply;","return Trace.code;")):
        change(provider,before,after)
    change(authored/"fixture/Core.java","Reply call(","int call(")
    change(authored/"fixture/Trace.java","int failAt, reads, callbacks;","int failAt, reads, callbacks, code;")
    change(observers/"PrivateAnchoredNominalCaller.java","Reply call(","int call(")
    observer=observers/"PrivateAnchoredNominalHarness.java"
    offset=2 if static else 1
    before=f"require(((ParameterizedType)target.getGenericParameterTypes()[{offset}]).getActualTypeArguments()[0]==formal);"
    after=(f"Type nestedType=((ParameterizedType)target.getGenericParameterTypes()[{offset}]).getActualTypeArguments()[0];\n"
           "        require(nestedType instanceof ParameterizedType); ParameterizedType nested=(ParameterizedType)nestedType;\n"
           "        require(nested.getRawType()==Inner.class && nested.getActualTypeArguments()[0]==formal);")
    for old,new in ((before,after),("target.getReturnType()==Reply.class","target.getReturnType()==int.class"),
        ("return Trace.nextInput;","return new Inner<Object>(Trace.nextInput);"),
        ("new Cell<Object>(item)","new Cell(mode==14?null:mode==15?new Object():new Inner<Object>(item))"),
        ("cell.value=expectedInput;","cell.value=mode==14?null:mode==15?new Object():new Inner<Object>(expectedInput);"),
        ("if(mode==9)Trace.reply=null;else if(mode==12)Trace.reply=incoming;",
         "if(mode==9)Trace.code=0;else if(mode==12)Trace.code=((ReplyImpl)incoming).id;"),
        ("Trace.reply=offered;","Trace.reply=offered;Trace.code=attempt==0?Integer.MIN_VALUE:Integer.MAX_VALUE;"),
        ("Reply result=null;","int result=0;"),("result=(Reply)Core.class","result=(Integer)Core.class"),
        ("result=(Reply)MethodHandles","result=(int)MethodHandles"),
        ("MethodType.methodType(Reply.class,","MethodType.methodType(int.class,"),
        ("mode==7?\"612\"","mode==7||mode==14||mode==15?\"612\""),
        ("read&&mode!=2?expectedInput","read&&mode!=2&&mode!=14&&mode!=15?expectedInput"),
        ("mode==6||mode==7||mode==8||mode==10)","mode==6||mode==7||mode==8||mode==10||mode==14)"),
        ("else require(failure==null&&result==(mode==9?null:mode==12?incoming:offered));",
         "else if(mode==15)require(failure instanceof ClassCastException);\n"
         "            else require(failure==null&&result==(mode==9?0:mode==12?((ReplyImpl)incoming).id:attempt==0?Integer.MIN_VALUE:Integer.MAX_VALUE));")):
        change(observer,old,new)
    return authored,sorted(observers.glob("*.java"))

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"): parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"): parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-carrier",action="store_true")
    parser.add_argument("--static-entry",action="store_true")
    parser.add_argument("--nested-scalar",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-anchored-nominal-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"private_anchored_nominal/java"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("PrivateAnchoredNominalCaller","PrivateAnchoredNominalHarness")]
        if args.static_entry:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            def replace_once(path,old,new):
                text=path.read_text()
                if text.count(old)!=1:raise RuntimeError("Original static anchor witness changed")
                path.write_text(text.replace(old,new))
            provider=fixture/"fixture/Provider.java"
            replace_once(provider,"private final <T> Reply dispatch(Reply incoming,","private static final <T> Reply dispatch(Provider owner, Reply incoming,")
            replace_once(provider,"Trace.receiver=this;calls++;","Trace.receiver=owner;owner.calls++;")
            replace_once(provider,"Reply result=owner.dispatch(incoming,","owner.getClass();Reply result=dispatch(owner,incoming,")
            sources=work/"harness-source";sources.mkdir()
            frozen=sources/harness_sources[1].name;frozen.open("x").write(harness_sources[1].read_text())
            replace_once(frozen,"getDeclaredMethod(\"dispatch\",Reply.class,","getDeclaredMethod(\"dispatch\",Provider.class,Reply.class,")
            replace_once(frozen,"!Modifier.isStatic(target.getModifiers())","Modifier.isStatic(target.getModifiers())")
            replace_once(frozen,"target.getGenericParameterTypes()[2]","target.getGenericParameterTypes()[3]")
            replace_once(frozen,"target.getGenericParameterTypes()[1]","target.getGenericParameterTypes()[2]")
            harness_sources=[harness_sources[0],frozen]
        if args.nested_scalar:
            fixture,harness_sources=prepare_nested_scalar(fixture,harness_sources,work,args.static_entry)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(8 if args.nested_scalar else 7) or len(all_methods)!=(12 if args.nested_scalar else 11): raise RuntimeError("Original private nominal inventory changed: "+repr((len(all_classes),len(all_methods))))
        declarations={"Lfixture/Cell;"}|({"Lfixture/Inner;"} if args.nested_scalar else set())
        classes=all_classes-(declarations if args.dependency_carrier else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        target="Lfixture/Provider;->dispatch(Lfixture/Reply;Lfixture/Cell;Lfixture/Callback;ZFJ)Lfixture/Reply;"
        signature="<T:Ljava/lang/Object;>(Lfixture/Reply;Lfixture/Cell<TT;>;Lfixture/Callback<-TT;Ljava/lang/Boolean;>;ZFJ)Lfixture/Reply;"
        if args.static_entry:
            target=target.replace("dispatch(","dispatch(Lfixture/Provider;")
            signature=signature.replace(">(",">(Lfixture/Provider;")
        if args.nested_scalar:
            target=target.rsplit(")",1)[0]+")I"
            signature=signature.replace("Cell<TT;>","Cell<Lfixture/Inner<TT;>;>").rsplit(")",1)[0]+")I"
        metadata=records["Lfixture/Provider;"]["methods"][target]
        if metadata["signature"]!=signature or metadata["access"]!=(0x1a if args.static_entry else 0x12): raise RuntimeError("Original private entry signature or access changed")
        dependency=work/"owned-carrier.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for declaration in sorted(declarations):
                name=declaration[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_carrier: classpath += ["--classpath",dependency]
        def execute(runtime,label):
            result=[]
            for route in range(4):
                for mode in range(16 if args.nested_scalar else 14):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"PrivateAnchoredNominalHarness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in output.splitlines()):
                        raise RuntimeError("Original private anchor observer changed: "+output)
                    result.append(output)
            return result
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]: raise RuntimeError("Private anchor changed DEX inventory")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original private nominal runtime entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        code=verify.run([verify.dexdump,"-d",source_dex],"original-private-call")
        if "Lfixture/Provider;.dispatch:" not in code or ("invoke-static/range" if args.static_entry else "invoke-direct/range") not in code: raise RuntimeError("Missing original private wide-word call witness")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["status"]!="recovered" or coverage.get("projected_method_count",0) or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Private anchor changed original scope, contracts or inventory")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=coverage.get("minimum_java_release",8),classpath=dependency if args.dependency_carrier else None)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner not in {"Lfixture/Callback;","Lfixture/Reply;"}}
        if actual_classes!=classes or set(actual_methods)!=methods|helpers or helpers!=expected_helpers: raise RuntimeError("Private anchor lost a runtime entry or helper accounting")
        actual_records,actual_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for owner in classes:
            before,after=records[owner],actual_records[owner]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations"):
                if before.get(key)!=after.get(key): raise RuntimeError("Private result class metadata changed: "+owner+":"+key)
            for identity,row in before["methods"].items():
                if after["methods"].get(identity)!=row or actual_exceptions.get(identity)!=exceptions[identity]: raise RuntimeError("Private original method metadata/Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_carrier else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline: raise RuntimeError("Private receiver, input binding or fixed result changed")
        # Independently authored wrong programs keep all public runtime entries;
        # the immutable caller/observer must catch each semantic difference.
        provider=(fixture/"fixture/Provider.java").read_text()
        mutations={"replaced-result":("return Trace.reply;","return incoming;"),"lost-receiver":("Trace.receiver="+("owner" if args.static_entry else "this")+";","Trace.receiver=null;"),"skipped-getter":("T input=cell.read();","T input=cell.value;")}
        if args.nested_scalar:
            mutations["replaced-result"]=("return Trace.code;","return 0;")
            mutations["skipped-getter"]=("T input=cell.read().value;","T input=cell.value.value;")
            mutations["lost-nested-value"]=("T input=cell.read().value;","T input=(T)(Object)cell.read();")
        for label,(old,new) in mutations.items():
            if provider.count(old)!=1: raise RuntimeError("Missing exact owned provider counterexample anchor")
            authored=work/("wrong-"+label);authored.mkdir();inputs=[]
            for original in sorted(fixture.rglob("*.java")):
                dest=authored/original.relative_to(fixture);dest.parent.mkdir(parents=True,exist_ok=True)
                text=original.read_text();dest.write_text(text.replace(old,new,1) if original.name=="Provider.java" else text);inputs.append(dest)
            wrong=work/("wrong-"+label+"-classes");verify.compile_local(inputs,wrong,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,harness))),"PrivateAnchoredNominalHarness","1","0"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/("wrong-"+label+"-observer.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned private anchored result" not in result.stderr: raise RuntimeError("Frozen caller accepted an altered private result: "+label)
        if args.nested_scalar:
            wrong=work/"signature-drift/fixture/Provider.class";wrong.parent.mkdir(parents=True)
            shutil.copy2(originals/"fixture/Provider.class",wrong)
            rows=records["Lfixture/Provider;"]["methods"]
            signature=next(row["signature"] for identity,row in rows.items() if "->dispatch(" in identity).encode()
            receipt=change_signature(wrong,signature,signature.replace(b"Inner<TT;>",b"Inner<Ljava/lang/Object;>"))
            (work/"signature-drift-encoding.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parents[1],originals,harness))),
                "PrivateAnchoredNominalHarness","1","0"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/"signature-drift-counterexample.log").open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned private anchored result" not in result.stderr:raise RuntimeError("Frozen observer missed unchanged-Code nested Signature drift")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash:
            raise RuntimeError("Original inputs, declaration carrier or frozen callers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_carrier":args.dependency_carrier,"static_entry":args.static_entry,"original_metadata_and_exceptions":"unchanged","private_scope_and_wide_parameters":"matched","arbitrary_fixed_interface_result":"matched","counterexamples":len(mutations),"immutable_callers":"unchanged"}
        if args.nested_scalar:
            evidence.pop("arbitrary_fixed_interface_result")
            evidence.update({"nested_scalar":True,"arbitrary_original_integer_result":"matched","counterexamples":len(mutations)+1})
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0
if __name__=="__main__": raise SystemExit(main())
