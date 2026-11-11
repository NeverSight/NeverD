#!/usr/bin/env python3
"""Verify captured nominal covariance through source and frozen class ancestry."""
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
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_bounded_covariant_methods import omit_owned_interface_bridge

PROVIDERS={"Lfixture/Root;", "Lfixture/Api;", "Lfixture/Base;"}
CAPTURED={"Lfixture/Owner$Worker;", "Lfixture/Owner$PrivateWorker;"}

def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd",type=Path,required=True)
    parser.add_argument("--java-home",type=Path)
    parser.add_argument("--android-jar",type=Path,required=True)
    parser.add_argument("--d8",type=Path)
    parser.add_argument("--work-dir",type=Path)
    parser.add_argument("--dependency-provider",action="store_true")
    args=parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True,exist_ok=False)
    context=nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-covariance-")
    with context as directory:
        work=Path(directory).resolve()
        verify=Verify(work,choose_jdk(args.java_home),choose_d8(args.d8),300)
        fixture=FIXTURES/"captured_covariant_bridges/java"
        originals,harness=work/"originals",work/"harness"
        verify.compile_local(sorted(fixture.rglob("*.java")),originals,release=8)
        verify.compile_local([FIXTURES/"harness"/(name+".java") for name in
            ("CapturedCovariantCaller","CapturedCovariantDerived","CapturedCovariantHarness")],harness,classpath=originals,release=8)
        receipt=omit_owned_interface_bridge(harness/"CapturedCovariantDerived.class",b"getItem(I)Ljava/lang/Object;")
        (work/"inherited-original-bridge.json").write_text(json.dumps(receipt,indent=2)+"\n")
        original_hashes,harness_hashes=hashes(originals),hashes(harness)
        sdk_hash=hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes,all_methods=compiler_inventory(verify,originals)
        if len(all_classes)!=8 or len(all_methods)!=16:
            raise RuntimeError("Owned captured covariance inventory changed: "+str((len(all_classes),len(all_methods))))
        classes=all_classes-(PROVIDERS if args.dependency_provider else set())
        methods={m for m in all_methods if m.split("->",1)[0] in classes}
        records,exceptions=class_identity.compiler_classes(originals),exception_inventory(originals)
        if any(row["signature"] for row in records.values()):
            raise RuntimeError("Captured covariance acquired a class Signature")
        dependency=work/"owned-provider.jar"
        with zipfile.ZipFile(dependency,"x",compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(PROVIDERS):
                name=owner[1:-1]+".class";jar.writestr(name,(originals/name).read_bytes())
        dependency_hash=hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath=["--classpath",args.android_jar.resolve()]
        if args.dependency_provider:classpath += ["--classpath",dependency]
        def execute(runtime,label):
            output=verify.run([verify.java,"-cp",os.pathsep.join(map(str,runtime)),"CapturedCovariantHarness"],label)
            if output.splitlines()!=["calls=168,failures=105,captures-and-original-virtual-bridges=matched"]:
                raise RuntimeError("Owned captured covariance matrix changed: "+output)
            return output
        def dex(root,label):
            destination=work/label;destination.mkdir()
            paths=[p for p in sorted(root.rglob("*.class")) if "L"+p.relative_to(root).as_posix()[:-6]+";" in classes]
            verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",destination,*paths],"d8-"+label)
            result=destination/"classes.dex"
            if list(destination.glob("classes*.dex"))!=[result]:raise RuntimeError("Unexpected owned DEX inventory")
            return result
        baseline=execute((originals,harness),"baseline")
        source_dex=dex(originals,"dex")
        dump=verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory=parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]}!=classes or {r["identity"] for r in inventory["methods"]}!=methods:
            raise RuntimeError("D8 changed original captured covariance entries")
        (work/"sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        disassembly=verify.run([verify.dexdump,"-d",source_dex],"original-bridge-disassembly")
        witnessed=set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)",disassembly):
            name=re.search(r"(?m)^\s+name\s+: '([^']+)'",block[2]);prototype=re.search(r"(?m)^\s+type\s+: '([^']+)'",block[2])
            if block[1] not in CAPTURED or not name or name[1]!="getItem" or not prototype or prototype[1]!="(I)Ljava/lang/Object;":continue
            ops=re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)",block[2])
            if ops not in (["invoke-virtual","move-result-object","return-object"],["invoke-virtual/range","move-result-object","return-object"]) or block[1]+".getItem:" not in block[2]:
                raise RuntimeError("Original captured covariance bridge body changed")
            witnessed.add(block[1]+"->getItem(I)Ljava/lang/Object;")
        if len(witnessed)!=2:raise RuntimeError("Missing original bridge witnesses")
        output=work/"recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage=json.loads((output/"report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]}!=methods or coverage["class_count"]!=len(classes)
            or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"] or coverage["projected_method_count"]!=6
            or coverage["status"]!="partial" or coverage.get("minimum_java_release")!=25 or coverage.get("generic_signature_projections")):
            raise RuntimeError("Captured covariance recovery lost original entries or contracts")
        rows={r["identity"]:r for r in coverage["methods"]}
        for identity in witnessed:
            if rows[identity].get("bridge_target")!=identity.replace("Ljava/lang/Object;","Lfixture/Item;") or not rows[identity].get("projection_kind"):
                raise RuntimeError("Original bridge lacks its complete target receipt")
        for identity,row in rows.items():
            if identity.split("->",1)[0] in CAPTURED and identity not in witnessed and row.get("projection_kind")!="inner-to-static-member":
                raise RuntimeError("Original captured method lost its ownership disclosure")
        bindings=coverage.get("class_source_bindings",[])
        captures={owner+"->this$0:Lfixture/Owner;" for owner in CAPTURED}
        if len(bindings)!=2 or {row.get("outer_capture_field") for row in bindings}!=captures or any(row["binding_kind"]!="inner-to-static-member" for row in bindings):
            raise RuntimeError("Captured ownership disclosure changed")
        rebuilt=work/"rebuilt"
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=25,classpath=dependency if args.dependency_provider else None)
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        helpers={r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers={owner+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes-{"Lfixture/Root;","Lfixture/Api;"}}
        if actual_classes!=classes or set(actual_methods)!=methods|expected_helpers or helpers!=expected_helpers:
            raise RuntimeError("Captured covariance invented or lost runtime entries")
        actual_records,actual_exceptions=class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for owner in classes:
            before,after=records[owner],actual_records[owner]
            expected_fields=json.loads(json.dumps(before["fields"]))
            expected_inner=dict(before["inner_class"]) if before["inner_class"] else None
            if owner in CAPTURED:
                field=expected_fields[owner+"->this$0:Lfixture/Owner;"]
                if field["access"]!=0x1010 or not expected_inner:raise RuntimeError("Original capture field metadata changed")
                field["access"] &= ~0x1000;expected_inner["access"] |= 8
            if after["fields"]!=expected_fields or after["inner_class"]!=expected_inner:
                raise RuntimeError("Capture metadata differs beyond disclosed source binding")
            for key in ("access","signature","superclass","interfaces","enclosing_method","deprecated_attribute","runtime_visible_annotations"):
                if before.get(key)!=after.get(key):raise RuntimeError("Captured class contract changed: "+owner+":"+key)
            for identity,metadata in before["methods"].items():
                if after["methods"].get(identity)!=metadata or actual_exceptions.get(identity)!=exceptions[identity]:
                    raise RuntimeError("Original method metadata/Exceptions changed: "+identity)
        runtime=(rebuilt,harness,dependency) if args.dependency_provider else (rebuilt,harness)
        if execute(runtime,"rebuilt")!=baseline:raise RuntimeError("Captured covariance changed runtime behavior")
        negative=work/"method-formal-originals";shutil.copytree(originals,negative)
        receipt=add_parameter_signature(negative/"fixture/Owner$Worker.class","<T:Ljava/lang/Object;>(I)Lfixture/Item;",method_name="getItem",prototype="(I)Lfixture/Item;")
        (work/"method-formal-encoding.json").write_text(json.dumps(receipt,indent=2)+"\n")
        encoded=dex(negative,"method-formal-dex");rejected=work/"method-formal-rejected"
        result=subprocess.run([str(args.neverd.resolve()),"mobile",str(encoded),*map(str,classpath),"-o",str(rejected)],text=True,capture_output=True,timeout=300,env=verify.environment)
        (work/"method-formal-recovery.log").write_text(result.stdout+result.stderr)
        if result.returncode==0 or rejected.exists() or "generic" not in result.stderr:raise RuntimeError("Captured covariance admitted a changed full contract")
        wrong=work/"changed-result/fixture/Owner.java";wrong.parent.mkdir(parents=True)
        source=(fixture/"fixture/Owner.java").read_text()
        if source.count("return Owner.this.items[index];")!=2:raise RuntimeError("Owned return anchors changed")
        wrong.write_text(source.replace("return Owner.this.items[index];","Item observed = Owner.this.items[index]; return null;"))
        wrong_classes=work/"changed-result-classes";verify.compile_local([wrong],wrong_classes,classpath=originals,release=8)
        result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong_classes,originals,harness))),"CapturedCovariantHarness"],text=True,capture_output=True,timeout=300,env=verify.environment)
        (work/"changed-result-counterexample.log").write_text(result.stdout+result.stderr)
        if result.returncode==0 or "captured covariance contract" not in result.stderr:raise RuntimeError("Frozen observer missed changed captured result identity")
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence={"status":"success","coverage":"partial","class_count":len(classes),"method_count":len(methods),"projected_original_bridges":2,"projected_methods":6,"runtime_calls":168,"expected_failures":105,"dependency_provider":args.dependency_provider,"capture_metadata":"only reported static member and synthetic field differences","original_metadata_Exceptions_and_inventories":"matched","original_bridges_external_overrides_and_outer_identity":"matched","same_code_constraint_counterexample":"rejected","changed_result_counterexample":"detected","input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n");print(json.dumps(evidence,sort_keys=True))
    return 0
if __name__=="__main__":raise SystemExit(main())
