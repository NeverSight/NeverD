#!/usr/bin/env python3
"""Verify three formal bindings and exact captured-owner factories with a nominal bound."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
from copy import deepcopy
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
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


MEMBER = "Lfixture/Outer$Member;"
FACTORY = "Lfixture/Factory;->create(Lfixture/Outer;Lfixture/Converter;Ljava/lang/String;I)Lfixture/Outer$Member;"
CONSTRUCTOR = MEMBER + "-><init>(Lfixture/Outer;Ljava/lang/Object;Lfixture/Vector;)V"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-factory", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-bounded-captured-factory-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "bounded_captured_factory/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("BoundedCapturedFactoryHarness", "BoundedCapturedFactoryCaller")], harness,
                             classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 8 or len(all_methods) != 14:
            raise RuntimeError("Original bounded captured inventory changed: " + repr((len(all_classes), len(all_methods))))
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        signature = ("<S:Ljava/lang/Object;T:Ljava/lang/Object;V:Lfixture/Vector;>"
                     "(Lfixture/Outer<TS;>;Lfixture/Converter<TT;TV;>;Ljava/lang/String;I)"
                     "Lfixture/Outer<TS;>.Member<TT;TV;>;")
        if records["Lfixture/Factory;"]["methods"][FACTORY]["signature"] != signature:
            raise RuntimeError("Compiler changed the complete factory binding relation")
        classes = all_classes - ({"Lfixture/Factory;"} if args.dependency_factory else set())
        methods = {identity for identity in all_methods if identity.split("->",1)[0] in classes}
        dependency = work / "owned-factory.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            archive.writestr("fixture/Factory.class", (originals / "fixture/Factory.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_factory:
            classpath += ["--classpath", dependency]

        def execute(runtime, metadata, label):
            outputs = []
            for route in range(4):
                for mode in range(15):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "BoundedCapturedFactoryHarness", str(route), str(mode), metadata], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                        raise RuntimeError("Original bounded captured matrix changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "input", "baseline")
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L"+p.relative_to(originals).as_posix()[:-6]+";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Bounded captured fixture changed DEX inventory")
        dump = verify.run([verify.dexdump,"-f","-h","-l","plain",source_dex],"sdk-inventory")
        inventory = parse_dexdump(dump,source_dex.name,dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original bounded captured runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory,indent=2)+"\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(),"mobile",source_dex,*classpath,"-o",output],"recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]:row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage["projected_method_count"] != 3 or rows[CONSTRUCTOR]["generic_signature_omitted_parameters"] != 1
                or {identity for identity,row in rows.items() if row["status"]=="source-projected"}
                   != {CONSTRUCTOR,MEMBER+"->value()Ljava/lang/Object;",MEMBER+"->vector()Lfixture/Vector;"}):
            raise RuntimeError("Bounded captured recovery changed original inventory or constructor receipt")
        bindings = coverage.get("class_source_bindings",[])
        if (len(bindings) != 1 or bindings[0]["class"] != MEMBER or bindings[0]["binding_kind"] != "inner-to-static-member"
                or bindings[0]["outer_capture_field"] != MEMBER+"->this$0:Lfixture/Outer;"):
            raise RuntimeError("Missing original captured outer storage receipt")
        projections = coverage.get("generic_signature_projections",[])
        if args.dependency_factory:
            if projections:
                raise RuntimeError("A frozen SDK Signature obtained an input metadata projection")
        elif (len(projections) != 1 or projections[0]["declaration"] != FACTORY
                or projections[0]["original_signature"] != signature or len(projections[0]["changes"]) != 1
                or projections[0]["changes"][0]["kind"] != "static-inner-owner-arguments"
                or projections[0]["changes"][0]["source_type"] != MEMBER):
            raise RuntimeError("Missing exact captured result owner Signature projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")),rebuilt,
                             classpath=dependency if args.dependency_factory else None,release=coverage.get("minimum_java_release",8))
        actual_classes,actual_methods = compiler_inventory(verify,rebuilt)
        helpers = {r["class"]+"->"+r["name"]+r["prototype"] for r in coverage.get("generated_source_helpers",[])}
        expected_helpers = {c+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes-{"Lfixture/Converter;"}}
        if actual_classes != classes or set(actual_methods) != methods|expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Bounded captured recovery invented or omitted a runtime entry")
        after,after_exceptions = class_identity.compiler_classes(rebuilt),exception_inventory(rebuilt)
        for c in classes:
            before,actual = records[c],after[c]
            for key in ("access","signature","superclass","interfaces","fields","inner_class","enclosing_method",
                        "deprecated_attribute","runtime_visible_annotations","runtime_invisible_annotations"):
                expected = deepcopy(before.get(key))
                if c==MEMBER and key=="inner_class":
                    expected["access"] |= 8
                if c==MEMBER and key=="fields":
                    expected[MEMBER+"->this$0:Lfixture/Outer;"]["access"] &= ~0x1000
                if expected != actual.get(key):
                    raise RuntimeError("Undisclosed bounded captured class metadata changed: "+c+":"+key)
            for identity,metadata in before["methods"].items():
                expected = deepcopy(metadata)
                if identity==FACTORY:
                    expected["signature"] = signature.replace("Lfixture/Outer<TS;>.Member<","Lfixture/Outer$Member<")
                if identity==CONSTRUCTOR:
                    expected["signature"] = "(Lfixture/Outer;TT;TV;)V"
                if actual["methods"].get(identity) != expected or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Undisclosed bounded captured method metadata or Exceptions changed: "+identity)
        runtime = (rebuilt,harness,dependency) if args.dependency_factory else (rebuilt,harness)
        if execute(runtime,"source","rebuilt") != baseline:
            raise RuntimeError("Bounded captured factory changed callbacks, actual outer identity, partial writes or failures")
        mutations = (("repeated-converter","Factory","Trace.hit(4);","converter.convert(value); Trace.hit(4);"),
                     ("wrong-outer","Factory","mode == 0 ? outer : outer.destination","outer"),
                     ("early-fields","Outer","Trace.hit(6);","this.value = value; this.vector = vector; Trace.hit(6);"))
        for label,name,old,new in mutations:
            text = (fixture / ("fixture/"+name+".java")).read_text()
            if text.count(old)!=1:
                raise RuntimeError("Bounded captured counterexample anchor changed")
            if label=="early-fields":
                text=text.replace("            this.value = value;\n            this.vector = vector;\n","")
            wrong=work/label/("fixture/"+name+".java");wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old,new))
            compiled=work/(label+"-classes")
            verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),
                "BoundedCapturedFactoryHarness","1","12" if label=="wrong-outer" else "6" if label=="early-fields" else "0","input"],
                text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).write_text(result.stdout+result.stderr)
            if result.returncode==0 or "owned bounded captured factory contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect "+label)
        if (hashes(originals)!=original_hashes or hashes(harness)!=harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest()!=sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest()!=dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence={"status":"success","class_count":len(classes),"method_count":len(methods),"dependency_factory":args.dependency_factory,
                  "fresh_jvm_cases_per_stage":len(baseline),"stages":2,"attempts_per_case":2,"three_formals_and_nominal_bound":"retained",
                  "actual_outer_and_callback_result_identity":"matched","mutable_vector_and_partial_constructor_fields":"matched",
                  "original_exception_identity_and_order":"matched","captured_static_source_metadata_changes":"disclosed",
                  "other_metadata_and_exceptions":"unchanged","independent_compiler_counterexamples":len(mutations),"input_and_oracle_bytes":"unchanged"}
        (work/"evidence.json").write_text(json.dumps(evidence,indent=2)+"\n")
        print(json.dumps(evidence,sort_keys=True))
    return 0


if __name__=="__main__":
    raise SystemExit(main())
