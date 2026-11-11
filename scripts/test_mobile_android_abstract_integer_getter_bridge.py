#!/usr/bin/env python3
"""Verify inherited final integer bridges that call original abstract getters."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_inherited_erased_parameters import add_parameter_signature
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_plain_integer_bridge import bridge_flag


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-interface", action="store_true")
    parser.add_argument("--class-ancestor", action="store_true")
    parser.add_argument("--dependency-ancestor", action="store_true")
    args = parser.parse_args()
    args.class_ancestor |= args.dependency_ancestor
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-abstract-integer-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "abstract_integer_getter_bridge/java").rglob("*.java"))
        if args.class_ancestor:
            inputs += sorted((FIXTURES / "integer_bridge_class_ancestors/java").rglob("*.java"))
            projected = []
            for source in inputs:
                if source.name == "Parent.java":
                    replacement = work / "owned-java/fixture/Parent.java"
                    replacement.parent.mkdir(parents=True)
                    replacement.write_text(source.read_text().replace("implements CountView<P>",
                                                                      "extends CounterRoot<P> implements CountView<P>"))
                    source = replacement
                projected.append(source)
            inputs = projected
        verify.compile_local(inputs, originals)
        harness_source = FIXTURES / "harness/AbstractIntegerGetterBridgeHarness.java"
        if args.class_ancestor:
            replacement = work / "owned-harness" / harness_source.name
            replacement.parent.mkdir()
            content = harness_source.read_text().replace("        Child child = new Child();",
                "        Child child = new Child();\n        check(Trace.events == 33);")
            content = content.replace("        Method size = Parent.class.getMethod(\"size\");",
                '        check(Parent.class.getGenericSuperclass().getTypeName().equals("fixture.CounterRoot<P>"));\n'
                '        check(CounterRoot.class.getTypeParameters()[0].getName().equals("R"));\n'
                '        Method size = Parent.class.getMethod("size");')
            replacement.write_text(content)
            harness_source = replacement
        verify.compile_local([harness_source], harness, classpath=originals)
        receipt = bridge_flag(originals / "fixture/Parent.class", "size", "()I", 0x0011, 0x0051)
        (work / "owned-bridge-flags.json").write_text(json.dumps(receipt, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        external = {"Lfixture/CountView;"} if args.dependency_interface else set()
        if args.dependency_ancestor:
            external.add("Lfixture/CounterRoot;")
        classes = all_classes - external
        methods = {k:v for k,v in all_methods.items() if k.split("->",1)[0] in classes}
        dependency = work / "owned-counter.jar" if external else None
        if dependency:
            with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
                for descriptor in sorted(external):
                    name = descriptor[1:-1] + ".class"
                    archive.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest() if dependency else None
        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}
        input_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals,harness))),
                               "AbstractIntegerGetterBridgeHarness", "original"], "baseline")
        if baseline.splitlines() != ["values=80,identities=10,failures=10",
                "abstract-target=retained,original-final-bridge-owner-and-virtual-dispatch=matched,bridge-flags=reported"]:
            raise RuntimeError("Owned abstract-getter baseline missed dispatch or failure cases: " + baseline)
        classpath = ["--classpath", args.android_jar.resolve()] + (["--classpath",dependency] if dependency else [])
        def make_dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            paths = [p for p in sorted(source.rglob("*.class"))
                     if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8,"--debug","--min-api","24",*classpath,"--output",destination,*paths],"d8-"+destination.name)
            dex = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned abstract-getter fixture did not produce one DEX")
            return dex
        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump,"-f","-h","-l","plain",dex],"sdk-inventory")
        inventory = parse_dexdump(dump,dex.name,dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned abstract-getter inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(),"mobile",dex,*classpath,"-o",output],"recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] != 0 or coverage["projected_method_count"] != 1 or
                coverage["status"] != "partial" or coverage.get("generic_signature_projections")):
            raise RuntimeError("Abstract-getter bridge changed original identities or full generic contracts")
        row = next(r for r in coverage["methods"] if r["identity"] == "Lfixture/Parent;->size()I")
        if (row.get("projection_kind") != "plain-interface-forwarding-bridge" or
                row.get("bridge_target") != "Lfixture/Parent;->getCount()I" or not row.get("metadata_differences")):
            raise RuntimeError("Final bridge lost its original abstract virtual target")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")),rebuilt,classpath=dependency,
                             release=coverage.get("minimum_java_release",8))
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify,rebuilt)
        helpers = {r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        if rebuilt_classes != classes or set(rebuilt_methods) != set(methods) | helpers:
            raise RuntimeError("javac copied an inherited bridge or added an unreported method")
        actual = verify.run([verify.java,"-cp",os.pathsep.join(map(str,[rebuilt,harness]+([dependency] if dependency else []))),
                             "AbstractIntegerGetterBridgeHarness","recovered"],"recovered-execution")
        if actual != baseline:
            raise RuntimeError("Abstract virtual getter changed integer values, effects, dispatch or exceptions")
        negative = work / "negative"
        shutil.copytree(originals,negative)
        receipt = add_parameter_signature(negative / "fixture/Parent.class","<E:Ljava/lang/Object;>()I",
                                          method_name="getCount",prototype="()I",original_flags=0x0401,code_count=0)
        (work / "negative-signature.json").write_text(json.dumps(receipt,indent=2)+"\n")
        negative_dex = make_dex(negative,work / "negative-dex")
        rejected = work / "rejected"
        result = subprocess.run([str(args.neverd.resolve()),"mobile",str(negative_dex),*map(str,classpath),"-o",str(rejected)],
                                 text=True,capture_output=True,timeout=300,env=verify.environment)
        (work / "negative-recovery.log").write_text(result.stdout+result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("New abstract-getter method formals bypassed the original plain contract")
        if (hashes(originals) != input_hashes or hashes(harness) != harness_hashes or
                (dependency and hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash)):
            raise RuntimeError("Original concrete fixture, harness or owned dependency changed")
        evidence = {"status":"success","class_count":len(classes),"method_count":len(methods),"value_cases":80,
                    "identity_cases":10,"ordered_failure_cases":10,"abstract_virtual_target":"retained",
                    "original_final_bridge_owner":"retained","original_inventory_and_concrete_harness_code":"unchanged",
                    "full_generic_reflection":"matched","bridge_flags":"reported","coverage":"partial",
                    "new_method_formals":"rejected","dependency_interface":args.dependency_interface,
                    "generic_class_ancestor":args.class_ancestor,"dependency_ancestor":args.dependency_ancestor}
        (work / "evidence.json").write_text(json.dumps(evidence,indent=2)+"\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
