#!/usr/bin/env python3
"""Verify independent terminal throw blocks around original final-field stores."""
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
    parser.add_argument('--dependency-trace',action='store_true');parser.add_argument('--reverse-exits',action='store_true')
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix='neverd-multiple-final-exits-')
    with context as directory:
        work=Path(directory).resolve();verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/'multiple_final_exits/java'
        if args.reverse_exits:
            owned=work/'reversed-source';shutil.copytree(fixture,owned);fixture=owned
            for name in ('Core','StaticCore'):
                path=fixture/'fixture'/(name+'.java');text=path.read_text()
                second='        if(Trace.mode==2) { Trace.event(6); throw Trace.failure2; }\n'
                third='        if(Trace.mode==3) { Trace.event(7); throw Trace.failure3; }\n'
                if text.count(second+third)!=1:raise RuntimeError('Original terminal layout anchor changed')
                path.write_text(text.replace(second+third,third+second))
        originals,harness=work/'originals',work/'harness'
        verify.compile_local(sorted(fixture.rglob('*.java')),originals,release=8)
        verify.compile_local([FIXTURES/'harness'/(n+'.java') for n in ('MultipleFinalExitsCaller','MultipleFinalExitsHarness')],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness);sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if (len(all_classes),len(all_methods))!=(3,5):raise RuntimeError('Original multiple-exit inventory changed')
        providers={'Lfixture/Trace;'} if args.dependency_trace else set()
        classes=all_classes-providers;methods={m for m in all_methods if m.split('->',1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        dependency=work/'owned-trace.jar'
        with zipfile.ZipFile(dependency,'x') as jar:jar.writestr('fixture/Trace.class',(originals/'fixture/Trace.class').read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest();classpath=['--classpath',args.android_jar.resolve()]
        if providers:classpath+=['--classpath',dependency]
        def command(runtime,statics,route,mode):return [verify.java,'-cp',os.pathsep.join(map(str,runtime)),'MultipleFinalExitsHarness',str(statics).lower(),str(route),str(mode)]
        def execute(runtime,label):
            outputs=[]
            for statics in (False,True):
                for route in range(4):
                    for mode in range(8):
                        output=verify.run(command(runtime,statics,route,mode),f'{label}-{statics}-{route}-{mode}')
                        pattern=f'static={str(statics).lower()},route={route},mode={mode},attempt=[01],trace=[0-9]+'
                        if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):raise RuntimeError('Frozen multiple-exit observer changed: '+output)
                        outputs.append(output)
            return outputs
        baseline=execute((originals,harness),'baseline')
        dex=work/'dex';dex.mkdir()
        paths=[p for p in sorted(originals.rglob('*.class')) if 'L'+p.relative_to(originals).as_posix()[:-6]+';' in classes]
        verify.run([*verify.d8,'--debug','--min-api','24',*classpath,'--output',dex,*paths],'d8');source_dex=dex/'classes.dex'
        if list(dex.glob('classes*.dex'))!=[source_dex]:raise RuntimeError('Original multiple-exit DEX inventory changed')
        dump=verify.run([verify.dexdump,'-f','-h','-l','plain',source_dex],'sdk-inventory');inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r['descriptor'] for r in inventory['classes']}!=classes or {r['identity'] for r in inventory['methods']}!=methods:raise RuntimeError('D8 changed an original multiple-exit entry')
        (work/'sdk-inventory.json').open('x').write(json.dumps(inventory,indent=2)+'\n')
        disassembly=verify.run([verify.dexdump,'-d',source_dex],'original-terminal-disassembly');witnesses=set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            if block[1] not in {'Lfixture/Core;','Lfixture/StaticCore;'}:continue
            ops=re.findall(r'\|[0-9a-fA-F]+:\s+(\S+)',block[2])
            if 'throw' not in ops:continue
            returns=ops.index('return-void');tail=ops[returns+1:]
            store='iput-wide' if block[1]=='Lfixture/Core;' else 'sput-wide'
            if (ops.count('return-void')!=1 or ops.count(store)!=2 or ops.count('throw')!=3 or tail.count('throw')!=3
                    or tail[-1]!='throw' or any(op.startswith(('if-','goto','return')) or 'switch' in op for op in tail)):
                raise RuntimeError('Original independent terminal block witness changed')
            witnesses.add(block[1])
        if witnesses!={'Lfixture/Core;','Lfixture/StaticCore;'}:raise RuntimeError('Missing original constructor/clinit terminal witnesses')
        output=work/'recovered';verify.run([args.neverd.resolve(),'mobile',source_dex,*classpath,'-o',output],'recover')
        coverage=json.loads((output/'report.json').read_text())['android_method_recovery']
        if ({r['identity'] for r in coverage['methods']}!=methods or coverage['class_count']!=len(classes) or coverage['method_count']!=len(methods)
                or coverage['unrecovered_method_count'] or coverage.get('projected_method_count',0) or coverage['status']!='recovered'
                or coverage.get('class_source_bindings') or coverage.get('field_source_bindings') or coverage.get('generic_signature_projections')):
            raise RuntimeError('Multiple-exit recovery changed original entries/contracts')
        rebuilt=work/'rebuilt';verify.compile_local(sorted((output/'sources').rglob('*.java')),rebuilt,classpath=dependency if providers else None,release=coverage.get('minimum_java_release',8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r['class']+'->'+r['name']+r['prototype'] for r in coverage.get('generated_source_helpers',[])}
        expected_helpers={c+'->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;' for c in classes}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:raise RuntimeError('Multiple-exit recovery invented or omitted an entry')
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ('access','signature','superclass','interfaces','fields','inner_class','enclosing_method','deprecated_attribute','runtime_visible_annotations','runtime_invisible_annotations'):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError('Original multiple-exit class metadata changed: '+c+':'+key)
            for identity,metadata in records[c]['methods'].items():
                if after[c]['methods'].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError('Original multiple-exit method metadata/Exceptions changed: '+identity)
        runtime=(rebuilt,harness,dependency) if providers else (rebuilt,harness)
        if execute(runtime,'rebuilt')!=baseline:raise RuntimeError('Multiple-exit recovery changed partial stores, effects or failures')
        source=(fixture/'fixture/Core.java').read_text()
        mutations=(('wrong-first','first=a;','first=0;',0),('wrong-second','second=b;','second=0;',0),
                   ('late-first','first=a; Trace.event(2);','Trace.event(2); first=a;',0),
                   ('wrong-exception','throw Trace.failure1;','throw Trace.failure2;',1),
                   ('missing-exit','Trace.mode==1','Trace.mode==99',1),('wrong-stage','Trace.event(5);','Trace.event(6);',1))
        for label,before,replacement,mode in mutations:
            if source.count(before)!=1:raise RuntimeError('Original multiple-exit wrong-program anchor changed')
            path=work/label/'fixture/Core.java';path.parent.mkdir(parents=True);path.open('x').write(source.replace(before,replacement))
            wrong=work/(label+'-classes');verify.compile_local([path],wrong,classpath=originals,release=8)
            result=subprocess.run(command((wrong,originals,harness),False,1,mode),text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+'-counterexample.log')).open('x').write(result.stdout+result.stderr)
            if result.returncode==0 or 'owned multiple final exits' not in result.stderr:raise RuntimeError('Frozen observer missed incorrect final program: '+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError('Original inputs/SDK/frozen observers changed')
        evidence={'status':'success','class_count':len(classes),'method_count':len(methods),'fresh_jvm_cases':len(baseline),'attempts_per_case':2,
                  'reverse_exits':args.reverse_exits,'dependency_trace':args.dependency_trace,'all_java':'compiled','original_metadata_and_exceptions':'unchanged',
                  'every_partial_wide_final_store':'matched','independent_throw_identity_null_and_sticky_init_failures':'matched',
                  'independent_wrong_programs':len(mutations),'input_and_oracle_bytes':'unchanged'}
        (work/'evidence.json').open('x').write(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=='__main__':raise SystemExit(main())
