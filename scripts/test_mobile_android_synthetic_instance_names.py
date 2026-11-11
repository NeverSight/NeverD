#!/usr/bin/env python3
"""Verify genuine D8 package-synthetic instance names and original lambda cleanup behavior."""
from __future__ import annotations

import argparse
import copy
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
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--closed-owner", action="store_true")
    parser.add_argument("--generic-owner", action="store_true")
    parser.add_argument("--captured-owner", action="store_true")
    args = parser.parse_args()
    if sum((args.closed_owner, args.generic_owner, args.captured_owner)) > 1:
        parser.error("Choose one declaring-owner profile")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-synthetic-instance-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        cli_hash = hashlib.sha256(args.neverd.read_bytes()).hexdigest()
        (work / "baseline-cli-sha256.txt").open("x").write(cli_hash + "\n")
        fixture = FIXTURES / "synthetic_instance_names/java"
        originals, harness = work / "originals", work / "harness"
        sources = sorted(fixture.rglob("*.java"))
        if args.closed_owner or args.generic_owner or args.captured_owner:
            sources.remove(fixture / "fixture/Core.java")
            variant = "captured_owner" if args.captured_owner else "generic_owner" if args.generic_owner else "closed_owner"
            sources += [fixture.parent / variant / "fixture/Core.java",
                        fixture.parent / "closed_owner/fixture/Scope.java"]
        verify.compile_local(sources, originals, release=8)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("SyntheticInstanceNameCaller", "SyntheticInstanceNameHarness")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes,
                                                                 "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        helper_owner = "Lfixture/Core$Worker;" if args.captured_owner else "Lfixture/Core;"
        original_helper = helper_owner + "->lambda$task$0(Ljava/lang/Runnable;)V"
        if len(all_classes) != (5 if args.captured_owner else 4 if args.closed_owner or args.generic_owner else 3) or len(all_methods) != (10 if args.captured_owner else 8) or original_helper not in all_methods:
            raise RuntimeError("Original javac instance-lambda inventory changed")
        providers = {"Lfixture/Trace;"}
        input_classes = all_classes - (providers if args.dependency_provider else set())
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if args.closed_owner and records["Lfixture/Core;"]["signature"] != "Ljava/lang/Object;Lfixture/Scope<Ljava/lang/Runnable;>;":
            raise RuntimeError("Original closed owner lost its full generic interface contract")
        if args.generic_owner and records["Lfixture/Core;"]["signature"] != "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/Scope<TV;>;":
            raise RuntimeError("Original generic owner lost its complete formal scope")
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x", compression=zipfile.ZIP_DEFLATED) as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            result = []
            for kind in range(2):
                for route in range(4):
                    for mode in range(10):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "SyntheticInstanceNameHarness", str(kind), str(route), str(mode)], f"{label}-{kind}-{route}-{mode}")
                        expected = f"kind={kind},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected,line) for line in output.splitlines()):
                            raise RuntimeError("Original lambda cleanup matrix changed: " + output)
                        result.append(output)
            return result

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        text = ((fixture.parent / "captured_owner/fixture/Core.java") if args.captured_owner else
                (fixture.parent / "generic_owner/fixture/Core.java") if args.generic_owner else
                (fixture.parent / "closed_owner/fixture/Core.java") if args.closed_owner else
                (fixture / "fixture/Core.java")).read_text()
        mutations = {
            "skipped-callback": [("action.run();", "", 1)],
            "repeated-callback": [("action.run();", "action.run(); action.run();", 1)],
            "skipped-cleanup": [("finally { after(); }", "finally {}", 1)],
            "foreign-receiver": [("finally { after(); }", "finally { new Core().after(); }", 1)],
            "hoisted-callback": [("return () -> { try { action.run(); } finally { after(); } };",
                                  "try { action.run(); } finally { after(); } return () -> {};", 1)],
        }
        if args.captured_owner:
            mutations = {label: [(a.replace("after();", "Core.this.after();"), b.replace("after();", "Core.this.after();").replace("new Core().Core.this.after()", "new Core().after()"), n) for a, b, n in changes] for label, changes in mutations.items()}
        for label,replacements in mutations.items():
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            changed = text
            for before,after,count in replacements:
                if changed.count(before) != count: raise RuntimeError("Original lambda counterexample anchor changed")
                changed = changed.replace(before,after)
            wrong.open("x").write(changed)
            compiled = work / (label+"-classes")
            verify.compile_local([wrong],compiled,classpath=originals,release=8)
            result = subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(compiled,originals,harness))),
                                     "SyntheticInstanceNameHarness","1","0","0"],text=True,capture_output=True,
                                     timeout=300,env=verify.environment)
            (work/(label+"-counterexample.log")).open("x").write(result.stdout+result.stderr)
            if result.returncode == 0 or "synthetic instance dispatch changed" not in result.stderr:
                raise RuntimeError("Independent observer did not detect "+label)
        dex = work / "dex"
        dex.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in input_classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        dex_hash = hashlib.sha256(source_dex.read_bytes()).hexdigest()
        (work / "source-dex-sha256.txt").open("x").write(dex_hash + "\n")
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned allocation field chain fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        classes = {row["descriptor"] for row in inventory["classes"]}
        methods = {row["identity"] for row in inventory["methods"]}
        helper_name = "lambda$task$0$fixture-Core" + ("$Worker" if args.captured_owner else "")
        mangled = helper_owner + "->" + helper_name + "(Ljava/lang/Runnable;)V"
        adapter = helper_owner[:-1] + "$$ExternalSyntheticLambda0;"
        adapter_constructor = adapter + "-><init>(" + helper_owner + "Ljava/lang/Runnable;)V"
        adapter_run = adapter + "->run()V"
        expected_methods = {m for m in all_methods if m.split("->",1)[0] in input_classes} - {original_helper}
        expected_methods |= {mangled, adapter_constructor, adapter_run}
        if classes != input_classes | {adapter} or methods != expected_methods:
            raise RuntimeError("D8 changed the independent lambda class/method transformation")
        by_method = {row["identity"]: row for row in inventory["methods"]}
        if (by_method[mangled]["access_flags"] != 0x1000 or by_method[mangled]["section"] != "Virtual methods"
                or by_method[adapter_constructor]["access_flags"] != 0x11001 or by_method[adapter_run]["access_flags"] != 0x11):
            raise RuntimeError("D8 changed the package-synthetic implementation or adapter access witness")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-lambda-disassembly")
        bodies = {}
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if name: bodies[(block[1],name[1])] = block[2]
        helper = bodies[(helper_owner, helper_name)]
        forwarded = bodies[(adapter, "run")]
        if (helper.count("Ljava/lang/Runnable;.run:") != 1 or helper.count("Lfixture/Core;.after:") != 2
                or "move-exception" not in helper or "throw" not in helper
                or forwarded.count(helper_owner + "." + helper_name + ":") != 1
                or "invoke-virtual" not in forwarded):
            raise RuntimeError("D8 changed the original callback/finally and virtual forwarding witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity,row in rows.items() if row["status"] == "source-projected"}
        expected_projected = {mangled, adapter_constructor}
        if args.captured_owner:
            expected_projected |= {m for m in methods if m.split("->", 1)[0] == helper_owner}
            bindings = coverage.get("class_source_bindings", [])
            capture = helper_owner + "->this$0:Lfixture/Core;"
            if (len(bindings) != 1 or bindings[0].get("class") != helper_owner
                    or bindings[0].get("binding_kind") != "inner-to-static-member"
                    or bindings[0].get("outer_capture_field") != capture
                    or bindings[0].get("source_enclosing_class") != "Lfixture/Core;"
                    or bindings[0].get("source_name") != "Worker"
                    or bindings[0].get("source_visibility") != "package"
                    or bindings[0].get("package_access_callers") != [adapter_run]
                    or not any("private InnerClasses annotation" in text for text in bindings[0].get("metadata_differences", []))):
                raise RuntimeError("Captured member lost its original enclosing identity or field")
            projections = coverage.get("generic_signature_projections", [])
            signature = records[helper_owner]["signature"]
            expected_tokens = sorted((m.start(), "Lfixture/Core;::" + m[1]) for m in re.finditer(r"T([KV]);", signature))
            if (len(projections) != 1 or projections[0].get("declaration") != helper_owner
                    or projections[0].get("declaration_kind") != "class"
                    or projections[0].get("original_signature") != signature
                    or len(expected_tokens) != 2
                    or sorted((c["offset"], c["original_type"]) for c in projections[0]["changes"]) != expected_tokens
                    or any(c["kind"] != "static-inner-type-variable" or c["source_type"] != "Ljava/lang/Object;"
                           for c in projections[0]["changes"])):
                raise RuntimeError("Captured member changed a contract beyond its proved outer formals")
        elif coverage.get("class_source_bindings") or coverage.get("generic_signature_projections"):
            raise RuntimeError("Independent synthetic owner acquired an unrelated source projection")
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != len(expected_projected)
                or projected != expected_projected or coverage["status"] != "partial"):
            raise RuntimeError("Synthetic instance recovery changed runtime entries or unrelated projections")
        row = rows[mangled]
        source_name = row.get("source_name", "")
        source_identity = helper_owner + "->" + source_name + "(Ljava/lang/Runnable;)V"
        if (row.get("projection_kind") != "virtual-method-identifier" or row.get("source_identity") != source_identity
                or row.get("source_name_declaration") != mangled or row.get("source_name_family") != [mangled]
                or len(row.get("metadata_differences",[])) != 3
                or not any("synthetic" in text for text in row["metadata_differences"])
                or not re.fullmatch(r"__neverdVirtualMethod[0-9]+",source_name)
                or rows[adapter_constructor].get("projection_kind") != "synthetic-constructor"
                or rows[adapter_constructor].get("source_identity") != adapter_constructor
                or len(rows[adapter_constructor].get("metadata_differences", [])) != 1
                or "synthetic access flag" not in rows[adapter_constructor]["metadata_differences"][0]):
            raise RuntimeError("Original synthetic instance name and flag changes lack their exact source receipt")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if owner != "Lfixture/Scope;"}
        mapped = {source_identity if m == mangled else m for m in methods}
        if actual_classes != classes or set(actual_methods) != mapped | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Synthetic instance recovery omitted or invented a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in input_classes:
            before, after = copy.deepcopy(records[owner]), actual_records[owner]
            if args.captured_owner and owner == helper_owner:
                before["inner_class"]["access"] = (before["inner_class"]["access"] & ~2) | 8
                before["signature"] = before["signature"].replace("TK;", "Ljava/lang/Object;").replace("TV;", "Ljava/lang/Object;")
                if before["fields"][capture]["access"] != 0x1010:
                    raise RuntimeError("Original enclosing field flags changed")
                before["fields"][capture]["access"] &= ~0x1000
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original class metadata changed: " + owner + ":" + key)
            for method, metadata in before["methods"].items():
                wanted = copy.deepcopy(metadata)
                target = method
                if method == original_helper:
                    target = source_identity
                    wanted["name"] = source_name
                    # D8 independently proved private-to-package access; Java
                    # source then drops only the disclosed synthetic flag.
                    wanted["access"] = 0
                if after["methods"].get(target) != wanted or actual_exceptions.get(target) != exceptions[method]:
                    raise RuntimeError("Method metadata changed beyond the recorded D8/name/flag differences: " + method)
        adapter_record = actual_records[adapter]
        if (adapter_record["access"] != 0x31 or adapter_record["superclass"] != "Ljava/lang/Object;"
                or adapter_record["interfaces"] != ["Ljava/lang/Runnable;"] or len(adapter_record["fields"]) != 2
                or adapter_record["methods"][adapter_constructor]["access"] != 1
                or adapter_record["methods"][adapter_run]["access"] != 0x11):
            raise RuntimeError("D8 adapter shape or source access changed")
        for name,kind in [("f$0",helper_owner),("f$1","Ljava/lang/Runnable;")]:
            field=adapter_record["fields"][adapter+"->"+name+":"+kind]
            if field["access"] != 0x11 or field["signature"] is not None or field["descriptor"] != kind:
                raise RuntimeError("Original D8 capture field changed")
        (work/"compiler-differences.json").open("x").write(json.dumps({
            "javac_to_d8": {"original_private_helper": original_helper, "package_synthetic_helper": mangled,
                              "adapter": adapter, "adapter_constructor": adapter_constructor},
            "d8_to_source": {"helper_name_and_synthetic_flag": source_identity,
                              "adapter_synthetic_constructor": adapter_constructor,
                              "adapter_class_and_capture_synthetic_flags": "not expressible in Java source"}},indent=2)+"\n")
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Allocation field chain call changed an original effect or result")
        if (hashlib.sha256(args.neverd.read_bytes()).hexdigest() != cli_hash
                or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash
                or hashlib.sha256(source_dex.read_bytes()).hexdigest() != dex_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status":"success","class_count":len(classes),"method_count":len(methods),
                    "fresh_jvm_cases":len(baseline),"attempts_per_case":2,"dependency_provider":args.dependency_provider,
                    "closed_owner":args.closed_owner,"generic_owner":args.generic_owner,"captured_owner":args.captured_owner,
                    "genuine_d8_package_synthetic_instance_name":"independently witnessed",
                    "inventories_and_unprojected_metadata":"unchanged","compiler_name_and_synthetic_flag_differences":"recorded",
                    "captured_identity_subclass_dispatch_and_finally_exceptions":"matched",
                    "independent_skipped_repeated_foreign_and_hoisted_counterexamples":"detected","input_and_oracle_bytes":"unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
