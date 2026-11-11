#!/usr/bin/env python3
"""Verify filled int-array allocation identity across constructor delegation."""
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
    parser.add_argument('--dependency-inputs',action='store_true')
    parser.add_argument('--shared-array',action='store_true')
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix='neverd-filled-int-constructor-')
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/'filled_int_constructor/java'
        if args.shared_array:
            target=work/'shared-source';shutil.copytree(fixture,target);fixture=target
            path=fixture/'fixture/Core.java';text=path.read_text()
            if text.count('int[] right=new int[]{second,first};')!=1:raise RuntimeError('Original array alias anchor changed')
            path.write_text(text.replace('int[] right=new int[]{second,first};','int[] right=left;'))
        originals,harness=work/'originals',work/'harness'
        verify.compile_local(sorted(fixture.rglob('*.java')),originals,release=25)
        verify.compile_local([FIXTURES/'harness'/(n+'.java') for n in ('FilledIntConstructorCaller','FilledIntConstructorHarness')],harness,classpath=originals,release=25)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=3 or len(all_methods)!=7:raise RuntimeError('Original filled array inventory changed')
        providers={'Lfixture/Trace;','Lfixture/Hook;'}
        classes=all_classes-(providers if args.dependency_inputs else set())
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
        if args.dependency_inputs:classpath+=['--classpath',dependency]
        def command(runtime,route,mode):return [verify.java,'-cp',os.pathsep.join(map(str,runtime)),'FilledIntConstructorHarness',str(route),str(mode),str(args.shared_array).lower()]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(14):
                    output=verify.run(command(runtime,route,mode),f'{label}-{route}-{mode}')
                    pattern=f'route={route},mode={mode},attempt=[01],trace=[0-9]+'
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError('Frozen array observer output changed: '+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),'baseline')
        if execute((encoded,harness),'encoded')!=baseline:raise RuntimeError('Version-only input encoding changed runtime behavior')
        dex=work/'dex';dex.mkdir()
        paths=[p for p in sorted(encoded.rglob('*.class')) if 'L'+p.relative_to(encoded).as_posix()[:-6]+';' in classes]
        verify.run([*verify.d8,'--debug','--min-api','24',*classpath,'--output',dex,*paths],'d8')
        source_dex=dex/'classes.dex'
        if list(dex.glob('classes*.dex'))!=[source_dex]:raise RuntimeError('Owned filled array DEX inventory changed')
        dump=verify.run([verify.dexdump,'-f','-h','-l','plain',source_dex],'sdk-inventory')
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r['descriptor'] for r in inventory['classes']}!=classes or {r['identity'] for r in inventory['methods']}!=methods:raise RuntimeError('D8 changed an original array constructor entry')
        (work/'sdk-inventory.json').open('x').write(json.dumps(inventory,indent=2)+'\n')
        disassembly=verify.run([verify.dexdump,'-d',source_dex],'original-array-disassembly')
        witnessed=False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            if block[1]!='Lfixture/Core;' or not re.search(r"(?m)^\s+type\s+: '\(IILfixture/Hook;\)V'",block[2]):continue
            ops=re.findall(r'\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)',block[2])
            arrays=[i for i,(op,ref) in enumerate(ops) if op.startswith('filled-new-array') and '[I' in ref]
            delegate=next(i for i,(op,ref) in enumerate(ops) if op.startswith('invoke-direct') and 'Lfixture/Core;.<init>:([I[ILfixture/Hook;)V' in ref)
            if len(arrays)!=(1 if args.shared_array else 2) or any(i>=delegate or ops[i+1][0]!='move-result-object' for i in arrays):raise RuntimeError('Original filled-array/result/delegation witness changed')
            witnessed=True
        if not witnessed:raise RuntimeError('Missing original filled-array constructor witness')
        output=work/'recovered'
        verify.run([args.neverd.resolve(),'mobile',source_dex,*classpath,'-o',output],'recover')
        coverage=json.loads((output/'report.json').read_text())['android_method_recovery']
        if ({r['identity'] for r in coverage['methods']}!=methods or coverage['class_count']!=len(classes) or coverage['method_count']!=len(methods)
                or coverage['unrecovered_method_count'] or coverage.get('projected_method_count',0) or coverage['status']!='recovered'
                or coverage.get('class_source_bindings') or coverage.get('field_source_bindings') or coverage.get('generic_signature_projections')
                or coverage.get('minimum_java_release',8)!=25):raise RuntimeError('Array recovery changed original entries or contracts')
        rebuilt=work/'rebuilt'
        verify.compile_local(sorted((output/'sources').rglob('*.java')),rebuilt,classpath=dependency if args.dependency_inputs else None,release=25)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r['class']+'->'+r['name']+r['prototype'] for r in coverage.get('generated_source_helpers',[])}
        expected_helpers={c+'->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;' for c in classes if c!='Lfixture/Hook;'}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:raise RuntimeError('Array recovery invented or omitted a runtime entry')
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ('access','signature','superclass','interfaces','fields','inner_class','enclosing_method','deprecated_attribute','runtime_visible_annotations','runtime_invisible_annotations'):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError('Original array class metadata changed: '+c+':'+key)
            for identity,metadata in records[c]['methods'].items():
                if after[c]['methods'].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError('Original array method metadata/Exceptions changed: '+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_inputs else (rebuilt,harness)
        if execute(runtime,'rebuilt')!=baseline:raise RuntimeError('Array allocation, alias, mutation or exact failure changed')
        source=(fixture/'fixture/Core.java').read_text()
        array_line='int[] right=left;' if args.shared_array else 'int[] right=new int[]{second,first};'
        opposite='int[] right=new int[]{value,second};' if args.shared_array else 'int[] right=left;'
        mutations=(('wrong-alias',array_line,opposite),('wrong-element','int[] left=new int[]{value,second};','int[] left=new int[]{value+1,second};'),
                   ('wrong-order','int[] left=new int[]{value,second};','int[] left=new int[]{second,value};'),
                   ('lost-postlude','left[1]=Trace.post;','left[1]=Trace.post+1;'),
                   ('ignored-seed','int value=Trace.seed(first);','Trace.seed(first); int value=first;'),
                   ('late-final','this.left=left; Trace.hit(3);','Trace.hit(3);'))
        for label,before,replacement in mutations:
            if source.count(before)!=1:raise RuntimeError('Original array wrong-program anchor changed')
            wrong_source=source.replace(before,replacement)
            if label=='late-final':wrong_source=wrong_source.replace('hook.apply(this,right);','hook.apply(this,right); this.left=left;')
            changed=work/label/'fixture/Core.java';changed.parent.mkdir(parents=True);changed.open('x').write(wrong_source)
            wrong=work/(label+'-classes');verify.compile_local([changed],wrong,classpath=originals,release=25)
            result=subprocess.run(command((wrong,originals,harness),1,0),text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+'-counterexample.log')).open('x').write(result.stdout+result.stderr)
            if result.returncode==0 or 'owned filled int constructor' not in result.stderr:raise RuntimeError('Frozen observer missed incorrect array program: '+label)
        if (hashes(originals)!=original_hashes or hashes(encoded)!=encoded_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError('Original inputs, SDK or frozen observers changed')
        evidence={'status':'success','class_count':len(classes),'method_count':len(methods),'fresh_jvm_cases':len(baseline),'attempts_per_case':2,
                  'shared_array':args.shared_array,'dependency_inputs':args.dependency_inputs,'all_java':'compiled','original_metadata_and_exceptions':'unchanged',
                  'array_allocation_aliases_and_retained_postlude':'matched','partial_final_stores_and_exact_failures':'matched',
                  'd8_version_only_encoding':'all Code and attribute bytes unchanged; both baselines matched','independent_wrong_programs':len(mutations),'input_and_oracle_bytes':'unchanged'}
        (work/'evidence.json').open('x').write(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=='__main__':raise SystemExit(main())
