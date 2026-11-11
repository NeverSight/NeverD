#!/usr/bin/env python3
"""Verify private receiver calls before constructor delegation."""
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
    for name in ('neverd','android-jar'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('java-home','d8','work-dir'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--dependency-defaults',action='store_true')
    parser.add_argument('--scalar-result',action='store_true')
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix='neverd-constructor-private-getter-')
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/'constructor_private_getter/java'
        harness_sources=[FIXTURES/'harness'/(n+'.java') for n in ('ConstructorPrivateGetterCaller','ConstructorPrivateGetterHarness')]
        if args.scalar_result:
            owned=work/'scalar-source';shutil.copytree(fixture,owned);fixture=owned
            path=fixture/'fixture/Core.java';path.write_text(re.sub(r'\bboolean\b','int',path.read_text()))
            changed=[]
            for path in harness_sources:
                text=path.read_text()
                text=text.replace('boolean explicit=mode!=9;','int explicit=mode==9?Integer.MIN_VALUE:23+attempt;')
                for before,after in (('boolean explicit','int explicit'),('boolean expectedValue','int expectedValue'),
                        ('boolean.class','int.class'),('new Core(false,null,null)','new Core(Integer.MIN_VALUE+1,null,null)'),
                        ('new Core(true,null,null)','new Core(Integer.MAX_VALUE-7,null,null)'),('!Trace.partial.value','Trace.partial.value==0')):
                    text=text.replace(before,after)
                target=work/'scalar-observer'/path.name;target.parent.mkdir(parents=True,exist_ok=True);target.open('x').write(text);changed.append(target)
            harness_sources=changed
        originals,harness=work/'originals',work/'harness'
        verify.compile_local(sorted(fixture.rglob('*.java')),originals,release=25)
        verify.compile_local(harness_sources,harness,classpath=originals,release=25)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=3 or len(all_methods)!=7:raise RuntimeError('Original private getter inventory changed')
        providers={'Lfixture/Defaults;'}
        classes=all_classes-(providers if args.dependency_defaults else set())
        methods={m for m in all_methods if m.split('->',1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        encoded=work/'encoded'
        for path in sorted(originals.rglob('*.class')):
            data=path.read_bytes()
            if data[:4]!=b'\xca\xfe\xba\xbe' or int.from_bytes(data[6:8],'big')!=69:raise RuntimeError('Original Java 25 version changed')
            lowered=data[:6]+(52).to_bytes(2,'big')+data[8:]
            destination=encoded/path.relative_to(originals);destination.parent.mkdir(parents=True,exist_ok=True);destination.open('xb').write(lowered)
            if lowered[:6]!=data[:6] or lowered[8:]!=data[8:]:raise RuntimeError('Version encoding changed Code or attributes')
        encoded_hashes=hashes(encoded);encoded_records=class_identity.compiler_classes(encoded)
        if (set(encoded_records)!=set(records) or exception_inventory(encoded)!=exceptions or any(
                {k:v for k,v in r.items() if k not in {'major','sha256'}}!={k:v for k,v in encoded_records[o].items() if k not in {'major','sha256'}}
                for o,r in records.items())):raise RuntimeError('Version-only input encoding changed metadata')
        dependency=work/'owned-inputs.jar'
        with zipfile.ZipFile(dependency,'x') as jar:
            for owner in sorted(providers):
                name=owner[1:-1]+'.class';jar.writestr(name,(encoded/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=['--classpath',args.android_jar.resolve()]
        if args.dependency_defaults:classpath+=['--classpath',dependency]
        def command(runtime,route,mode):return [verify.java,'-cp',os.pathsep.join(map(str,runtime)),'ConstructorPrivateGetterHarness',str(route),str(mode)]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(12):
                    output=verify.run(command(runtime,route,mode),f'{label}-{route}-{mode}')
                    pattern=f'route={route},mode={mode},attempt=[01],trace=[0-9]+'
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError('Frozen private getter observer output changed: '+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),'baseline')
        if execute((encoded,harness),'encoded')!=baseline:raise RuntimeError('Version-only input encoding changed runtime behavior')
        dex=work/'dex';dex.mkdir()
        paths=[p for p in sorted(encoded.rglob('*.class')) if 'L'+p.relative_to(encoded).as_posix()[:-6]+';' in classes]
        verify.run([*verify.d8,'--debug','--min-api','24',*classpath,'--output',dex,*paths],'d8')
        source_dex=dex/'classes.dex'
        if list(dex.glob('classes*.dex'))!=[source_dex]:raise RuntimeError('Owned private getter DEX inventory changed')
        dump=verify.run([verify.dexdump,'-f','-h','-l','plain',source_dex],'sdk-inventory')
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r['descriptor'] for r in inventory['classes']}!=classes or {r['identity'] for r in inventory['methods']}!=methods:raise RuntimeError('D8 changed an original private getter constructor entry')
        (work/'sdk-inventory.json').open('x').write(json.dumps(inventory,indent=2)+'\n')
        disassembly=verify.run([verify.dexdump,'-d',source_dex],'original-private-getter-disassembly');witnessed=False
        scalar='I' if args.scalar_result else 'Z'
        signature='('+scalar+'ILfixture/Core;Ljava/lang/Object;)V'
        target_signature='('+scalar+'Lfixture/Core;Ljava/lang/Object;)V'
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            if block[1]!='Lfixture/Core;' or not re.search(r"(?m)^\s+type\s+: '"+re.escape(signature)+"'",block[2]):continue
            ops=re.findall(r'\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)',block[2])
            getters=[i for i,(op,ref) in enumerate(ops) if op.startswith('invoke-direct') and 'Lfixture/Core;.read:()'+scalar in ref]
            delegate=next(i for i,(op,ref) in enumerate(ops) if op.startswith('invoke-direct') and 'Lfixture/Core;.<init>:'+target_signature in ref)
            if (len(getters)!=1 or getters[0]>=delegate or ops[getters[0]+1][0]!='move-result'
                    or not any(op.startswith('if-') for op,ref in ops[:getters[0]])):
                raise RuntimeError('Original guarded private getter/delegation witness changed')
            witnessed=True
        if not witnessed:raise RuntimeError('Missing original private getter constructor witness')
        output=work/'recovered'
        verify.run([args.neverd.resolve(),'mobile',source_dex,*classpath,'-o',output],'recover')
        coverage=json.loads((output/'report.json').read_text())['android_method_recovery']
        if ({r['identity'] for r in coverage['methods']}!=methods or coverage['class_count']!=len(classes) or coverage['method_count']!=len(methods)
                or coverage['unrecovered_method_count'] or coverage.get('projected_method_count',0) or coverage['status']!='recovered'
                or coverage.get('class_source_bindings') or coverage.get('field_source_bindings') or coverage.get('generic_signature_projections')
                or coverage.get('minimum_java_release',8)!=25):raise RuntimeError('Private getter recovery changed original entries or contracts')
        rebuilt=work/'rebuilt'
        verify.compile_local(sorted((output/'sources').rglob('*.java')),rebuilt,classpath=dependency if args.dependency_defaults else None,release=25)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r['class']+'->'+r['name']+r['prototype'] for r in coverage.get('generated_source_helpers',[])}
        expected_helpers={c+'->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;' for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:raise RuntimeError('Private getter recovery invented or omitted a runtime entry')
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ('access','signature','superclass','interfaces','fields','inner_class','enclosing_method','deprecated_attribute','runtime_visible_annotations','runtime_invisible_annotations'):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError('Original private getter class metadata changed: '+c+':'+key)
            for identity,metadata in records[c]['methods'].items():
                if after[c]['methods'].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError('Original private getter method metadata/Exceptions changed: '+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_defaults else (rebuilt,harness)
        if execute(runtime,'rebuilt')!=baseline:raise RuntimeError('Private receiver, getter, delegation or exact failure changed')
        source=(fixture/'fixture/Core.java').read_text()
        mutations=(('ignored-getter','chosen=selected.read();','chosen=explicit;',0),
                   ('substituted-result','chosen=selected.read();','selected.read(); chosen=Trace.replacement.value;',0),
                   ('reloaded-receiver','this(chosen,selected,token);','this(chosen,Defaults.peer,token);',0),
                   ('early-final','Trace.partial=this; Trace.hit(5);\n        this.value=value;','Trace.partial=this; this.value=value; Trace.hit(5);',0),
                   ('wrong-mask','(mask&1)!=0','mask!=0',10),('missing-mutation','Defaults.peer=Trace.replacement;','',0),
                   ('wrong-argument','this(chosen,selected,token);','this(explicit,selected,token);',0))
        for label,before,replacement,mode in mutations:
            if source.count(before)!=1:raise RuntimeError('Original private getter wrong-program anchor changed')
            changed=work/label/'fixture/Core.java';changed.parent.mkdir(parents=True);changed.open('x').write(source.replace(before,replacement))
            wrong=work/(label+'-classes');verify.compile_local([changed],wrong,classpath=originals,release=25)
            result=subprocess.run(command((wrong,originals,harness),1,mode),text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+'-counterexample.log')).open('x').write(result.stdout+result.stderr)
            if result.returncode==0 or 'owned constructor private getter' not in result.stderr:raise RuntimeError('Frozen observer missed incorrect constructor private call: '+label)
        if (hashes(originals)!=original_hashes or hashes(encoded)!=encoded_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError('Original inputs, SDK or frozen observers changed')
        evidence={'status':'success','class_count':len(classes),'method_count':len(methods),'fresh_jvm_cases':len(baseline),'attempts_per_case':2,
                  'scalar_result':args.scalar_result,'dependency_defaults':args.dependency_defaults,'all_java':'compiled','original_metadata_and_exceptions':'unchanged',
                  'private_receiver_result_and_delegate_identity':'matched','partial_final_stores_getter_static_mutation_and_exact_failures':'matched',
                  'd8_version_only_encoding':'all Code and attribute bytes unchanged; both baselines matched','independent_wrong_programs':len(mutations),'input_and_oracle_bytes':'unchanged'}
        (work/'evidence.json').open('x').write(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=='__main__':raise SystemExit(main())
