#!/usr/bin/env python3
"""Verify genuine Kotlin compatibility accessors retain exact interface default dispatch."""
from __future__ import annotations
import argparse
from collections import Counter
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
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile

OWNER="Lfixture/DefaultApi;"
DEFAULT=OWNER+"->work(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;"
ACCESS=OWNER+"->access$work$jd(Lfixture/DefaultApi;Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;"
LOCAL="Lfixture/DefaultApi$work$callback$1;"

class DefaultFixtureClassFile(KotlinFixtureClassFile):
    SCHEMAS={**KotlinFixtureClassFile.SCHEMAS,"Ljava/lang/Deprecated;":{}}
    def deprecation(self,attributes,context,*,allowed=True,**unused):
        result={"deprecated_attribute":False,"runtime_visible_annotations":[],
                "runtime_invisible_annotations":[],"runtime_invisible_parameter_annotations":[]}
        seen=set();require=class_identity.require
        def element(reader,expected):
            tag=reader.take(1).decode("ascii");require(tag==expected[0],"Changed owned annotation value type")
            if tag=="[":return {"tag":tag,"values":[element(reader,expected[1:]) for _ in range(reader.number(2))]}
            require(tag in ("I","s"),"Unsupported owned annotation value")
            index=reader.number(2)
            return {"tag":tag,"value":self.text(index) if tag=="s" else int.from_bytes(self.constant(index,3),"big",signed=True)}
        def annotations(reader,kind):
            rows=[];types=set()
            for _ in range(reader.number(2)):
                owner=self.text(reader.number(2));require(owner in self.SCHEMAS and owner not in types,"Unknown/duplicate owned annotation on "+context);types.add(owner)
                visible=owner in {"Lkotlin/Metadata;","Lkotlin/jvm/JvmStatic;","Ljava/lang/Deprecated;"}
                require((kind=="RuntimeVisibleAnnotations")==visible,"Changed original annotation retention")
                if kind=="RuntimeInvisibleParameterAnnotations":require(owner in {"Lorg/jetbrains/annotations/Nullable;","Lorg/jetbrains/annotations/NotNull;"},"Unknown parameter annotation")
                schema=self.SCHEMAS[owner];values={}
                for _ in range(reader.number(2)):
                    name=self.text(reader.number(2));require(name in schema and name not in values,"Unknown/duplicate annotation element")
                    values[name]=element(reader,schema[name])
                require(owner=="Lkotlin/Metadata;" or set(values)==set(schema),"Incomplete annotation elements")
                rows.append({"type":owner,"elements":values})
            return sorted(rows,key=lambda r:r["type"])
        for kind,data in attributes:
            if kind not in class_identity.ANNOTATION_ATTRIBUTES:continue
            require(allowed and kind not in seen,"Duplicate/unsupported annotation attribute");seen.add(kind)
            if kind=="Deprecated":
                require(not data,"Nonempty Deprecated attribute");result["deprecated_attribute"]=True;continue
            require(kind in {"RuntimeVisibleAnnotations","RuntimeInvisibleAnnotations","RuntimeInvisibleParameterAnnotations"},"Unknown owned annotation attribute")
            reader=class_identity.Bytes(data)
            if kind=="RuntimeInvisibleParameterAnnotations":result["runtime_invisible_parameter_annotations"]=[annotations(reader,kind) for _ in range(reader.number(1))]
            else:result["runtime_visible_annotations" if kind=="RuntimeVisibleAnnotations" else "runtime_invisible_annotations"]=annotations(reader,kind)
            reader.finish()
        return result

def compiler_facts(root):
    result={}
    for path in sorted(root.rglob("*.class")):
        row=DefaultFixtureClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix()!=row["name"][1:-1]+".class":raise RuntimeError("Duplicate/misplaced original compiler class")
        result[row["name"]]=row
    return result

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar","kotlin-lib","kotlin-java-home"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--generic-owner",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interface-default-access-")
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),600)
        fixture=FIXTURES/"interface_default_access";kotlin=args.kotlin_lib.resolve()
        kotlin_sources=fixture/("kotlin_generic" if args.generic_owner else "kotlin")
        inputs=[args.android_jar.resolve(),*sorted(kotlin.glob("*.jar"))]
        if not (kotlin/"kotlin-compiler.jar").is_file():raise RuntimeError("Original Kotlin compiler is unavailable")
        input_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        dependencies=[]
        for p in (kotlin/"kotlin-stdlib.jar",kotlin/"annotations-13.0.jar"):
            dest=work/p.name
            with zipfile.ZipFile(p) as before,zipfile.ZipFile(dest,"x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):raise RuntimeError("Unsafe compiler dependency entry")
                        after.writestr(entry.filename,before.read(entry))
            dependencies.append(dest)
        dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        original_java,originals,harness=work/"original-java",work/"originals",work/"harness"
        java_sources=sorted((fixture/"java").rglob("*.java"))
        if args.generic_owner:java_sources+=sorted((fixture/"java_generic").rglob("*.java"))
        verify.compile_local(java_sources,original_java,release=8);originals.mkdir()
        compiler=[choose_jdk(args.kotlin_java_home)/"bin/java","-cp",kotlin/"*","org.jetbrains.kotlin.cli.jvm.K2JVMCompiler","-no-stdlib","-no-reflect",
                  "-classpath",os.pathsep.join(map(str,(kotlin/"kotlin-stdlib.jar",original_java))),"-jvm-target","1.8","-Xjvm-default=all-compatibility"]
        verify.run([*compiler,"-d",originals,*sorted(kotlin_sources.rglob("*.kt"))],"kotlinc-original")
        for p in original_java.rglob("*.class"):
            dest=originals/p.relative_to(original_java);dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,dest)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in ("InterfaceDefaultAccessHarness","InterfaceDefaultAccessCaller")],harness,
            classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        classes,methods=compiler_inventory(verify,originals);records,exceptions=compiler_facts(originals),exception_inventory(originals)
        methods=set(methods)
        if len(classes)!=(8 if args.generic_owner else 7) or len(methods)!=(14 if args.generic_owner else 13) or records[LOCAL]["enclosing_method"]!={"owner":OWNER,"name":"work","prototype":"(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;"}:
            raise RuntimeError("Original Kotlin default/accessor/local inventory changed")
        def execute(runtime,name,label):
            outputs=[]
            for route in range(4):
                for variant in range(2):
                    for mode in range(10):
                        output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,(*runtime,*dependencies))),"InterfaceDefaultAccessHarness",
                            str(route),str(variant),str(mode),name],f"{label}-{route}-{variant}-{mode}")
                        pattern=f"route={route},variant={variant},mode={mode},attempt=[01],defaultTrace=[0-9]+"
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError("Original interface default matrix changed: "+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"work","baseline")
        classpath=["--classpath",args.android_jar.resolve()]
        for p in dependencies:classpath += ["--classpath",p]
        dex=work/"dex";dex.mkdir()
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*sorted(originals.rglob("*.class"))],"d8-original")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original default DEX count changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:raise RuntimeError("D8 changed an original default entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        annotations=verify.run([verify.dexdump,"-a",source_dex],"sdk-annotations")
        visible=Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)",annotations))
        if "VISIBILITY_BUILD" in annotations or visible!=Counter({"Lkotlin/Metadata;":5,"Ljava/lang/Deprecated;":1}):raise RuntimeError("D8 changed original Kotlin/Deprecated annotation retention")
        expected=deepcopy(records);dropped={}
        for c in classes:
            for identity,row in [(c,expected[c]),*expected[c]["fields"].items(),*expected[c]["methods"].items()]:
                for key in ("runtime_invisible_annotations","runtime_invisible_parameter_annotations"):
                    if row.get(key):dropped.setdefault(identity,{})[key]=row[key]
                    row[key]=[]
        (work/"d8-class-annotation-loss.json").write_text(json.dumps(dropped,indent=2)+"\n")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"--timeout","600","-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"];rows={r["identity"]:r for r in coverage["methods"]}
        if (set(rows)!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"]!="partial" or rows[DEFAULT].get("projection_kind")!="interface-default-body"
                or rows[ACCESS].get("projection_kind")!="static-interface-default-accessor"):raise RuntimeError("Interface default recovery changed original inventory or dispatch receipt")
        helper=rows[DEFAULT]["default_body_helper"];name=helper.split("->",1)[1].split("(",1)[0]
        if rows[ACCESS].get("default_body_helper")!=helper:raise RuntimeError("Default and static entries duplicated the original body")
        bindings=coverage.get("class_source_bindings",[])
        if len(bindings)!=1 or bindings[0]["class"]!=LOCAL or bindings[0].get("source_enclosing_method",{}).get("identity")!=helper:raise RuntimeError("Missing exact original/source default enclosure receipt")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=os.pathsep.join(map(str,dependencies)),release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        candidates=[c for c in actual_classes if c.endswith(bindings[0]["source_name"]+";")]
        if len(candidates)!=1:raise RuntimeError("Ambiguous relocated local class identity")
        replacement=candidates[0]
        constructor=LOCAL+"-><init>(Ljava/lang/Object;)V"
        def mapped(identity):
            if identity==constructor:identity=LOCAL+"-><init>("+OWNER+"Ljava/lang/Object;)V"
            return identity.replace(LOCAL,replacement)
        helpers={mapped(r["class"]+"->"+r["name"]+r["prototype"]) for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={mapped(c)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes-{"Lfixture/Callback;"}}
        expected_helpers |= {helper,OWNER.replace(";","$DefaultImpls;")+"-><init>()V"}
        if actual_classes!={mapped(c) for c in classes} or set(actual_methods)!= {mapped(m) for m in methods}|expected_helpers or helpers!=expected_helpers:raise RuntimeError("Default/local recovery invented or omitted an entry/helper")
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                wanted=deepcopy(expected[c].get(key))
                if c==LOCAL:
                    if key=="access":wanted &= ~1
                    if key=="inner_class":wanted.update(inner=replacement,name=bindings[0]["source_name"],access=0x10)
                    if key=="enclosing_method":wanted["name"]=name
                    if key=="fields":
                        wanted={mapped(k):v for k,v in wanted.items()}
                        for row in wanted.values():row["access"] &= ~0x1000
                if after[mapped(c)].get(key)!=wanted:raise RuntimeError("Undisclosed default/local class metadata changed: "+c+":"+key)
            for identity,metadata in expected[c]["methods"].items():
                wanted=deepcopy(metadata)
                if identity==ACCESS:wanted["access"] &= ~0x1000
                if identity==constructor:wanted["prototype"]="("+OWNER+"Ljava/lang/Object;)V"
                if after[mapped(c)]["methods"].get(mapped(identity))!=wanted or after_exceptions.get(mapped(identity))!=exceptions[identity]:raise RuntimeError("Undisclosed default/local method metadata or Exceptions changed: "+identity)
        if execute((rebuilt,harness),name,"rebuilt")!=baseline:raise RuntimeError("Default access changed overrides, receiver identity, effects, failures or enclosure")
        text=(kotlin_sources/"fixture/DefaultApi.kt").read_text()
        mutations=(("wrong-result","return result","return Any()",0),("lost-effect","Trace.hit(3)","Trace.hit(8)",0))
        for label,old,new,mode in mutations:
            if text.count(old)!=1:raise RuntimeError("Default counterexample anchor changed")
            wrong=work/label/"DefaultApi.kt";wrong.parent.mkdir();wrong.write_text(text.replace(old,new))
            variant=work/(label+"-classes");variant.mkdir()
            verify.run([*compiler,"-d",variant,wrong],"kotlinc-"+label)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),"InterfaceDefaultAccessHarness","1","1",str(mode),"work"],text=True,capture_output=True,timeout=600,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned interface default access" not in result.stderr:raise RuntimeError("Frozen default observer missed "+label)
        wrong=work/"virtual-dispatch/InterfaceDefaultAccessCaller.java";wrong.parent.mkdir()
        text=(FIXTURES/"harness/InterfaceDefaultAccessCaller.java").read_text()
        old="return DefaultApi.DefaultImpls.work(receiver, value, tag);"
        if text.count(old)!=1:raise RuntimeError("Independent default dispatch caller anchor changed")
        wrong.write_text(text.replace(old,"return receiver.work(value, tag);"))
        variant=work/"virtual-dispatch-classes"
        verify.compile_local([wrong],variant,classpath=os.pathsep.join(map(str,(originals,*dependencies))),release=8)
        result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(variant,originals,harness,*dependencies))),"InterfaceDefaultAccessHarness","1","1","0","work"],text=True,capture_output=True,timeout=600,env=verify.environment)
        (work/"virtual-dispatch-counterexample.log").write_text(result.stdout+result.stderr)
        if result.returncode==0 or "owned interface default access" not in result.stderr:raise RuntimeError("Frozen observer missed virtual dispatch in the default entry")
        if hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}!=input_hashes or {str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}!=dependency_hashes:raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","generic_owner":args.generic_owner,"class_count":len(classes),"method_count":len(methods),"fresh_jvm_cases_per_stage":len(baseline),"stages":2,"attempts_per_case":2,
            "default_dispatch_bypasses_override":"matched","ordinary_virtual_override_dispatch":"matched","one_original_local_body_and_enclosure":"disclosed",
            "original_receiver_capture_result_and_exceptions":"matched","original_code_bytes":"compiler output unchanged","independent_counterexamples":3,
            "compiler_CLASS_annotation_loss_sites":len(dropped),"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
