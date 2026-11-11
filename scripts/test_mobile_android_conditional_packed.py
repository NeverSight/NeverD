#!/usr/bin/env python3
"""Verify relational branches and long bit operations inside allocation arguments."""
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
    if error.name!='scripts':raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('neverd','android-jar'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('java-home','d8','work-dir'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--dependency-inputs',action='store_true')
    parser.add_argument('--ordered-inputs',action='store_true')
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix='neverd-conditional-packed-')
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/'conditional_packed/java'
        if args.ordered_inputs:
            owned=work/'ordered-source';shutil.copytree(fixture,owned);fixture=owned
            path=fixture/'fixture/Creator.java';text=path.read_text()
            if text.count('left==right')!=1:raise RuntimeError('Original comparison anchor changed')
            path.write_text(text.replace('left==right','low>=high'))
        originals,harness=work/'originals',work/'harness'
        verify.compile_local(sorted(fixture.rglob('*.java')),originals,release=8)
        verify.compile_local([FIXTURES/'harness'/(n+'.java') for n in ('ConditionalPackedCaller','ConditionalPackedHarness')],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if (len(all_classes),len(all_methods))!=(4,10):raise RuntimeError('Original packed allocation inventory changed')
        providers={'Lfixture/Provider;','Lfixture/Target;'} if args.dependency_inputs else set()
        classes=all_classes-providers;methods={m for m in all_methods if m.split('->',1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/'owned-inputs.jar'
        with zipfile.ZipFile(dependency,'x') as jar:
            for owner in sorted(providers):
                name=owner[1:-1]+'.class';jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest();classpath=['--classpath',args.android_jar.resolve()]
        if providers:classpath+=['--classpath',dependency]
        def command(runtime,route,mode):return [verify.java,'-cp',os.pathsep.join(map(str,runtime)),'ConditionalPackedHarness',str(route),str(mode),str(args.ordered_inputs).lower()]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(16):
                    output=verify.run(command(runtime,route,mode),f'{label}-{route}-{mode}')
                    pattern=f'route={route},mode={mode},attempt=[01],trace=[0-9]+'
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError('Frozen packed observer changed: '+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),'baseline')
        dex=work/'dex';dex.mkdir()
        paths=[p for p in sorted(originals.rglob('*.class')) if 'L'+p.relative_to(originals).as_posix()[:-6]+';' in classes]
        verify.run([*verify.d8,'--debug','--min-api','24',*classpath,'--output',dex,*paths],'d8');source_dex=dex/'classes.dex'
        if list(dex.glob('classes*.dex'))!=[source_dex]:raise RuntimeError('Original packed DEX inventory changed')
        dump=verify.run([verify.dexdump,'-f','-h','-l','plain',source_dex],'sdk-inventory')
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r['descriptor'] for r in inventory['classes']}!=classes or {r['identity'] for r in inventory['methods']}!=methods:raise RuntimeError('D8 changed an original packed entry')
        (work/'sdk-inventory.json').open('x').write(json.dumps(inventory,indent=2)+'\n')
        disassembly=verify.run([verify.dexdump,'-d',source_dex],'original-packed-disassembly');witnessed=False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            if block[1]!='Lfixture/Creator;' or not re.search(r"(?m)^\s+name\s+: 'make'",block[2]):continue
            ops=re.findall(r'\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)',block[2]);kinds=[op.split('/')[0] for op,_ in ops]
            new=next(i for i,(op,ref) in enumerate(ops) if op=='new-instance' and 'Lfixture/Target;' in ref)
            read=next(i for i,(op,ref) in enumerate(ops) if op.startswith('invoke-static') and 'Lfixture/Provider;.read:' in ref)
            branch=next(i for i,op in enumerate(kinds) if op.startswith('if-'))
            init=next(i for i,(op,ref) in enumerate(ops) if op.startswith('invoke-direct') and 'Lfixture/Target;.<init>:' in ref)
            if (not new<read<branch<init or sum(op.startswith('if-') for op in kinds)!=1 or kinds[branch]!=('if-lt' if args.ordered_inputs else 'if-ne')
                    or not {'shl-long','and-long','or-long','xor-long'}.issubset(kinds)):raise RuntimeError('Original conditional/long allocation witness changed')
            witnessed=True
        if not witnessed:raise RuntimeError('Missing original conditional packed witness')
        output=work/'recovered';verify.run([args.neverd.resolve(),'mobile',source_dex,*classpath,'-o',output],'recover')
        coverage=json.loads((output/'report.json').read_text())['android_method_recovery']
        if ({r['identity'] for r in coverage['methods']}!=methods or coverage['class_count']!=len(classes) or coverage['method_count']!=len(methods)
                or coverage['unrecovered_method_count'] or coverage.get('projected_method_count',0) or coverage['status']!='recovered'
                or coverage.get('class_source_bindings') or coverage.get('field_source_bindings') or coverage.get('generic_signature_projections')
                or coverage.get('minimum_java_release',8)!=14):raise RuntimeError('Packed recovery changed original entries/contracts')
        rebuilt=work/'rebuilt';verify.compile_local(sorted((output/'sources').rglob('*.java')),rebuilt,classpath=dependency if providers else None,release=14)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r['class']+'->'+r['name']+r['prototype'] for r in coverage.get('generated_source_helpers',[])}
        expected_helpers={c+'->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;' for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:raise RuntimeError('Packed recovery invented or omitted an entry')
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ('access','signature','superclass','interfaces','fields','inner_class','enclosing_method','deprecated_attribute','runtime_visible_annotations','runtime_invisible_annotations'):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError('Original packed class metadata changed: '+c+':'+key)
            for identity,metadata in records[c]['methods'].items():
                if after[c]['methods'].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError('Original packed method metadata/Exceptions changed: '+identity)
        runtime=(rebuilt,harness,dependency) if providers else (rebuilt,harness)
        if execute(runtime,'rebuilt')!=baseline:raise RuntimeError('Packed allocation changed values, effects or exact failures')
        source=(fixture/'fixture/Creator.java').read_text()
        condition='low>=high' if args.ordered_inputs else 'left==right';inverted='low<high' if args.ordered_inputs else 'left!=right'
        mutations=(('inverted-condition',condition,inverted,1),('wrong-shift','<<shift','>>>shift',2),('wrong-mask','&mask','&0L',2),
                   ('wrong-float','Float.floatToRawIntBits(first)','Float.floatToRawIntBits(second)',1),
                   ('ignored-seed','Provider.read(left)','left',1),('missing-after','Provider.after()','0',1))
        for label,before,replacement,count in mutations:
            if source.count(before)<count:raise RuntimeError('Original packed wrong-program anchor changed')
            path=work/label/'fixture/Creator.java';path.parent.mkdir(parents=True);path.open('x').write(source.replace(before,replacement,count))
            wrong=work/(label+'-classes');verify.compile_local([path],wrong,classpath=originals,release=8)
            result=subprocess.run(command((wrong,originals,harness),1,0),text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+'-counterexample.log')).open('x').write(result.stdout+result.stderr)
            if result.returncode==0 or 'owned conditional packed arguments' not in result.stderr:raise RuntimeError('Frozen observer missed incorrect packed program: '+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError('Original inputs/SDK/frozen observers changed')
        evidence={'status':'success','class_count':len(classes),'method_count':len(methods),'fresh_jvm_cases':len(baseline),'attempts_per_case':2,
                  'ordered_inputs':args.ordered_inputs,'dependency_inputs':args.dependency_inputs,'all_java':'compiled','original_metadata_and_exceptions':'unchanged',
                  'reference_identity_integer_relations_and_raw_long_bits':'matched','effect_order_and_exact_failures':'matched','independent_wrong_programs':len(mutations),'input_and_oracle_bytes':'unchanged'}
        (work/'evidence.json').open('x').write(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=='__main__':raise SystemExit(main())
