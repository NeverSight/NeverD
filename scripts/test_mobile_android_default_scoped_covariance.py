#!/usr/bin/env python3
"""Verify raw Kotlin callback covariance through a proven generic default method scope."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
    from test_mobile_android_protected_array_callbacks import change_method_flags


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--class-lambdas", action="store_true")
    parser.add_argument("--input-runtime", action="store_true")
    parser.add_argument("--indexed", action="store_true")
    parser.add_argument("--plain-indexed", action="store_true")
    args = parser.parse_args()
    args.indexed |= args.plain_indexed
    if args.indexed and args.class_lambdas:
        parser.error("the indexed profile preserves its raw Object superclass")
    function = "Function2" if args.indexed else "Function1"
    bridge_descriptor = "(" + "Ljava/lang/Object;" * (2 if args.indexed else 1) + ")Ljava/lang/Object;"
    target_descriptor = "(" + ("I" if args.indexed else "") + "Ljava/lang/Object;)Ljava/lang/Void;"
    target_signature = "(" + ("I" if args.indexed else "") + "TK;)Ljava/lang/Void;"
    if args.plain_indexed:
        target_signature = None
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-default-scoped-covariance-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / ("raw_indexed_scope" if args.indexed else "default_scoped_covariance")
        if args.plain_indexed:
            copied = work / "plain-fixture"
            shutil.copytree(fixture, copied)
            fixture = copied
            source = fixture / "kotlin/fixture/Callbacks.kt"
            text = source.read_text()
            if text.count("inline fun <K : Any> make") != 1 or text.count("(Int, K)") != 1:
                raise RuntimeError("Original plain-indexed declaration changed")
            source.write_text(text.replace("inline fun <K : Any> make", "inline fun make").replace("(Int, K)", "(Int, Any)"))
            observer = fixture / "harness/DefaultScopedCovarianceHarness.java"
            text = observer.read_text()
            begin = text.index("        require(formals.length == 1")
            end = text.index("        Object index =", begin)
            text = text[:begin] + '        require(formals.length == 0, "original unscoped method");\n' + text[end:]
            observer.write_text(text)
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        dependencies = []
        for jar in (stdlib, kotlin / "annotations-13.0.jar"):
            output = work / jar.name
            with zipfile.ZipFile(jar) as source, zipfile.ZipFile(output, "x") as dest:
                for entry in source.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        dest.writestr(entry.filename, source.read(entry))
            dependencies.append(output)
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", *( ["-Xlambdas=class"] if args.class_lambdas else [])]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        if args.input_runtime:
            # Include exact compiler-distributed classes as DEX input. Their
            # method bodies and metadata are never patched for this profile.
            with zipfile.ZipFile(stdlib) as runtime:
                for name in ("kotlin/Function", "kotlin/jvm/functions/" + function,
                             "kotlin/jvm/internal/FunctionBase", "kotlin/jvm/internal/Lambda"):
                    destination = originals / (name + ".class")
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with destination.open("xb") as output:
                        output.write(runtime.read(name + ".class"))
        # javac hides synthetic methods. A separate declaration view changes
        # only that access bit, preserving all Code and runtime descriptors;
        # frozen caller bytes execute against original/rebuilt classes only.
        view = work / "caller-declarations"
        shutil.copytree(originals, view)
        receipt = change_method_flags(view / "fixture/Callbacks.class",
            f"(Lkotlin/jvm/functions/{function};ILjava/lang/Object;)V".encode(), 0x1009, 0x9, name=b"make$default")
        (work / "frozen-caller-declaration-view.json").write_text(json.dumps(receipt, indent=2) + "\n")
        verify.compile_local(sorted((fixture / "harness").glob("*.java")), harness,
                             classpath=os.pathsep.join(map(str, (view, *dependencies))), release=17)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classes, methods = compiler_inventory(verify, originals)
        methods = set(methods)

        def facts(root):
            return {row["name"]: row for path in sorted(root.rglob("*.class"))
                    for row in [KotlinFixtureClassFile(path.read_bytes()).facts()]}

        records, exceptions = facts(originals), exception_inventory(originals)
        locals_ = {owner for owner in classes if owner.startswith("Lfixture/Callbacks$")}
        local = next((owner for owner in locals_ if owner + "->invoke" + target_descriptor in methods), None)
        if local is None or records[local]["signature"] is not None:
            raise RuntimeError("Kotlin changed the original raw callback class")
        bridge_id = local + "->invoke" + bridge_descriptor
        target_id = local + "->invoke" + target_descriptor
        base_id = f"Lfixture/Callbacks;->make(Lkotlin/jvm/functions/{function};)V"
        default_id = f"Lfixture/Callbacks;->make$default(Lkotlin/jvm/functions/{function};ILjava/lang/Object;)V"
        if (records[local]["methods"][target_id]["signature"] != target_signature
                or records[local]["methods"][bridge_id]["access"] != 0x1041
                or records["Lfixture/Callbacks;"]["signature"] is not None
                or records["Lfixture/Callbacks;"]["methods"][default_id]["signature"] is not None):
            raise RuntimeError("Kotlin changed the witnessed generic method scope")
        expected_classes = 8 if args.input_runtime else 4
        expected_methods = 15 if args.input_runtime else 10
        if len(classes) != expected_classes or len(methods) != expected_methods:
            raise RuntimeError("Owned default scoped covariance inventory changed")

        def execute(runtime, label, flags):
            result = []
            for route in range(6):
                for mode in range(16 if args.indexed else 10):
                    if route in (4, 5) and mode in ((15,) if args.indexed else (1, 3, 6)):
                        continue
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        "DefaultScopedCovarianceHarness", str(route), str(mode), str(args.class_lambdas).lower(), flags],
                        f"{label}-{route}-{mode}")
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(
                            f"route={route},mode={mode},attempt=[01],trace=[0-9]+", line) for line in output.splitlines()):
                        raise RuntimeError("Original default scoped observer matrix changed")
                    result.append(output)
            return result

        baseline = execute((originals, harness), "baseline", "original")
        classpath = ["--classpath", args.android_jar.resolve()]
        for jar in dependencies:
            classpath += ["--classpath", jar]
        dex_dir = work / "dex"; dex_dir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath,
                    "--output", dex_dir, *sorted(originals.rglob("*.class"))], "d8")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned default scope changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original default scoped entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "sdk-bridge-code")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            descriptor = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] == local and name and name[1] == "invoke" and descriptor and descriptor[1] == bridge_descriptor:
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                if args.indexed:
                    copies = {"move", "move/from16", "move/16", "move-object", "move-object/from16", "move-object/16"}
                    code = [(op.removesuffix("/range"), operands) for op, operands in ops if op not in copies]
                    if ([op for op, _ in code] != ["check-cast", "invoke-virtual", "move-result", "invoke-virtual", "move-result-object", "return-object"]
                            or "Ljava/lang/Number;" not in code[0][1] or "intValue:()I" not in code[1][1]
                            or target_descriptor not in code[3][1]):
                        raise RuntimeError("D8 changed the original Number conversion and typed target")
                elif len(ops) != 3 or not ops[0][0].startswith("invoke-virtual") or ops[1][0] != "move-result-object" or ops[2][0] != "return-object":
                    raise RuntimeError("D8 changed original exact covariant bridge body")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing original default scoped bridge witness")
        annotation_dump = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotation_dump)
        if "VISIBILITY_BUILD" in annotation_dump or visible != ["Lkotlin/Metadata;"] * (7 if args.input_runtime else 3):
            raise RuntimeError("D8 annotation retention changed")
        dex_records, dropped = copy.deepcopy(records), {}
        for owner in classes:
            record = dex_records[owner]
            for identity, row in [(owner, record), *record["fields"].items(), *record["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        (work / "d8-annotation-differences.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output, "--timeout", "600"], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        bindings, projections = coverage.get("class_source_bindings", []), coverage.get("generic_signature_projections", [])
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        expected_projected = {identity for identity in methods if identity.split("->", 1)[0] in locals_} | {base_id, default_id}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or projected != expected_projected
                or len(bindings) != 2 or {row["class"] for row in bindings} != locals_
                or any(row["binding_kind"] != "anonymous-to-shared-member" for row in bindings)
                or not (not projections if args.plain_indexed else
                    len(projections) == 1 and projections[0]["declaration"] == target_id
                    and projections[0]["original_signature"] == target_signature
                    and projections[0]["changes"] == [{"kind": "synthetic-default-method-type-variable", "offset": 2 if args.indexed else 1,
                        "original_type": default_id + "::K", "source_type": "Ljava/lang/Object;", "scope_origin": base_id}])):
            raise RuntimeError("Default scoped covariance lacks its exact original scope receipts")
        if rows[bridge_id].get("bridge_target") != target_id or rows[bridge_id].get("projection_kind") != ("raw-indexed-result-bridge" if args.plain_indexed else "raw-indexed-scope-bridge" if args.indexed else "generic-interface-bridge"):
            raise RuntimeError("Original default scoped bridge mapping changed")
        if args.indexed and (rows[bridge_id].get("unboxed_parameters") != [0]
                or rows[bridge_id].get("unbox_method") != "Ljava/lang/Number;->intValue()I"):
            raise RuntimeError("Original indexed Number conversion receipt changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8),
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        after, after_exceptions = facts(rebuilt), exception_inventory(rebuilt)
        renamed, binding_map = {}, {row["class"]: row for row in bindings}
        for owner, binding in binding_map.items():
            candidates = [name for name, row in after.items() if (row.get("inner_class") or {}).get("name") == binding["source_name"]
                          and (row.get("inner_class") or {}).get("outer") == "Lfixture/Callbacks;"]
            if len(candidates) != 1:
                raise RuntimeError("Scope receipt does not identify one static member")
            renamed[owner] = candidates[0]
        def mapped(text):
            for before, current in renamed.items(): text = text.replace(before, current)
            return text
        helpers = {mapped(r["class"] + "->" + r["name"] + r["prototype"]) for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes if any(row["code"] for row in records[owner]["methods"].values())}
        expected_helpers.add("Lfixture/Callbacks;-><init>()V")
        if actual_classes != {mapped(owner) for owner in classes} or set(actual_methods) != {mapped(identity) for identity in methods} | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Default scoped covariance invented or lost an entry")
        for owner in classes:
            before, current = dex_records[owner], after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != current.get(key):
                    raise RuntimeError("Original default scoped class metadata changed: " + owner + ":" + key)
            wanted_inner, wanted_enclosing = copy.deepcopy(before["inner_class"]), before["enclosing_method"]
            if owner in locals_:
                wanted_inner = {"inner": renamed[owner], "outer": "Lfixture/Callbacks;", "name": binding_map[owner]["source_name"], "access": 0x18 | (before["access"] & 1)}
                wanted_enclosing = None
            if current["inner_class"] != wanted_inner or current["enclosing_method"] != wanted_enclosing:
                raise RuntimeError("Default scoped class identity changed beyond its receipt")
            fields = {mapped(identity): dict(metadata) for identity, metadata in before["fields"].items()}
            for metadata in fields.values(): metadata["descriptor"] = mapped(metadata["descriptor"])
            if current["fields"] != fields:
                raise RuntimeError("Original default scoped fields changed")
            for identity, metadata in before["methods"].items():
                wanted = copy.deepcopy(metadata); wanted["prototype"] = mapped(wanted["prototype"])
                if identity == default_id: wanted["access"] &= ~0x1000
                if args.indexed and identity == bridge_id: wanted["access"] &= ~0x1040
                if identity == target_id: wanted["signature"] = None
                if current["methods"].get(mapped(identity)) != wanted or after_exceptions[mapped(identity)] != exceptions[identity]:
                    raise RuntimeError("Original default scoped method metadata changed: " + identity)
        if execute((rebuilt, harness), "rebuilt", "source") != baseline:
            raise RuntimeError("Original default scoped runtime behavior changed")
        text = (fixture / "kotlin/fixture/Callbacks.kt").read_text()
        value = "value" if args.indexed else "it"
        mutations = (
            ("wrong-result", text.replace(f"Trace.seen = {value}; null", f"Trace.seen = {value}; {value}"), "0"),
            ("lost-effect", text.replace("Trace.hit(3);", ""), "0"),
            ("wrong-identity", text.replace(f"Trace.seen = {value};", "Trace.seen = Any();"), "0"),
            ("reordered-factory", text.replace("Trace.callback = callback", "").replace("stage.run()", "stage.run(); Trace.callback = callback"), "14" if args.indexed else "4"),
        )
        if args.indexed:
            mutations += (("wrong-index", text.replace("Trace.index = index;", "Trace.index = index + 1;"), "1"),)
        for label, source, mode in mutations:
            if source == text: raise RuntimeError("Default scope mutation anchor changed")
            changed = work / label / "Callbacks.kt"; changed.parent.mkdir(); changed.write_text(source)
            wrong = work / (label + "-classes"); wrong.mkdir()
            verify.run([*compiler, "-d", wrong, changed], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                "DefaultScopedCovarianceHarness", "1", mode, str(args.class_lambdas).lower(), "original"],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "default scoped covariance contract" not in result.stderr:
                raise RuntimeError("Independent observer missed " + label)
        if args.indexed:
            wrong = work / "narrowed-number"
            callback_file = local[1:-1] + ".class"
            target = wrong / callback_file; target.parent.mkdir(parents=True)
            data = (originals / callback_file).read_bytes()
            before, after = b"java/lang/Number", b"java/lang/Integer"
            old = b"\x01" + len(before).to_bytes(2, "big") + before
            new = b"\x01" + len(after).to_bytes(2, "big") + after
            if data.count(old) != 1:
                raise RuntimeError("Missing independent Number narrowing anchor")
            target.write_bytes(data.replace(old, new))
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness, *dependencies))),
                "DefaultScopedCovarianceHarness", "0", "1", "false", "original"], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / "narrowed-number-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "default scoped covariance contract" not in result.stderr:
                raise RuntimeError("Independent observer missed Integer narrowing")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != value for p, value in dependency_hashes.items())):
            raise RuntimeError("Original compiler, SDK, inputs or frozen callers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "class_lambdas": args.class_lambdas,
                    "input_runtime": args.input_runtime, "indexed": args.indexed, "plain_indexed": args.plain_indexed,
                    "original_generic_default_scope": "absent" if args.plain_indexed else "shared proof retained",
                    "original_metadata_and_exceptions": "reported placement and default/bridge flags; exact original scope receipts",
                    "both_callback_descriptors": "retained", "bridge_flags": "reported source flags" if args.indexed else "retained", "d8_class_annotation_loss": "independently verified",
                    "frozen_callers_singleton_identity_nulls_and_partial_effects": "matched", "independent_counterexamples": len(mutations) + int(args.indexed),
                    "original_inputs_and_oracles": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
