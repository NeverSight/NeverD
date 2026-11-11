#!/usr/bin/env python3
"""Verify scoped producer/comparison carriers without predicting provider results."""
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


SIGNATURE="<T:Ljava/lang/Object;>(Ljava/util/List<+TT;>;IILfixture/Comparison<-TT;Ljava/lang/Integer;>;)I"
TARGET="Lfixture/Provider;->scan(Ljava/util/List;IILfixture/Comparison;)I"


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ("neverd","android-jar"):parser.add_argument("--"+name,type=Path,required=True)
    for name in ("java-home","d8","work-dir"):parser.add_argument("--"+name,type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    parser.add_argument("--bounded-owner",action="store_true")
    parser.add_argument("--package-owner",action="store_true")
    args=parser.parse_args()
    if args.work_dir:args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-comparison-carrier-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"comparison_carrier/java"
        if args.bounded_owner or args.package_owner:
            profile=work/'owner-source';shutil.copytree(fixture,profile);fixture=profile
            path=fixture/'fixture/Provider.java';text=path.read_text()
            if text.count('public final class Provider<T>')!=1:raise RuntimeError('Original comparison owner anchor changed')
            if args.bounded_owner:text=text.replace('class Provider<T>','class Provider<T extends Number>')
            if args.package_owner:text=text.replace('public final class Provider','final class Provider')
            path.write_text(text)
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(n+".java") for n in ("ComparisonCarrierCaller","ComparisonCarrierHarness")],harness,classpath=originals,release=8)
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=4 or len(all_methods)!=8:raise RuntimeError("Original comparison carrier inventory changed")
        classes=all_classes-({'Lfixture/Provider;'} if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        if records['Lfixture/Provider;']['methods'][TARGET]['signature']!=SIGNATURE:raise RuntimeError('Original comparison relation changed')
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x") as jar:jar.writestr('fixture/Provider.class',(originals/'fixture/Provider.class').read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def command(runtime,route,mode):
            return [verify.java,'-cp',os.pathsep.join(map(str,runtime)),'ComparisonCarrierHarness',str(route),str(mode),
                    str(args.bounded_owner).lower(),str(args.package_owner).lower()]
        def execute(runtime,label):
            outputs=[]
            for route in range(4):
                for mode in range(18):
                    output=verify.run(command(runtime,route,mode),f'{label}-{route}-{mode}')
                    pattern=f'route={route},mode={mode},attempt=[01],trace=[0-9]+'
                    if len(output.splitlines())!=2 or not all(re.fullmatch(pattern,line) for line in output.splitlines()):
                        raise RuntimeError('Original comparison observer output changed: '+output)
                    outputs.append(output)
            return outputs
        baseline=execute((originals,harness),'baseline')
        dex=work/'dex';dex.mkdir()
        paths=[p for p in sorted(originals.rglob('*.class')) if 'L'+p.relative_to(originals).as_posix()[:-6]+';' in classes]
        verify.run([*verify.d8,'--debug','--min-api','24',*classpath,'--output',dex,*paths],'d8')
        source_dex=dex/'classes.dex'
        if list(dex.glob('classes*.dex'))!=[source_dex]:raise RuntimeError('Original comparison DEX inventory changed')
        dump=verify.run([verify.dexdump,'-f','-h','-l','plain',source_dex],'sdk-inventory')
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r['descriptor'] for r in inventory['classes']}!=classes or {r['identity'] for r in inventory['methods']}!=methods:
            raise RuntimeError('D8 changed an original comparison entry')
        (work/'sdk-inventory.json').open('x').write(json.dumps(inventory,indent=2)+'\n')
        output=work/'recovered'
        verify.run([args.neverd.resolve(),'mobile',source_dex,*classpath,'-o',output],'recover')
        coverage=json.loads((output/'report.json').read_text())['android_method_recovery']
        if ({r['identity'] for r in coverage['methods']}!=methods or coverage['class_count']!=len(classes) or coverage['method_count']!=len(methods)
                or coverage['unrecovered_method_count'] or coverage.get('projected_method_count',0) or coverage['status']!='recovered'
                or coverage.get('class_source_bindings') or coverage.get('field_source_bindings') or coverage.get('generic_signature_projections')):
            raise RuntimeError('Comparison binding changed original entries or generic contracts')
        rebuilt=work/'rebuilt'
        verify.compile_local(sorted((output/'sources').rglob('*.java')),rebuilt,classpath=dependency if args.dependency_provider else None,release=coverage.get('minimum_java_release',8))
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r['class']+'->'+r['name']+r['prototype'] for r in coverage.get('generated_source_helpers',[])}
        expected_helpers={c+'->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;' for c in classes if c!='Lfixture/Comparison;'}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError('Comparison binding invented or omitted a runtime entry')
        after,after_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            for key in ('access','signature','superclass','interfaces','fields','inner_class','enclosing_method','deprecated_attribute','runtime_visible_annotations','runtime_invisible_annotations'):
                if records[c].get(key)!=after[c].get(key):raise RuntimeError('Original comparison metadata changed: '+c+':'+key)
            for identity,metadata in records[c]['methods'].items():
                if after[c]['methods'].get(identity)!=metadata or after_exceptions[identity]!=exceptions[identity]:raise RuntimeError('Original comparison method metadata changed: '+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,'rebuilt')!=baseline:raise RuntimeError('Comparison binding changed effects, unboxing, opaque results or failures')
        mutations=(('ignored-comparison','comparison.apply(value).intValue()','0',0),
            ('wrong-bounds','int i=from;','int i=0;',0),('wrong-result','return Trace.result;','return Trace.sum;',0),
            ('cached-element','values.get(i)','values.get(from)',0),('late-current','Trace.current=value;Trace.hit(3);','Trace.hit(3);Trace.current=value;',13))
        source=(fixture/'fixture/Provider.java').read_text()
        for label,before,after,mode in mutations:
            if source.count(before)!=1:raise RuntimeError('Original comparison wrong-program anchor changed')
            changed=work/label/'fixture/Provider.java';changed.parent.mkdir(parents=True);changed.open('x').write(source.replace(before,after))
            wrong=work/(label+'-classes');verify.compile_local([changed],wrong,classpath=originals,release=8)
            result=subprocess.run(command((wrong,originals,harness),1,mode),text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+'-counterexample.log')).open('x').write(result.stdout+result.stderr)
            if result.returncode==0 or 'owned comparison carrier' not in result.stderr:raise RuntimeError('Independent observer missed incorrect comparison behavior: '+label)
        drift=work/'signature-drift';shutil.copytree(originals,drift)
        changed=SIGNATURE.replace('Ljava/lang/Integer;','+Ljava/lang/Integer;')
        receipt=change_signature(drift/'fixture/Provider.class',SIGNATURE.encode(),changed.encode())
        (work/'signature-drift.json').open('x').write(json.dumps(receipt,indent=2)+'\n')
        result=subprocess.run(command((drift,harness),1,0),text=True,capture_output=True,timeout=300,env=verify.environment)
        (work/'signature-drift-counterexample.log').open('x').write(result.stdout+result.stderr)
        if result.returncode==0 or 'owned comparison carrier' not in result.stderr:raise RuntimeError('Frozen observer missed unchanged-Code comparison Signature drift')
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):raise RuntimeError('Original comparison inputs or frozen observers changed')
        evidence={'status':'success','class_count':len(classes),'method_count':len(methods),'fresh_jvm_cases':len(baseline),'attempts_per_case':2,
            'dependency_provider':args.dependency_provider,'bounded_owner':args.bounded_owner,'package_owner':args.package_owner,'independent_wrong_programs':6,
            'all_original_entries_metadata_and_exceptions':'unchanged','scoped_callback_unboxing_bounds_effects_opaque_results_and_failures':'matched',
            'original_inputs_and_immutable_callers':'unchanged'}
        (work/'evidence.json').open('x').write(json.dumps(evidence,indent=2)+'\n');print(json.dumps(evidence,sort_keys=True))
    return 0

if __name__=='__main__':raise SystemExit(main())
