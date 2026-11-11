#!/usr/bin/env python3
"""Verify private nullary factories retain owned package carriers and allocation effects."""
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
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-view",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    parser.add_argument("--closed-scalar-result",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-nullary-carriers-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"private_nullary_carriers/java"
        harness_name="PrivateNullaryCarrier"
        observers=[FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.closed_scalar_result:
            copied=work/"closed-scalar-java";shutil.copytree(fixture,copied);fixture=copied
            (fixture/"fixture/Owner.java").write_text('''package fixture;
public final class Owner {
    static {Trace.hit(1);}
    private static <K,V> Box<Object,Object> make(int capacity) {
        Trace.hit(2);
        Box<Object,Object> allocated=new Box<Object,Object>(capacity);
        Trace.allocated=allocated;Trace.hit(6);
        return Trace.override?(Box<Object,Object>)Trace.result:allocated;
    }
    public static Box<?,?> call() {return make(Trace.capacity);}
    public static String text() {return (String)make(Trace.capacity).get();}
}
''')
            (fixture/"fixture/Box.java").write_text('''package fixture;
public final class Box<A,B> implements View<Object> {
    static {Trace.hit(3);}
    public final float[] values;
    public final Object value,other;
    public final int capacity;
    public Box(int capacity) {
        Trace.partial=this;Trace.hit(4);
        this.values=Trace.values;this.value=Trace.seed;this.other=Trace.other;this.capacity=capacity;Trace.hit(5);
    }
    public Object get() {Trace.hit(7);return value;}
}
''')
            trace=fixture/"fixture/Trace.java";text=trace.read_text()
            text=text.replace("public final class Trace {","public final class Trace {\n    public static int capacity;\n    public static Object other;\n    public static java.util.function.IntConsumer observer;")
            text=text.replace("events=events*10+event;","events=events*10+event;\n        if(observer!=null)observer.accept(event);")
            trace.write_text(text)
            copied=[]
            for original in observers:
                text=original.read_text().replace("Box<?>","Box<?,?>")
                if original.name.endswith("Harness.java"):
                    anchor="        boolean checked=mode>=13;"
                    if text.count(anchor)!=1:raise RuntimeError("Frozen closed factory reflection anchor changed")
                    text=text.replace(anchor,anchor+'''
        java.lang.reflect.Method factory=Owner.class.getDeclaredMethod("make",int.class);
        java.lang.reflect.TypeVariable<?>[] vars=factory.getTypeParameters();
        require(java.lang.reflect.Modifier.isPrivate(factory.getModifiers())&&java.lang.reflect.Modifier.isStatic(factory.getModifiers()));
        require(vars.length==2&&vars[0].getName().equals("K")&&vars[1].getName().equals("V")&&vars[0].getBounds()[0]==Object.class&&vars[1].getBounds()[0]==Object.class);
        java.lang.reflect.ParameterizedType returns=(java.lang.reflect.ParameterizedType)factory.getGenericReturnType();
        require(returns.getRawType()==Box.class&&java.util.Arrays.equals(returns.getActualTypeArguments(),new java.lang.reflect.Type[]{Object.class,Object.class}));''')
                    anchor="            Trace.events=0;Trace.partial=Trace.allocated=untouched;"
                    if text.count(anchor)!=1:raise RuntimeError("Frozen scalar capacity setup anchor changed")
                    text=text.replace(anchor,anchor+'''
            int capacity=attempt==0?Integer.MIN_VALUE+17:Integer.MAX_VALUE-23;
            Object other=mode==10||mode==15?null:new Object();
            Trace.capacity=capacity;Trace.other=other;
            Trace.observer=event->{if(event==2||event==3)Trace.capacity=1777;};''')
                    text=text.replace("if(allocated)require(Trace.allocated==partial&&partial!=previous);",
                                      "require(partial.capacity==(fields?capacity:0)&&partial.other==(fields?other:null));\n                if(allocated)require(Trace.allocated==partial&&partial!=previous);")
                target=work/original.name;target.open("x").write(text);copied.append(target)
            observers=copied
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(observers,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=4 or len(all_methods)!=11:raise RuntimeError("Original private nullary carrier inventory changed")
        declarations={"Lfixture/View;"}
        classes=all_classes-(declarations if args.dependency_view else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        identity="Lfixture/Owner;->make()Lfixture/Box;"
        expected="<T:Ljava/lang/Object;>()Lfixture/Box<TT;>;"
        box_signature="<T:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/View<TT;>;"
        if args.closed_scalar_result:
            identity="Lfixture/Owner;->make(I)Lfixture/Box;"
            expected="<K:Ljava/lang/Object;V:Ljava/lang/Object;>(I)Lfixture/Box<Ljava/lang/Object;Ljava/lang/Object;>;"
            box_signature="<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/View<Ljava/lang/Object;>;"
        if records["Lfixture/Owner;"]["methods"][identity]["signature"]!=expected:
            raise RuntimeError("Original private nullary factory and invariant result changed")
        if records["Lfixture/Box;"]["signature"]!=box_signature:
            raise RuntimeError("Original package carrier inheritance contract changed")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in ("View",):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_view:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(24):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"fixture."+harness_name+"Harness",str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original private nullary carrier callback matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original private nullary carrier DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original private nullary carrier entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-argument-disassembly")
        if "Lfixture/Owner;.make:" not in disassembly or "check-cast" not in disassembly:
            raise RuntimeError("D8 lost the private factory or original later cast witness")
        mutations=(("duplicate-allocation","Owner","Box<T> allocated=new Box<T>(Trace.values,(T)Trace.seed);","new Box<T>(Trace.values,(T)Trace.seed); Box<T> allocated=new Box<T>(Trace.values,(T)Trace.seed);",0),
                   ("lost-override","Owner","Trace.override?(Box<T>)Trace.result:allocated","allocated",9),
                   ("late-allocated-field","Owner","Trace.allocated=allocated;Trace.hit(6);","Trace.hit(6);Trace.allocated=allocated;",6),
                   ("early-constructor-fields","Box","Trace.partial=this;Trace.hit(4);\n        this.values=values;this.value=value;Trace.hit(5);","Trace.partial=this;this.values=values;this.value=value;Trace.hit(4);Trace.hit(5);",4),
                   ("wrong-array","Box","this.values=values;","this.values=null;",0),
                   ("lost-getter-effect","Box","Trace.hit(7);return value;","return value;",13))
        if args.closed_scalar_result:
            allocation="Box<Object,Object> allocated=new Box<Object,Object>(capacity);"
            fields="this.values=Trace.values;this.value=Trace.seed;this.other=Trace.other;this.capacity=capacity;"
            mutations=(("duplicate-allocation","Owner",allocation,"new Box<Object,Object>(capacity); "+allocation,0),
                       ("lost-override","Owner","Trace.override?(Box<Object,Object>)Trace.result:allocated","allocated",9),
                       ("late-allocated-field","Owner","Trace.allocated=allocated;Trace.hit(6);","Trace.hit(6);Trace.allocated=allocated;",6),
                       ("early-constructor-fields","Box","Trace.partial=this;Trace.hit(4);\n        "+fields+"Trace.hit(5);","Trace.partial=this;"+fields+"Trace.hit(4);Trace.hit(5);",4),
                       ("wrong-array","Box","this.values=Trace.values;","this.values=null;",0),
                       ("lost-getter-effect","Box","Trace.hit(7);return value;","return value;",13),
                       ("late-capacity","Owner","new Box<Object,Object>(capacity)","new Box<Object,Object>(Trace.capacity)",0),
                       ("lost-other","Box","this.other=Trace.other;","this.other=null;",0))
        for label,owner,before,after,mode in mutations:
            text=(fixture/("fixture/"+owner+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original private nullary carrier counterexample anchor changed")
            changed=work/label/(owner+".java");changed.parent.mkdir();changed.open("x").write(text.replace(before,after))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),"fixture."+harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned private nullary carrier observer" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect private nullary carrier: "+label)
        evidence={"status":"baseline-only" if args.baseline_only else "success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_view":args.dependency_view,
            "closed_scalar_result":args.closed_scalar_result,
            "independent_wrong_programs":len(mutations)}
        if not args.baseline_only:
            output=work/"recovered"
            verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
            coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                    or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                    or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                    or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
                raise RuntimeError("Private nullary carrier recovery changed original entries or metadata contracts")
            rebuilt=work/"rebuilt"
            verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_view else None,release=coverage.get("minimum_java_release",8))
            actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
            helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
            expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/View;"}}
            if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
                raise RuntimeError("Private nullary carrier recovery invented or omitted a runtime entry")
            after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
            for c in classes:
                for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                    if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original private nullary carrier class metadata changed: "+c+":"+key)
                for identity,metadata in records[c]["methods"].items():
                    if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                        raise RuntimeError("Original private nullary carrier method metadata or Exceptions changed: "+identity)
            runtime=(rebuilt,harness,dependency) if args.dependency_view else (rebuilt,harness)
            if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Private nullary carrier recovery changed callback order or result/failure identity")
            evidence.update({"all_original_entries_and_metadata":"unchanged","complete_private_nullary_result_and_owned_package_scope":"matched",
                "allocation_class_init_partial_final_fields_exact_returns_and_late_casts":"matched"})
            if args.closed_scalar_result:
                controls=[]
                signature=expected.encode()
                for label,changed in (
                    ("bounded-unused-formal",signature.replace(b"K:Ljava/lang/Object;",b"K:Ljava/lang/Number;")),
                    ("extra-unused-formal",signature.replace(b"V:Ljava/lang/Object;>",b"V:Ljava/lang/Object;W:Ljava/lang/Object;>")),
                    ("unknown-result-layer",signature.replace(b"<Ljava/lang/Object;Ljava/lang/Object;>",b"<Ljava/lang/Object;Lmissing/Leaf;>")),
                ):
                    negative=work/(label+"-originals");shutil.copytree(originals,negative)
                    receipt=change_signature(negative/"fixture/Owner.class",signature,changed)
                    (work/(label+"-encoding.json")).open("x").write(json.dumps(receipt,indent=2)+"\n")
                    negative_dex=work/(label+"-dex");negative_dex.mkdir()
                    negative_paths=[negative/p.relative_to(originals) for p in paths]
                    verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",negative_dex,*negative_paths],label+"-d8")
                    rejected=work/(label+"-rejected")
                    result=subprocess.run([str(args.neverd.resolve()),"mobile",str(negative_dex/"classes.dex"),
                        *map(str,classpath),"-o",str(rejected)],text=True,capture_output=True,timeout=300,env=verify.environment)
                    (work/(label+"-recovery.log")).open("x").write(result.stdout+result.stderr)
                    if result.returncode==0 or rejected.exists() or "generic invocation requires" not in result.stderr:
                        raise RuntimeError("Closed unused factory accepted an unproved complete contract: "+label)
                    controls.append(label)
                evidence["same_code_signature_counterexamples"]=controls
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence["original_inputs_and_immutable_callers"]="unchanged"
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
