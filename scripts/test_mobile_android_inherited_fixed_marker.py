#!/usr/bin/env python3
"""Verify inherited fixed marker constructors retain superclass and callback effects."""
from __future__ import annotations
import argparse
from contextlib import nullcontext
from copy import deepcopy
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
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
    from scripts.test_mobile_android_nested_return import change_signature
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_array_callbacks import change_method_flags
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    parser.add_argument("--open-owner",action="store_true")
    parser.add_argument("--final-owner",action="store_true")
    parser.add_argument("--variant-input",action="store_true")
    parser.add_argument("--nominal-bound", choices=("class", "interface"))
    parser.add_argument("--mixed-star-access",choices=("public-target","plain-marker","inherited-marker"))
    args=parser.parse_args()
    args.open_owner |= bool(args.mixed_star_access)
    args.variant_input |= bool(args.mixed_star_access)
    marker_synthetic=args.mixed_star_access not in ("plain-marker","inherited-marker")
    if args.nominal_bound and args.variant_input:parser.error("Choose one bounded or nested-variance profile")
    if args.variant_input and args.final_owner:parser.error("Variant input profiles cover abstract and open owners")
    if args.open_owner and args.final_owner:parser.error("Choose one concrete marker owner variant")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-inherited-fixed-marker-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"inherited_fixed_marker/java"
        harness_name="InheritedFixedMarker"
        runtime_harness=harness_name+"Harness"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.open_owner or args.final_owner:
            copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            core=fixture/"fixture/Core.java";text=core.read_text()
            if text.count("public abstract class Core")!=1 or text.count("public abstract Value current();")!=1:
                raise RuntimeError("Original concrete owner witness anchors changed")
            core.write_text(text.replace("public abstract class Core","final class Core" if args.final_owner else "public class Core").replace("public abstract Value current();","public Value current() { return value; }"))
            if args.final_owner:
                (fixture/"fixture/Child.java").unlink()
                client=fixture/"fixture/Client.java";text=client.read_text()
                if text.count("Child")!=2:raise RuntimeError("Original final-owner facade anchors changed")
                client.write_text(text.replace("Child","Core"))
            sources=work/"harness-source";sources.mkdir();copied_sources=[]
            for source in harness_sources:
                if args.final_owner and not source.name.endswith("Caller.java"):continue
                text=source.read_text()
                if source.name.endswith("Caller.java"):
                    old="public final class InheritedFixedMarkerCaller {"
                    new=old+" public static Core makeCore(Value value,long bits,boolean enabled,Sink sink,kotlin.jvm.internal.DefaultConstructorMarker marker){return new Core(value,bits,enabled,sink,marker);}"
                else:
                    old="Modifier.isAbstract(Core.class.getModifiers())"
                    new="!Modifier.isAbstract(Core.class.getModifiers())"
                if text.count(old)!=1:raise RuntimeError("Original frozen concrete owner observer anchor changed")
                text=text.replace(old,new)
                if args.final_owner:
                    if text.count("Child")!=2:raise RuntimeError("Original final-owner frozen caller anchors changed")
                    text="package fixture;\n"+text.replace("Child","Core")
                path=sources/source.name;path.open("x").write(text);copied_sources.append(path)
            harness_sources=copied_sources+[FIXTURES/"harness"/("FinalFixedMarkerHarness.java" if args.final_owner else "OpenFixedMarkerDirectHarness.java")]
            if args.final_owner:runtime_harness="fixture.FinalFixedMarkerHarness"
        variant_sink = "Sink<? super Value, ? extends Box<? super Object, ? extends Object>>"
        if args.mixed_star_access:
            variant_sink="Sink<? super Value, ? extends Box<? super Object, ?>>"
        if args.variant_input:
            if not args.open_owner:
                copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            core=fixture/"fixture/Core.java";text=core.read_text()
            if text.count("Sink<Value>")!=3:raise RuntimeError("Original variant callback anchors changed")
            core.write_text(text.replace("Sink<Value>",variant_sink))
            sink=fixture/"fixture/Sink.java";text=sink.read_text()
            if text.count("Sink<E>")!=1:raise RuntimeError("Original binary callback declaration changed")
            sink.write_text(text.replace("Sink<E>","Sink<E,U>"))
            (fixture/"fixture/Box.java").open("x").write("package fixture;\npublic final class Box<A,B> {}\n")
            copied_sources=[];sources=work/"variant-harness-source";sources.mkdir()
            for source in harness_sources:
                text=source.read_text().replace("Sink<Value>","Sink<Value,Box<Object,Object>>")
                if source.name=="InheritedFixedMarkerHarness.java":
                    old="((ParameterizedType)sinkType).getActualTypeArguments()[0]==Value.class"
                    if text.count(old)!=1:raise RuntimeError("Original generic reflection observer changed")
                    text=text.replace(old,"variantInput(((ParameterizedType)sinkType).getActualTypeArguments())")
                    helper="""    static boolean variantInput(Type[] types) {
        if (types.length!=2 || !(types[0] instanceof WildcardType) || !(types[1] instanceof WildcardType)) return false;
        WildcardType first=(WildcardType)types[0], outer=(WildcardType)types[1];
        if (first.getLowerBounds().length!=1 || first.getLowerBounds()[0]!=Value.class || first.getUpperBounds().length!=1 || first.getUpperBounds()[0]!=Object.class) return false;
        if (outer.getLowerBounds().length!=0 || outer.getUpperBounds().length!=1 || !(outer.getUpperBounds()[0] instanceof ParameterizedType)) return false;
        ParameterizedType nested=(ParameterizedType)outer.getUpperBounds()[0];
        if (nested.getRawType()!=Box.class || nested.getActualTypeArguments().length!=2) return false;
        for (int i=0;i<2;i++) {
            if (!(nested.getActualTypeArguments()[i] instanceof WildcardType)) return false;
            WildcardType slot=(WildcardType)nested.getActualTypeArguments()[i];
            if (slot.getUpperBounds().length!=1 || slot.getUpperBounds()[0]!=Object.class || slot.getLowerBounds().length!=(i==0?1:0)) return false;
            if (i==0 && slot.getLowerBounds()[0]!=Object.class) return false;
        }
        return true;
    }
"""
                    text=text.replace("    static int calls;",helper+"    static int calls;")
                path=sources/source.name;path.open("x").write(text);copied_sources.append(path)
            harness_sources=copied_sources
        if args.nominal_bound:
            if not (args.open_owner or args.final_owner):
                copied=work/"fixture";shutil.copytree(fixture,copied);fixture=copied
            bound = "Tagged" if args.nominal_bound=="interface" else "Value"
            sink=fixture/"fixture/Sink.java";text=sink.read_text()
            if text.count("Sink<E>")!=1:raise RuntimeError("Original bounded carrier declaration changed")
            sink.write_text(text.replace("Sink<E>","Sink<E extends "+bound+">"))
            if args.nominal_bound=="interface":
                value=fixture/"fixture/Value.java";text=value.read_text()
                if text.count("class Value")!=1:raise RuntimeError("Original bounded actual type changed")
                value.write_text(text.replace("class Value","class Value implements Tagged"))
            sources=work/"bounded-harness-source";sources.mkdir();copied_sources=[]
            for source in harness_sources:
                text=source.read_text()
                if source.name in ("InheritedFixedMarkerHarness.java","FinalFixedMarkerHarness.java"):
                    anchor="        Constructor<?> target="
                    if text.count(anchor)!=1:raise RuntimeError("Original frozen bound observer changed")
                    check="check" if args.final_owner else "require"
                    observed=check+"(Sink.class.getTypeParameters().length==1 && Sink.class.getTypeParameters()[0].getBounds().length==1 && Sink.class.getTypeParameters()[0].getBounds()[0]=="+bound+".class);"
                    text=text.replace(anchor,"        "+observed+"\n"+anchor)
                path=sources/source.name;path.open("x").write(text);copied_sources.append(path)
            harness_sources=copied_sources
        if args.mixed_star_access:
            core=fixture/"fixture/Core.java";text=core.read_text()
            flat=args.mixed_star_access!="inherited-marker"
            if flat:
                if text.count(" extends Base implements Tagged")!=1 or text.count("        Trace.partial=this;")!=1:
                    raise RuntimeError("Owned flat marker owner anchors changed")
                text=text.replace(" extends Base implements Tagged","")
                text=text.replace("    public final Value value;","    public final int baseStamp;\n    public final Value value;",1)
                text=text.replace("        Trace.partial=this;","        Trace.base=this;Trace.hit(8);baseStamp=42;Trace.hit(9);\n        Trace.partial=this;",1)
            if args.mixed_star_access=="public-target":
                if text.count("private Core(")!=1:raise RuntimeError("Owned target constructor access changed")
                text=text.replace("private Core(","public Core(",1)
            core.write_text(text)
            adjusted=work/"access-harness-source";adjusted.mkdir();updated=[]
            for source in harness_sources:
                text=source.read_text()
                if source.name=="InheritedFixedMarkerHarness.java":
                    if flat:
                        before="Core.class.getSuperclass()==Base.class && Core.class.getTypeParameters().length==0 && Core.class.getInterfaces().length==1 && Core.class.getInterfaces()[0]==Tagged.class"
                        if text.count(before)!=1:raise RuntimeError("Owned marker inheritance observer changed")
                        text=text.replace(before,"Core.class.getSuperclass()==Object.class && Core.class.getTypeParameters().length==0 && Core.class.getInterfaces().length==0",1)
                    if args.mixed_star_access=="public-target":
                        before="Modifier.isPrivate(target.getModifiers())"
                        if text.count(before)!=1:raise RuntimeError("Owned marker target observer changed")
                        text=text.replace(before,"Modifier.isPublic(target.getModifiers())",1)
                path=adjusted/source.name;path.open("x").write(text);updated.append(path)
            harness_sources=updated
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,
            harness,classpath=originals,release=8)
        marker_prototype="(Lfixture/Value;JZLfixture/Sink;Lkotlin/jvm/internal/DefaultConstructorMarker;)V"
        original_records=class_identity.compiler_classes(originals)
        receipt=change_method_flags(originals/"fixture/Core.class",marker_prototype.encode(),0x1,0x1001 if marker_synthetic else 0x1,name=b"<init>")
        (work/"marker-flag-witness.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(10 if args.variant_input else 8 if args.final_owner else 9) or len(all_methods)!=(14 if args.variant_input else 11 if args.final_owner else 13):raise RuntimeError("Original inherited fixed marker inventory changed")
        declarations={"Lfixture/Sink;","Lfixture/Base;","Lfixture/Tagged;","Lfixture/Value;"}
        if args.variant_input:declarations.add("Lfixture/Box;")
        classes=all_classes-(declarations if args.dependency_inputs else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        wanted=deepcopy(original_records)
        wanted["Lfixture/Core;"]["methods"]["Lfixture/Core;-><init>"+marker_prototype]["access"]|=(0x1000 if marker_synthetic else 0)
        for owner in records:
            for key in wanted[owner]:
                if key!="sha256" and wanted[owner][key]!=records[owner].get(key):
                    raise RuntimeError("Marker encoding changed other original metadata: "+owner+":"+key)
        factory="Lfixture/Core;-><init>(Lfixture/Value;JZLfixture/Sink;)V"
        signature="(Lfixture/Value;JZLfixture/Sink<Lfixture/Value;>;)V"
        if args.variant_input:
            signature="(Lfixture/Value;JZLfixture/Sink<-Lfixture/Value;+Lfixture/Box<-Ljava/lang/Object;+Ljava/lang/Object;>;>;)V"
        if args.mixed_star_access:
            signature=signature.replace("-Ljava/lang/Object;+Ljava/lang/Object;","-Ljava/lang/Object;*")
        if records["Lfixture/Core;"]["methods"][factory]["signature"]!=signature:
            raise RuntimeError("Original fixed callback constructor relation changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("Sink","Base","Tagged","Value")+(("Box",) if args.variant_input else ()):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs:classpath += ["--classpath",dependency]
        def execute(runtime,label,synthetic):
            outputs=[]
            for route in range(4):
                for mode in range(14):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),runtime_harness,
                        str(route),str(mode),str(synthetic).lower()],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original inherited fixed marker matrix changed: "+output)
                    outputs.append(output)
            if args.open_owner:
                for route in range(4):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"OpenFixedMarkerDirectHarness",str(route),str(synthetic).lower()],f"{label}-direct-{route}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"direct-route={route},attempt=[01],trace=891263",line) for line in output.splitlines()):
                        raise RuntimeError("Original direct concrete constructor observer changed")
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline",marker_synthetic)
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original inherited fixed marker DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original inherited fixed marker entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        if args.variant_input:
            disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-marker-disassembly")
            witnessed=False
            for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
                if block[1]!="Lfixture/Core;" or "'"+marker_prototype+"'" not in block[2]:continue
                ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)",block[2])
                if (len(ops)!=2 or not ops[0][0].startswith("invoke-direct") or ops[1][0]!="return-void"
                        or "Lfixture/Core;.<init>:(Lfixture/Value;JZLfixture/Sink;)V" not in ops[0][1]):
                    raise RuntimeError("Original marker forwarding body changed")
                witnessed=True
            if not witnessed:raise RuntimeError("Missing original two-instruction marker forwarding witness")
        mutations=(("wrong-value","Core","this(value,bits,enabled,(Sink<Value>)sink);","this(null,bits,enabled,(Sink<Value>)sink);",0),
                   ("lost-wide","Child","super(value,bits,enabled,sink,marker);","super(value,(int)bits,enabled,sink,marker);",0),
                   ("wrong-boolean","Core","this(value,bits,enabled,(Sink<Value>)sink);","this(value,bits,!enabled,(Sink<Value>)sink);",0),
                   ("early-stamp","Child","Trace.hit(4);return value;","stamp=bits;Trace.hit(4);return value;",0),
                   ("skipped-super-effect","Base","Trace.base=this;Trace.hit(8);","Trace.base=this;",0))
        if args.final_owner:
            mutations=tuple(m for m in mutations if m[1]!="Child")+(
                ("lost-wide","Core","this.bits=bits;","this.bits=(int)bits;",0),
                ("early-callback","Core","Trace.hit(2);sink.accept(current());","sink.accept(current());Trace.hit(2);",0))
        if args.variant_input:
            mutations=tuple((label,name,before.replace("Sink<Value>",variant_sink),after.replace("Sink<Value>",variant_sink),mode)
                            for label,name,before,after,mode in mutations)
            mutations += (("changed-nested-contract","Core",variant_sink,
                           variant_sink.replace(", ? extends Box<",", ? super Box<"),0),)
        if args.mixed_star_access and args.mixed_star_access!="inherited-marker":
            mutations=tuple((label,"Core" if name=="Base" else name,before,after,mode)
                            for label,name,before,after,mode in mutations)
        for label,name,before,after,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=(3 if label=="changed-nested-contract" else 1):raise RuntimeError("Original inherited fixed marker counterexample anchor changed")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.open("x").write(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed,*([fixture/"fixture/Core.java"] if name!="Core" else [])],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                runtime_harness,"1",str(mode),"false"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            marker="owned final fixed marker" if args.final_owner else "owned inherited fixed marker"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect inherited fixed marker: "+label)
        if args.nominal_bound:
            wrong=work/"changed-bound-classes"/"fixture";wrong.mkdir(parents=True)
            path=wrong/"Sink.class";shutil.copy2(originals/"fixture/Sink.class",path)
            original=("<E:"+(":" if args.nominal_bound=="interface" else "")+"Lfixture/"+bound+";>Ljava/lang/Object;").encode()
            receipt=change_signature(path,original,b"<E:Ljava/lang/Object;>Ljava/lang/Object;")
            (work/"changed-bound-witness.json").open("x").write(json.dumps(receipt,indent=2)+"\n")
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parent,originals,harness))),
                runtime_harness,"1","0","true"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/"changed-bound-counterexample.log").open("x").write(result.stdout+result.stderr)
            marker="owned final fixed marker" if args.final_owner else "owned inherited fixed marker"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Frozen observer missed unchanged-Code declaration bound drift")
        if args.baseline_only:
            if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                    or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                    or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
            evidence={"status":"baseline-only","class_count":len(classes),"method_count":len(methods),
                "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
                "open_owner":args.open_owner,
                "final_owner":args.final_owner,"variant_input":args.variant_input,"nominal_bound":args.nominal_bound,"mixed_star_access":args.mixed_star_access,
                "independent_wrong_programs":len(mutations)+bool(args.nominal_bound),"marker_flag_only_Code_and_all_other_metadata":"unchanged"}
            (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
            return 0
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0)!=int(marker_synthetic) or coverage["status"]!=("partial" if marker_synthetic else "recovered")
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Inherited fixed marker recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Sink;","Lfixture/Tagged;"} }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Inherited fixed marker recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original inherited fixed marker class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                wanted=deepcopy(metadata)
                if identity=="Lfixture/Core;-><init>"+marker_prototype:wanted["access"] &= ~0x1000
                if after[c]["methods"].get(identity)!=wanted or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original inherited fixed marker method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,"rebuilt",False)!=baseline:raise RuntimeError("Inherited fixed marker recovery changed ordered superclass/callback effects")
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,
            "open_owner":args.open_owner,
            "final_owner":args.final_owner,"variant_input":args.variant_input,"nominal_bound":args.nominal_bound,"mixed_star_access":args.mixed_star_access,
            "inherited_owner_fixed_private_target_and_concrete_callback":"matched","all_original_entries_and_metadata":"matched except reported marker constructor synthetic flag" if marker_synthetic else "matched",
            "superclass_subclass_dispatch_callback_partial_fields_marker_wide_boolean_and_failures":"matched",
            "independent_wrong_programs":len(mutations)+bool(args.nominal_bound),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
