#!/usr/bin/env python3
"""Verify complete closed constructor casts after an original allocation prefix."""
from __future__ import annotations
import argparse
import copy
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
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag


TERMINAL_SOURCE = b"Ljava/lang/Object;Lfixture/View;Lfixture/Marker<Lfixture/Box<Lfixture/Value;>;>;"
TERMINAL_INPUT = TERMINAL_SOURCE.replace(b"Lfixture/View;", b"Lfixture/View<Lfixture/Box<Lfixture/Value;>;>;")
TERMINAL_BRIDGE = "Lfixture/Core;->accept(Ljava/lang/Object;)Z"
TERMINAL_TARGET = "Lfixture/Core;->accept(Lfixture/Box;)Z"


def terminal_owner(path: Path) -> dict:
    receipt = change_signature(path, TERMINAL_SOURCE, TERMINAL_INPUT)
    receipt["bridge_flags"] = bridge_flag(path, "accept", "(Ljava/lang/Object;)Z", 1, 0x1041)
    return receipt


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-inputs",action="store_true")
    parser.add_argument("--baseline-only",action="store_true")
    parser.add_argument("--closed-owner",action="store_true")
    parser.add_argument("--private-target",action="store_true")
    parser.add_argument("--terminal-owner",action="store_true")
    args=parser.parse_args()
    if args.terminal_owner:args.closed_owner=True
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-allocating-constructor-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"closed_allocating_constructor/java"
        harness_name="ClosedAllocatingConstructor"
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in (harness_name+"Caller",harness_name+"Harness")]
        if args.closed_owner or args.private_target:
            authored=work/'owner-source';shutil.copytree(fixture,authored);fixture=authored
            path=fixture/'fixture/Core.java';text=path.read_text()
            if args.closed_owner:
                anchor='public final class Core {'
                if text.count(anchor)!=1:raise RuntimeError('Original closed owner anchor changed')
                text=text.replace(anchor,'public final class Core implements '+('View, Marker<Box<Value>>' if args.terminal_owner else 'View<Box<Value>>, Marker')+' {')
                (fixture/'fixture/View.java').open('x').write('package fixture; public interface View<E> {'+(' boolean accept(E value);' if args.terminal_owner else '')+'}\n')
                (fixture/'fixture/Marker.java').open('x').write('package fixture; public interface Marker'+('<E>' if args.terminal_owner else '')+' {}\n')
                if args.terminal_owner:
                    text=text.replace('\n}', '\n    public boolean accept(Object value) { throw new UnsupportedOperationException("owned closed operation"); }\n'
                        '    public boolean accept(Box<Value> value) { throw new UnsupportedOperationException("owned closed operation"); }\n}')
            if args.private_target:
                anchor='public Core(Box<Value> box,'
                if text.count(anchor)!=1:raise RuntimeError('Original selected private constructor anchor changed')
                text=text.replace(anchor,'private Core(Box<Value> box,')
            path.write_text(text)
            observers=work/'owner-harness';observers.mkdir();frozen=observers/harness_sources[1].name
            text=harness_sources[1].read_text();anchor='RuntimeException failure=new IllegalStateException'
            checks=('require(Core.class.getTypeParameters().length==0);\n'
                    '        require(Modifier.isPrivate(Core.class.getDeclaredConstructor(Box.class,Callback.class).getModifiers())=='
                    +str(args.private_target).lower()+');\n        ')
            if args.terminal_owner:
                checks+=('Type[] edges=Core.class.getGenericInterfaces();require(edges.length==2);\n'
                         '        if(args.length>2 && args[2].equals("rebuilt"))require(edges[0]==View.class);\n'
                         '        else {ParameterizedType edge=(ParameterizedType)edges[0];require(edge.getRawType()==View.class);\n'
                         '            ParameterizedType nested=(ParameterizedType)edge.getActualTypeArguments()[0];\n'
                         '            require(nested.getRawType()==Box.class && nested.getActualTypeArguments()[0]==Value.class);}\n'
                         '        ParameterizedType marker=(ParameterizedType)edges[1];require(marker.getRawType()==Marker.class);\n'
                         '        ParameterizedType nested=(ParameterizedType)marker.getActualTypeArguments()[0];\n'
                         '        require(nested.getRawType()==Box.class && nested.getActualTypeArguments()[0]==Value.class);\n        ')
                point='            System.out.println("route="+route'
                if text.count(point)!=1:raise RuntimeError('Original terminal observer anchor changed')
                text=text.replace(point,'            if(result!=null)terminal(result,route);\n'+point)
                point='    public static void main(String[] args) throws Throwable {'
                helper='''    static void terminal(Core core,int route) throws Throwable {
        long events=Trace.events;Throwable previous=null;
        for(Object value:new Object[]{null,new Object(),new Value(),Trace.box}){
            Throwable caught=null;
            try{
                if(route==0)core.accept(value);
                else if(route==1)((View)core).accept(value);
                else if(route==2)try{Core.class.getMethod("accept",Object.class).invoke(core,value);}catch(InvocationTargetException e){throw e.getCause();}
                else {boolean unused=(boolean)MethodHandles.lookup().unreflect(Core.class.getMethod("accept",Object.class)).invokeExact(core,value);}
            }catch(Throwable error){caught=error;}
            require(caught instanceof UnsupportedOperationException && "owned closed operation".equals(caught.getMessage()));
            require(caught!=previous && Trace.events==events);previous=caught;
        }
    }
'''
                if text.count(point)!=1:raise RuntimeError('Original terminal method observer anchor changed')
                text=text.replace(point,helper+point)
            elif args.closed_owner:
                checks+=('Type[] edges=Core.class.getGenericInterfaces();require(edges.length==2 && edges[1]==Marker.class);\n'
                         '        ParameterizedType edge=(ParameterizedType)edges[0];require(edge.getRawType()==View.class);\n'
                         '        ParameterizedType nested=(ParameterizedType)edge.getActualTypeArguments()[0];\n'
                         '        require(nested.getRawType()==Box.class && nested.getActualTypeArguments()[0]==Value.class);\n        ')
            if text.count(anchor)!=1:raise RuntimeError('Original closed owner observer anchor changed')
            frozen.open('x').write(text.replace(anchor,checks+anchor));harness_sources=[harness_sources[0],frozen]
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        if args.terminal_owner:
            (work/'input-metadata-change.json').open('x').write(json.dumps(terminal_owner(originals/'fixture/Core.class'),indent=2)+'\n')
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=(9 if args.closed_owner else 7) or len(all_methods)!=(15 if args.terminal_owner else 12):raise RuntimeError("Original closed constructor inventory changed")
        declared_names=("Box","Callback","Value","Label")+(("View","Marker") if args.closed_owner else ())
        declarations={"Lfixture/"+n+";" for n in declared_names}
        classes=all_classes-(declarations if args.dependency_inputs else ({'Lfixture/View;'} if args.terminal_owner else set()))
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/"owned-inputs.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for n in declared_names:jar.writestr("fixture/"+n+".class",(originals/("fixture/"+n+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs or args.terminal_owner:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(16):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",str(route),str(mode),label],f"{label}-{route}-{mode}")
                    if len(output.splitlines())!=2 or not all(re.fullmatch(f"route={route},mode={mode},attempt=[01],trace=[0-9]+",line) for line in output.splitlines()):
                        raise RuntimeError("Closed constructor observer output changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original closed constructors DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original closed constructor entry")
        (work/"sdk-inventory.json").open("x").write(json.dumps(inventory,indent=2)+"\n")
        mutations=(("lost-wide","Box","this.bits=bits;","this.bits=0;",0),
                   ("lost-allocation","Core","new Box<Value>(bits)","(Box<Value>)Trace.box",0),
                   ("early-callback","Core","Trace.hit(6);","callback.apply(Trace.value,Trace.label);Trace.hit(6);",0),
                   ("lost-result","Core","this.result=result;","this.result=null;",0),
                   ("late-field","Core","this.box=box;Trace.hit(5);","Trace.hit(5);this.box=box;",12))
        if args.terminal_owner:
            mutations+=(("added-bridge-cast","Core","accept(Object value) { throw","accept(Object value) { Box<?> checked=(Box<?>)value;throw",0),)
        for label,owner,old,new,mode in mutations:
            source=(fixture/("fixture/"+owner+".java")).read_text()
            if source.count(old)!=1:raise RuntimeError("Closed constructor wrong-program anchor changed")
            changed=work/label/(owner+".java");changed.parent.mkdir();changed.open("x").write(source.replace(old,new))
            wrong=work/(label+"-classes");verify.compile_local([changed],wrong,classpath=originals,release=8)
            if args.terminal_owner and owner=='Core':terminal_owner(wrong/'fixture/Core.class')
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned closed allocating constructor" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect constructor behavior: "+label)
        evidence={"status":"baseline-only" if args.baseline_only else "success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,"independent_wrong_programs":len(mutations)}
        evidence.update({'closed_owner':args.closed_owner,'private_target':args.private_target,'terminal_owner':args.terminal_owner})
        if not args.baseline_only:
            output=work/"recovered"
            verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
            coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
            if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes) or coverage["method_count"]!=len(methods)
                    or coverage["unrecovered_method_count"] or coverage.get("projected_method_count",0)!=(1 if args.terminal_owner else 0) or coverage["status"]!=("partial" if args.terminal_owner else "recovered")
                    or coverage.get("class_source_bindings") or coverage.get("field_source_bindings") or (coverage.get("generic_signature_projections") and not args.terminal_owner)):
                raise RuntimeError("Closed constructor binding changed original entries or generic contracts")
            if args.terminal_owner:
                rows=coverage.get('generic_signature_projections',[])
                projected=[r for r in coverage['methods'] if r['status']=='source-projected']
                expected_change={'kind':'terminal-throw-raw-interface','offset':18,
                    'original_type':TERMINAL_INPUT.decode(),'source_type':'Lfixture/View;','scope_origin':TERMINAL_BRIDGE}
                if (len(rows)!=1 or rows[0]['declaration']!='Lfixture/Core;' or rows[0]['declaration_kind']!='class'
                        or rows[0]['original_signature']!=TERMINAL_INPUT.decode() or rows[0]['changes']!=[expected_change]
                        or len(projected)!=1 or projected[0]['identity']!=TERMINAL_BRIDGE
                        or projected[0]['source_identity']!=TERMINAL_BRIDGE or projected[0]['bridge_target']!=TERMINAL_TARGET
                        or projected[0]['projection_kind']!='terminal-throw-interface-bridge'):
                    raise RuntimeError('Closed constructor lost its separate terminal interface proof')
            rebuilt=work/"rebuilt"
            verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,classpath=dependency if args.dependency_inputs or args.terminal_owner else None,release=coverage.get("minimum_java_release",8))
            actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
            helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
            expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in {"Lfixture/Callback;","Lfixture/View;","Lfixture/Marker;"}}
            if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
                raise RuntimeError("Closed constructor binding invented or omitted a runtime entry")
            after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
            for c in classes:
                for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method","deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                    expected=TERMINAL_SOURCE.decode() if args.terminal_owner and c=='Lfixture/Core;' and key=='signature' else records[c].get(key)
                    if expected!=after[c].get(key):raise RuntimeError("Original closed constructor metadata changed : "+c+":"+key)
                for identity,metadata in records[c]["methods"].items():
                    expected=copy.deepcopy(metadata)
                    if args.terminal_owner and identity==TERMINAL_BRIDGE:expected['access'] &= ~0x1040
                    if after[c]["methods"].get(identity)!=expected or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError("Original closed constructor method metadata changed: "+identity)
            runtime=(rebuilt,harness,dependency) if args.dependency_inputs or args.terminal_owner else (rebuilt,harness)
            if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Closed constructor binding changed storage, initialization or failure identity")
            evidence.update({"all_original_entries_and_metadata":"matched with one explicit terminal interface projection" if args.terminal_owner else "unchanged",
                "allocation_initialization_wide_values_closed_callback_and_partial_final_fields":"matched"})
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence["original_inputs_and_immutable_callers"]="unchanged"
        (work/"evidence.json").open("x").write(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
