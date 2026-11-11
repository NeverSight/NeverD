#!/usr/bin/env python3
"""Verify D8 allocation handler gaps and copy adapters against frozen JVM observers."""
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
    parser.add_argument("--receiver-copy",action="store_true",help="Use an independently authored smali receiver-alias program")
    parser.add_argument("--entry-frame",action="store_true",help="Overwrite an incoming word after saving its constructor alias")
    parser.add_argument("--retry-loop",action="store_true",help="Re-enter the complete allocation after caught construction failures")
    parser.add_argument("--long-cleanup",action="store_true",help="Keep a long catch continuation and its protected finally path")
    parser.add_argument("--constant-adapters",action="store_true",help="Retain matching constants before the shared cleanup")
    parser.add_argument("--wide-constants",action="store_true")
    parser.add_argument("--adapter-constant",type=lambda x:int(x,0),default=0)
    args=parser.parse_args()
    if args.constant_adapters:args.receiver_copy=True
    if args.wide_constants and not args.constant_adapters:parser.error("--wide-constants requires --constant-adapters")
    width=64 if args.wide_constants else 32
    if not -(1<<(width-1))<=args.adapter_constant<(1<<(width-1)):parser.error("adapter constant exceeds its word width")
    if args.retry_loop and (args.receiver_copy or args.entry_frame):
        parser.error("--retry-loop uses original javac/D8 code")
    if args.entry_frame:args.receiver_copy=True
    if args.long_cleanup and (args.receiver_copy or args.retry_loop):
        parser.error("--long-cleanup uses an independent javac/D8 continuation")
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-handler-gap-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"allocation_handler_gap/java"
        cleanup_steps=160
        if args.long_cleanup:
            profile=work/"long-cleanup-source"
            for original in sorted(fixture.rglob("*.java")):
                text=original.read_text()
                if original.name=="Client.java":
                    anchor="Trace.hit(3);return null;"
                    if text.count(anchor)!=1:raise RuntimeError("Original long cleanup anchor changed")
                    steps="".join(f"Trace.cleanup(value,b,{i});" for i in range(cleanup_steps))
                    text=text.replace(anchor,"Trace.hit(3);try{"+steps+"}finally{Trace.finished++;}return null;")
                elif original.name=="Trace.java":
                    anchor="    public static long events;"
                    if text.count(anchor)!=1:raise RuntimeError("Original cleanup state anchor changed")
                    text=text.replace(anchor,anchor+"\n"
                        "    public static int cleanupCount,cleanupFailAt,finished;\n"
                        "    public static long cleanupTrace;\n"
                        "    public static Object cleanupFirst,cleanupSecond;\n"
                        "    public static RuntimeException cleanupFailure;\n"
                        "    public static Error cleanupError;\n"
                        "    public static void cleanup(Object first,Object second,int index){\n"
                        "        if(first!=cleanupFirst || second!=cleanupSecond)throw new AssertionError(\"owned cleanup identity\");\n"
                        "        cleanupCount++;cleanupTrace=cleanupTrace*31+index;\n"
                        "        if(index==cleanupFailAt){if(cleanupError!=null)throw cleanupError;throw cleanupFailure;}\n"
                        "    }")
                target=profile/original.relative_to(fixture);target.parent.mkdir(parents=True,exist_ok=True);target.open("x").write(text)
            fixture=profile
        constant_type='long' if args.wide_constants else 'int'
        constant_literal=str(args.adapter_constant)+('L' if args.wide_constants else '')
        constant_assignment='Trace.constant='+constant_literal+';'
        if args.constant_adapters:
            profile=work/'constant-source'
            for original in sorted(fixture.rglob('*.java')):
                text=original.read_text()
                if original.name=='Client.java':text=text.replace('Trace.caught=failure;',constant_assignment+'Trace.caught=failure;')
                elif original.name=='Trace.java':text=text.replace('public static long events;','public static '+constant_type+' constant;\n    public static long events;')
                target=profile/original.relative_to(fixture);target.parent.mkdir(parents=True,exist_ok=True);target.open('x').write(text)
            fixture=profile
        if args.retry_loop:
            profile=work/"retry-source"
            for original in sorted(fixture.rglob("*.java")):
                text=original.read_text()
                if original.name=="Client.java":
                    text=text.replace("try{return new Node", "for(int round=0;round<3;round++){try{return new Node")
                    text=text.replace("Trace.caught=failure;", "Trace.caught=failure;Trace.failedPartial=Trace.partial;")
                    text=text.replace("Trace.hit(3);return null;}", "Trace.hit(3);}}return null;")
                elif original.name=="Trace.java":
                    text=text.replace("public static int failAt;", "public static int failAt,remaining;\n    public static RuntimeException cleanupFailure;\n    public static Object failedPartial;")
                    text=text.replace("if(failAt==n)throw failure;", "if(n==3 && cleanupFailure!=null)throw cleanupFailure;if(failAt==n && remaining!=0){if(remaining>0)remaining--;throw failure;}")
                target=profile/original.relative_to(fixture);target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text)
            fixture=profile
        harness_name="LongAllocationCleanup" if args.long_cleanup else "AllocationHandlerRetry" if args.retry_loop else "AllocationHandlerGap"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        harness_sources=[FIXTURES/"harness"/(n+".java") for n in ("AllocationHandlerGapCaller",harness_name+"Harness")]
        if args.constant_adapters:
            text=harness_sources[1].read_text();anchor='Trace.partial=null;'
            sentinel='81985529216486895L' if args.wide_constants else '123456789'
            if args.adapter_constant==int(sentinel.rstrip('L')):raise RuntimeError('Constant adapter observer sentinel equals the input literal')
            if text.count(anchor)!=1:raise RuntimeError('Original constant adapter reset anchor changed')
            text=text.replace(anchor,'Trace.constant='+sentinel+';'+anchor)
            anchor='            Node partial=(Node)Trace.partial;'
            if text.count(anchor)!=1:raise RuntimeError('Original constant adapter observer anchor changed')
            text=text.replace(anchor,'            require(Trace.class.getField("constant").getType()=='+constant_type+'.class);\n'
                '            require(Trace.constant==(mode==2||mode==3?'+constant_literal+':'+sentinel+'));\n'+anchor)
            changed=work/'constant-harness'/harness_sources[1].name;changed.parent.mkdir();changed.open('x').write(text);harness_sources[1]=changed
        verify.compile_local(harness_sources,harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=3 or len(all_methods)!=(6 if args.long_cleanup else 5):raise RuntimeError("Original allocation handler gap inventory changed")
        declarations={"Lfixture/Node;"}
        if args.receiver_copy:declarations.add("Lfixture/Trace;")
        classes=all_classes-(declarations if args.dependency_inputs or args.receiver_copy else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:
            for name in (("Node","Trace") if args.receiver_copy else ("Node",)):
                jar.writestr("fixture/"+name+".class",(originals/("fixture/"+name+".class")).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_inputs or args.receiver_copy:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(12 if args.long_cleanup else 9 if args.retry_loop else 6):
                    output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),harness_name+"Harness",
                        str(route),str(mode)],f"{label}-{route}-{mode}")
                    pattern=f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError("Original allocation handler gap matrix changed: "+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),"baseline")
        dex=work/"dex";dex.mkdir()
        paths=[p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",dex,*paths],"d8")
        source_dex=dex/"classes.dex"
        if list(dex.glob("classes*.dex"))!=[source_dex]:raise RuntimeError("Original allocation handler gap DEX inventory changed")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed an original allocation handler gap entry")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        code=verify.run([verify.dexdump,"-d",source_dex],"sdk-code")
        start=code.index("name          : 'make'")
        stop=code.index("  Virtual methods",start)
        body=code[start:stop]
        ranges=re.findall(r"0x([0-9a-f]+) - 0x([0-9a-f]+)\s+Ljava/lang/IllegalArgumentException; -> 0x([0-9a-f]+)",body)
        instructions={int(pc,16):name for pc,name in re.findall(r"\|([0-9a-f]+): ([a-z][a-z0-9/-]*)",body)}
        throwing=[pc for pc,name in instructions.items() if name in ("new-instance","invoke-direct/range")]
        gaps=[pc for pc,name in instructions.items() if name.startswith("move-object") and len(throwing)==2 and throwing[0]<pc<throwing[1]]
        if args.long_cleanup:
            if (len(ranges)!=1 or len(throwing)!=2 or len(gaps)!=7
                    or int(ranges[0][0],16)!=throwing[0]
                    or not all(int(ranges[0][0],16)<=pc<int(ranges[0][1],16) for pc in throwing+gaps)):
                raise RuntimeError("Independent D8 long cleanup allocation/receiver-copy witness changed")
        elif (not args.receiver_copy and (len(ranges)!=2 or len(throwing)!=2 or len(gaps)!=6 or ranges[0][2]==ranges[1][2]
                or any(int(lo,16)<=pc<int(hi,16) for pc in gaps for lo,hi,_ in ranges))):
            raise RuntimeError("Independent D8 no longer supplies the split allocation/copy/handler witness: "+
                json.dumps({"ranges":ranges,"throwing":throwing,"gaps":gaps}))
        (work/"allocation-handler-witness.json").write_text(json.dumps({"protected_ranges":ranges,
            "throwing_allocation_pcs":throwing,
            "protected_copy_pcs" if args.long_cleanup else "unprotected_copy_pcs":gaps,
            "input_method_bytecode":"independently authored receiver-alias smali" if args.receiver_copy else "unchanged compiler output"},indent=2)+"\n")
        if args.long_cleanup:
            cleanup_calls=re.findall(r"\|[0-9a-f]+: invoke-static(?:/range)? .*?Lfixture/Trace;\.cleanup:",body)
            if len(cleanup_calls)!=cleanup_steps or len(instructions)<=256:
                raise RuntimeError("Independent D8 output lacks the complete long cleanup")
            (work/"long-cleanup-witness.json").write_text(json.dumps({"cleanup_calls":len(cleanup_calls),
                "method_instructions":len(instructions),"original_code":"unchanged javac/D8 output"},indent=2)+"\n")
        if args.retry_loop:
            backedges=[(int(pc,16),int(target,16)) for pc,target in re.findall(
                r"\|([0-9a-f]+): goto(?:/\d+)? ([0-9a-f]+) ",body) if int(target,16)<int(pc,16)]
            if not backedges or not any(target<=throwing[0]<pc for pc,target in backedges):
                raise RuntimeError("Original D8 retry does not re-enter the complete allocation")
            (work/"retry-loop-witness.json").write_text(json.dumps({"backedges":backedges,
                "original_code":"unchanged javac/D8 output","protected_ranges":ranges},indent=2)+"\n")
        output=work/"recovered"
        source_input=FIXTURES/"allocation_handler_gap/smali" if args.receiver_copy else source_dex
        if args.entry_frame or args.constant_adapters:
            source_input=work/"entry-frame-smali";source_input.mkdir()
            text=(FIXTURES/"allocation_handler_gap/smali/Client.smali").read_text()
            if args.entry_frame:
                before="    move-object v8, v0"
                if text.count(before)!=1:raise RuntimeError("Owned entry frame alias anchor changed")
                text=text.replace(before,"    move-object/from16 p0, p1\n"+before)
            if args.constant_adapters:
                constant='    '+('const-wide' if args.wide_constants else 'const')+' v7, '+str(args.adapter_constant)+'\n'
                root='    move-exception v1\n    goto :cleanup'
                common='    :cleanup\n'
                if text.count(root)!=1 or text.count(common)!=1:raise RuntimeError('Original constant adapter handler anchors changed')
                text=text.replace(root,'    move-exception v1\n'+constant+'    goto :cleanup')
                text=text.replace(common,constant+common+'    '+('sput-wide' if args.wide_constants else 'sput')+' v7, Lfixture/Trace;->constant:'+('J' if args.wide_constants else 'I')+'\n')
                (work/'constant-adapter-witness.json').open('x').write(json.dumps({'width':width,'literal':args.adapter_constant,
                    'independently_authored_adapters':2,'shared_cleanup_store':1,'original_argument_restoration':'unchanged',
                    'input_smali_sha256':hashlib.sha256(text.encode()).hexdigest()},indent=2)+'\n')
            (source_input/"Client.smali").open('x').write(text)
        verify.run([args.neverd.resolve(),"mobile",source_input,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count",0) or coverage["status"]!="recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Allocation handler recovery changed original entries or metadata contracts")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,
            classpath=dependency if args.dependency_inputs or args.receiver_copy else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes if c not in set() }
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Allocation handler recovery invented or omitted a runtime entry")
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError("Original allocation handler gap class metadata changed: "+c+":"+key)
            for identity,metadata in records[c]["methods"].items():
                if after[c]["methods"].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:
                    raise RuntimeError("Original allocation handler gap method metadata or Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs or args.receiver_copy else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Allocation handler recovery changed ordered effects or input identities")
        mutations=(("lost-result","Client","try{return new Node(value,b,c,d,e,f);}","try{new Node(value,b,c,d,e,f);return null;}",0),
                   ("wrong-input","Client","new Node(value,b,c,d,e,f)","new Node(null,b,c,d,e,f)",0),
                   ("lost-catch","Client","Trace.caught=failure;","Trace.caught=null;",2),
                   ("late-field","Node","this.value=value;this.arguments=new Object[]{value,b,c,d,e,f};Trace.hit(2);","Trace.hit(2);this.value=value;this.arguments=new Object[]{value,b,c,d,e,f};",3))
        if args.retry_loop:
            mutations += (("lost-retry","Client","round<3","round<1",2),
                          ("lost-cleanup","Client","Trace.hit(3);","",7))
        if args.long_cleanup:
            mutations+=(("lost-last-step","Client","Trace.cleanup(value,b,159);","",2),
                        ("reordered-last-steps","Client","Trace.cleanup(value,b,158);Trace.cleanup(value,b,159);",
                         "Trace.cleanup(value,b,159);Trace.cleanup(value,b,158);",2),
                        ("late-wrong-input","Client","Trace.cleanup(value,b,159);","Trace.cleanup(b,value,159);",2),
                        ("lost-finally","Client","Trace.finished++;","",8),
                        ("swallowed-late-failure","Client","Trace.cleanup(value,b,159);",
                         "try{Trace.cleanup(value,b,159);}catch(RuntimeException ignored){}",8))
        if args.constant_adapters:
            mutations+=(("wrong-constant","Client",constant_assignment,'Trace.constant='+str(args.adapter_constant^1)+('L' if args.wide_constants else '')+';',2),
                        ("early-constant","Client","try{return new Node","try{ "+constant_assignment+"return new Node",0))
        for label,name,before,after,mode in mutations:
            text=(fixture/("fixture/"+name+".java")).read_text()
            if text.count(before)!=1:raise RuntimeError("Original allocation handler gap counterexample anchor changed")
            changed=work/label/(name+".java");changed.parent.mkdir();changed.write_text(text.replace(before,after))
            wrong=work/(label+"-classes")
            verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong,originals,harness))),
                harness_name+"Harness","1",str(mode)],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            marker="owned long allocation cleanup" if args.long_cleanup else "owned allocation handler retry" if args.retry_loop else "owned allocation handler gap"
            if result.returncode==0 or marker not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect allocation handler gap: "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError("Original inputs or frozen observers changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),
            "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_inputs":args.dependency_inputs,"receiver_copy":args.receiver_copy,"entry_frame":args.entry_frame,
            "retry_loop":args.retry_loop,
            "long_cleanup":args.long_cleanup,"cleanup_steps":cleanup_steps if args.long_cleanup else None,
            "constant_adapters":args.constant_adapters,"adapter_constant_width":width if args.constant_adapters else None,
            "adapter_constant":args.adapter_constant if args.constant_adapters else None,
            "exception_layout":"independent D8 long continuation" if args.long_cleanup else "authored smali receiver alias" if args.receiver_copy else "independent D8 split handlers",
            "allocation_copy_gaps_and_input_origin_adapters":"matched","all_original_entries_and_metadata":"unchanged",
            "constructor_argument_identity_partial_fields_catch_and_uncaught_failures":"matched",
            "independent_wrong_programs":len(mutations),"original_inputs_and_immutable_callers":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=="__main__":raise SystemExit(main())
