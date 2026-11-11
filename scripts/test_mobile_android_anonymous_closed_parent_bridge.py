#!/usr/bin/env python3
"""Verify closed anonymous scopes inherit complete fixed parent bridge contracts."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-anonymous-closed-parent-bridge-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "anonymous_closed_parent_bridge"
        originals, harness = work / "originals", work / "harness"
        name = "fixture.AnonymousClosedParentBridgeHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (n + ".java") for n in ("AnonymousClosedParentBridgeHarness", "AnonymousClosedParentBridgeCaller")], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (6, 12):
            raise RuntimeError("Owned anonymous closed parent bridge inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Value;", "Lfixture/Reader;", "Lfixture/Parent;"}
        excluded = providers if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"

        def write_provider(destination, source):
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                for owner in sorted(providers):
                    entry = owner[1:-1] + ".class"
                    jar.writestr(entry, (source / entry).read_bytes())

        write_provider(dependency, originals)
        dependency_hash = sha(dependency)

        def execute(runtime, method):
            outputs = []
            for route in range(5):
                for branch in range(2):
                    for mode in range(16):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(branch), str(mode)],
                                          f"{method}-{route}-{branch}-{mode}")
                        if text != "attempts=2,anonymous-closed-parent-bridge=matched\n":
                            raise RuntimeError("Frozen anonymous closed parent bridge observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        wrong = [
            ("lost-parent", "Host.java", "return super.read(fraction,left,right);", "return Trace.result;", 1, 1, 0),
            ("repeated-parent", "Host.java", "return super.read(fraction,left,right);", "super.read(fraction,left,right);return super.read(fraction,left,right);", 1, 1, 0),
            ("wrong-receiver", "Host.java", "return super.read(fraction,left,right);", "return new Parent().read(fraction,left,right);", 1, 1, 0),
            ("swapped-inputs", "Host.java", "return super.read(fraction,left,right);", "return super.read(fraction,right,left);", 1, 1, 0),
            ("lost-capture-effect", "Host.java", "Host.this.calls++;", ";", 1, 1, 0),
            ("lost-captured-owner", "Host.java", "Trace.host=Host.this;", "Trace.host=null;", 1, 1, 0),
            ("lost-fraction", "Parent.java", "Trace.fraction=fraction;", "Trace.fraction=0;", 1, 1, 0),
            ("canonicalized-nan", "Parent.java", "Trace.fraction=fraction;", "Trace.fraction=Float.intBitsToFloat(Float.floatToIntBits(fraction));", 1, 1, 13),
            ("lost-left-read", "Parent.java", "Trace.observedLeft=left.code;", "Trace.observedLeft=31;", 1, 1, 1),
            ("lost-right-read", "Parent.java", "Trace.observedRight=right.code;", "Trace.observedRight=47;", 1, 1, 2),
            ("lost-callback", "Parent.java", "Trace.action.run();", ";", 1, 1, 0),
            ("duplicate-callback", "Parent.java", "Trace.action.run();", "Trace.action.run();Trace.action.run();", 1, 1, 0),
            ("wrapped-failure", "Parent.java", "Trace.action.run();", "try{Trace.action.run();}catch(Throwable failure){throw new IllegalArgumentException(failure);}", 1, 1, 9),
            ("fresh-result", "Parent.java", "return Trace.result;", "return new Value();", 1, 1, 0),
            ("early-result", "Parent.java", "Trace.action.run();Trace.hit(5);return Trace.result;", "Value saved=Trace.result;Trace.action.run();Trace.hit(5);return saved;", 1, 1, 10),
            ("lost-generic-input", "AnonymousClosedParentBridgeCaller.java", "receiver.read(fraction,left,right)", "receiver.read(fraction,left,left)", 2, 1, 0),
        ]
        for label, filename, anchor, replacement, route, branch, mode in wrong:
            original_source = (FIXTURES / "harness" / filename) if filename.endswith("Caller.java") else fixture / "java/fixture" / filename
            text = original_source.read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Anonymous closed parent bridge counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(branch), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned anonymous closed parent bridge mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)
        classpath = ["--classpath", sdk]
        if excluded:
            classpath += ["--classpath", dependency]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original anonymous closed parent bridge definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        minimum_java_release=coverage.get("minimum_java_release",8)
        anonymous="Lfixture/Host$1;";ctor=anonymous+"-><init>(Lfixture/Host;)V"
        erased="->read(FLjava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;"
        typed="->read(FLfixture/Value;Lfixture/Value;)Lfixture/Value;"
        rows={row["identity"]:row for row in coverage["methods"]}
        bridges={anonymous+erased}|({"Lfixture/Parent;"+erased} if not excluded else set())
        locals={ctor,anonymous+typed,"Lfixture/Host;->make()Lfixture/Parent;"}
        if (set(rows)!=methods or coverage["class_count"]!=len(classes)
                or coverage["method_count"]!=len(methods) or coverage["unrecovered_method_count"]
                or coverage["status"]!="partial" or coverage.get("projected_method_count")!=len(bridges|locals)
                or coverage.get("generic_signature_projections") or minimum_java_release!=25):
            raise RuntimeError("Anonymous parent bridge lost original definitions or source contracts")
        for identity,row in rows.items():
            expected="source-projected" if identity in bridges|locals else "recovered" if facts[identity.split("->",1)[0]]["methods"][identity]["code"] else "declaration-only"
            if row["status"]!=expected:raise RuntimeError("Original anonymous parent Code role changed")
            if identity in bridges:
                owner=identity.split("->",1)[0]
                if (row.get("projection_kind")!="generic-interface-bridge" or row.get("bridge_target")!=owner+typed
                        or row.get("bridge_dispatch")!="Lfixture/Reader;"+erased):
                    raise RuntimeError("Anonymous bridge lost its exact inherited target")
            elif identity in locals and row.get("projection_kind")!="anonymous-to-named-local":
                raise RuntimeError("Anonymous source ownership is not disclosed")
        bindings=coverage.get("class_source_bindings",[])
        if len(bindings)!=1:raise RuntimeError("Expected one original anonymous local mapping")
        binding=bindings[0]
        if (binding["class"]!=anonymous or binding["binding_kind"]!="anonymous-to-named-local"
                or binding["binary_name_status"]!="changed"
                or not any("constructor parameters may differ" in item for item in binding.get("metadata_differences",[]))):
            raise RuntimeError("Anonymous local capture mapping lost its disclosure")
        rebuilt=work/"rebuilt";dependencies=[dependency] if excluded else []
        verify.compile_local(sorted((output/"sources").rglob("*.java")),rebuilt,release=minimum_java_release,
                             classpath=dependency if excluded else None)
        after,after_exceptions=compiler_facts(rebuilt),exception_inventory(rebuilt)
        candidates=[owner for owner,row in after.items() if (row.get("inner_class") or {}).get("name")==binding["source_name"]
                    and row["enclosing_method"]==facts[anonymous]["enclosing_method"]]
        if len(candidates)!=1:raise RuntimeError("Compiled local class lacks its exact original enclosure")
        renamed=candidates[0];mapped=lambda text:text.replace(anonymous,renamed)
        source_ids={identity:mapped(identity) for identity in methods}
        source_ids[ctor]=mapped(ctor).replace("-><init>(","-><init>(Lfixture/Host;",1)
        helpers={mapped(row["class"]+"->"+row["name"]+row["prototype"]) for row in coverage.get("generated_source_helpers",[])}
        expected_helpers={mapped(owner)+"->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes-{"Lfixture/Reader;"}}
        actual_classes,actual_methods=compiler_inventory(verify,rebuilt)
        if actual_classes!={mapped(owner) for owner in classes} or set(actual_methods)!=set(source_ids.values())|helpers or helpers!=expected_helpers:
            raise RuntimeError("Anonymous bridge invented or omitted compiled runtime entries")
        disclosures=[]
        for owner in classes:
            wanted=json.loads(mapped(json.dumps(facts[owner])));actual=copy.deepcopy(after[mapped(owner)])
            actual["methods"]={m:row for m,row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major")!=52 or actual.pop("major")!=69:raise RuntimeError("Anonymous bridge compiler release changed")
            if owner==anonymous:
                wanted["inner_class"]["name"]=binding["source_name"]
                capture=mapped(anonymous)+"->this$0:Lfixture/Host;"
                if wanted["fields"][capture]["access"]!=0x1010:raise RuntimeError("Original capture flags changed")
                wanted["fields"][capture]["access"] &= ~0x1000
                method=wanted["methods"].pop(mapped(ctor));method["prototype"]=source_ids[ctor].split("-><init>",1)[1]
                if method["method_parameters"]!=[{"name":None,"access":0x8010}]:
                    raise RuntimeError("Original anonymous constructor lost its mandated enclosing parameter")
                # The disclosed local constructor keeps that hidden enclosing
                # parameter and adds the explicit original capture argument.
                method["method_parameters"] += [{"name":None,"access":0}]
                wanted["methods"][source_ids[ctor]]=method
                disclosures.append({"original_class":owner,"compiled_class":renamed,"source_name":binding["source_name"],
                    "original_constructor":ctor,"compiled_constructor":source_ids[ctor],"capture_field":capture,"original_capture_flags":0x1010,"source_capture_flags":0x10,
                    "original_constructor_parameters":[{"name":None,"access":0x8010}],
                    "compiled_constructor_parameters":method["method_parameters"]})
            if actual!=wanted or any(after_exceptions[source_ids[m]]!=exceptions[m] for m in facts[owner]["methods"]):
                (work/"metadata-mismatch.json").write_text(json.dumps({"owner":owner,"expected":wanted,"actual":actual},indent=2)+"\n")
                raise RuntimeError("Original anonymous bridge metadata changed beyond its exact local capture disclosure: "+owner)
        (work/"disclosed-metadata.json").open("x").write(json.dumps(disclosures,indent=2)+"\n")
        if execute((rebuilt,harness,*dependencies),"rebuilt")!=baseline:
            raise RuntimeError("Anonymous parent bridge changed captures, dispatch, effects or original failures")
        short_signature="Ljava/lang/Object;Lfixture/Reader<Lfixture/Value;>;"
        controls=[
            ("wrong-slot","Ljava/lang/Object;Lfixture/Reader<Ljava/lang/String;>;"),
            ("covariant-interface","Ljava/lang/Object;Lfixture/Reader<+Lfixture/Value;>;"),
            ("contravariant-interface","Ljava/lang/Object;Lfixture/Reader<-Lfixture/Value;>;"),
            ("star-interface","Ljava/lang/Object;Lfixture/Reader<*>;"),
            ("foreign-formal","Ljava/lang/Object;Lfixture/Reader<TMissing;>;"),
            ("wrong-arity","Ljava/lang/Object;Lfixture/Reader<Lfixture/Value;Ljava/lang/Object;>;"),
            ("wrong-super","Lfixture/Value;Lfixture/Reader<Lfixture/Value;>;"),
            ("open-parent-scope","<Unused:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Reader<Lfixture/Value;>;"),
        ]
        for label, changed_signature in controls:
            changed_originals = work / (label + "-originals")
            shutil.copytree(originals, changed_originals)
            target = changed_originals / "fixture/Parent.class"
            code_before = method_code(target.read_bytes())
            receipt = change_signature(target, short_signature.encode(), changed_signature.encode())
            receipt["target_identity"] = "Lfixture/Parent;"
            if method_code(target.read_bytes()) != code_before or compiler_facts(changed_originals)["Lfixture/Parent;"]["signature"] != changed_signature:
                raise RuntimeError("Signature control changed Code or failed to retain exact metadata")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            counter_classpath = ["--classpath", sdk]
            if excluded:
                counter_provider = work / (label + "-provider.jar")
                write_provider(counter_provider, changed_originals)
                counter_classpath += ["--classpath", counter_provider]
            counter_dex = work / (label + "-dex")
            counter_dex.mkdir()
            counter_paths = [p for p in sorted(changed_originals.rglob("*.class")) if "L" + p.relative_to(changed_originals).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--release", "--min-api", "24", *counter_classpath, "--output", counter_dex, *counter_paths], label + "-d8")
            if not excluded:
                annotations = verify.run([verify.dexdump, "-a", "-f", "-h", "-l", "plain", counter_dex / "classes.dex"], label + "-annotations")
                owners = re.findall(r"Class #(\d+)\s+-\n\s*Class descriptor\s*:\s*'Lfixture/Parent;'", annotations)
                if len(owners) != 1:
                    raise RuntimeError("Missing exact class Signature annotation owner")
                blocks = re.findall(r"Class #" + owners[0] + r" annotations:\n(.*?)\nClass #" + owners[0] + r"\s+-", annotations, re.S)
                if len(blocks) != 1 or blocks[0].count("Annotations on class") != 1:
                    raise RuntimeError("Missing exact class Signature annotation block")
                classblock = blocks[0].split("Annotations on class", 1)[1].split("Annotations on ", 1)[0]
                values = re.findall(r"VISIBILITY_SYSTEM Ldalvik/annotation/Signature; value=\{ (.*?) \}", classblock)
                if len(values) != 1:
                    raise RuntimeError("Missing exact class Signature annotation value")
                pattern = r'"(?:[^"\\]|\\.)*"'
                pieces = re.findall(pattern, values[0])
                encoded = "".join(json.loads(piece) for piece in pieces)
                if re.sub(pattern, "", values[0]).strip() or encoded != changed_signature:
                    raise RuntimeError("D8 normalized the intended class Signature control: " + label)
                (work / (label + "-dex-signature.json")).open("x").write(json.dumps({"expected": changed_signature, "encoded": encoded, "status": "exact"}, indent=2) + "\n")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(counter_dex / "classes.dex"), *map(str, counter_classpath), "--timeout", "600", "-o", str(work / (label + "-rejected"))], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-rejection.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or (work / (label + "-rejected")).exists():
                raise RuntimeError("Unproved anonymous closed parent bridge Signature was accepted: " + label)

        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged except exact reported local capture",
                   "all_java_units":len(list((output/"sources").rglob("*.java"))), "minimum_java_release": minimum_java_release}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
