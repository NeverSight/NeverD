#!/usr/bin/env python3
"""Verify genuine Kotlin private names, exact direct dispatch and wide argument words."""
from __future__ import annotations

import argparse
from collections import Counter
from contextlib import nullcontext
from copy import deepcopy
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
    from scripts.test_mobile_android_synthetic_method_names import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import compiler_facts


PRIVATE = "Lfixture/Provider;->combine-value(JJ)J"
PUBLIC = "Lfixture/Child;->combine-value(JJ)J"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--synthetic", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-instance-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "private_instance_names"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file():
            raise RuntimeError("The original witness requires the unmodified Kotlin compiler distribution")
        inputs = [args.android_jar.resolve(), *compiler_jars]
        input_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
        stdlib = kotlin / "kotlin-stdlib.jar"
        dependencies = []
        for source in (stdlib, kotlin / "annotations-13.0.jar"):
            dest = work / source.name
            with zipfile.ZipFile(source) as before, zipfile.ZipFile(dest, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(dest)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        text = (fixture / "kotlin/fixture/Provider.kt").read_text()
        if args.synthetic:
            text = text.replace('    @JvmName("combine-value")\n    private', '    @JvmSynthetic @JvmName("combine-value")\n    private')
        source = work / "authored/Provider.kt"
        source.parent.mkdir()
        source.write_text(text)
        originals.mkdir()
        compiler = [verify.java, "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect",
                    "-classpath", os.pathsep.join(map(str, (stdlib, original_java))), "-jvm-target", "1.8"]
        verify.run([*compiler, "-d", originals, source], "kotlinc-original")
        for java in sorted(original_java.rglob("*.class")):
            dest = originals / java.relative_to(original_java)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(java, dest)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("PrivateInstanceNamesHarness", "PrivateInstanceNamesCaller")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        classes, methods = compiler_inventory(verify, originals)
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if len(classes) != 3 or len(methods) != 9 or PRIVATE not in methods or PUBLIC not in methods:
            raise RuntimeError("Original private instance inventory changed: " + repr((len(classes), len(methods))))
        if (records["Lfixture/Provider;"]["methods"][PRIVATE]["access"] != (0x1012 if args.synthetic else 0x12)
                or records["Lfixture/Child;"]["methods"][PUBLIC]["access"] != 0x11
                or records["Lfixture/Provider;"]["signature"] != "<T:Ljava/lang/Object;>Ljava/lang/Object;"):
            raise RuntimeError("Original private/public flags or independent generic owner changed")

        def execute(runtime, private_name, public_name, label):
            outputs = []
            for route in range(4):
                for kind in range(2):
                    for mode in range(9):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                            "PrivateInstanceNamesHarness", str(route), str(kind), str(mode), private_name, public_name], f"{label}-{route}-{kind}-{mode}")
                        expected = f"route={route},kind={kind},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original private instance matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "combine-value", "combine-value", "baseline")
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]
        dex_dir = work / "dex"
        dex_dir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir,
                    *sorted(originals.rglob("*.class"))], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned private instance changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original private instance runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations))
        if "VISIBILITY_BUILD" in annotations or visible != Counter({"Lkotlin/Metadata;": 2}):
            raise RuntimeError("D8 changed independently read runtime metadata retention")
        dex_records, dropped = deepcopy(records), {}
        for c in classes:
            facts = dex_records[c]
            for identity, row in [(c, facts), *facts["fields"].items(), *facts["methods"].items()]:
                for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                    if row.get(key):
                        dropped.setdefault(identity, {})[key] = row[key]
                    row[key] = []
        if set(dropped) != {PRIVATE, PUBLIC}:
            raise RuntimeError("Compiler CLASS annotation loss changed its exact private/public sites")
        (work / "d8-class-annotation-loss.json").write_text(json.dumps(dropped, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {identity for identity, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial" or projected != {PRIVATE, PUBLIC}
                or coverage["projected_method_count"] != 2 or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Private instance recovery changed original inventory or unrelated contracts")
        if (rows[PRIVATE].get("projection_kind") != ("synthetic-private-method-identifier" if args.synthetic else "private-method-identifier")
                or rows[PUBLIC].get("projection_kind") != "virtual-method-identifier"
                or rows[PRIVATE]["source_name"] != "__neverdPrivateMethod1"
                or rows[PRIVATE]["source_name"] == rows[PUBLIC]["source_name"]
                or set(rows[PUBLIC].get("source_name_family", [])) != {PUBLIC}
                or any(not rows[identity].get("metadata_differences") for identity in (PRIVATE, PUBLIC))):
            raise RuntimeError("Private lexical name contaminated the unrelated public virtual family")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=8)
        mappings = {identity: rows[identity]["source_identity"] for identity in (PRIVATE, PUBLIC)}
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != {mappings.get(m, m) for m in methods} | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Private instance recovery invented or omitted a runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            before, actual = dex_records[c], after[c]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != actual.get(key):
                    raise RuntimeError("Undisclosed private instance class metadata changed: " + c + ":" + key)
            for identity, metadata in before["methods"].items():
                selected, expected = mappings.get(identity, identity), deepcopy(metadata)
                if identity in mappings:
                    expected["name"] = rows[identity]["source_name"]
                    expected["access"] &= ~0x1000
                if actual["methods"].get(selected) != expected or after_exceptions.get(selected) != exceptions[identity]:
                    raise RuntimeError("Undisclosed private instance method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness), rows[PRIVATE]["source_name"], rows[PUBLIC]["source_name"], "rebuilt") != baseline:
            raise RuntimeError("Private direct dispatch changed receiver, wide arguments, partial writes or failures")
        mutations = (("wrong-seed", "seed += first", "seed += 1"),
                     ("discarded-result", "Trace.result = result", "Trace.result = 0"),
                     ("repeated-target", "= combine(first, second)", "= combine(first, second).let { combine(first, second) }"))
        for label, old, new in mutations:
            if text.count(old) != 1:
                raise RuntimeError("Independent private instance counterexample anchor changed")
            wrong = work / label / "Provider.kt"
            wrong.parent.mkdir()
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            compiled.mkdir()
            verify.run([*compiler, "-d", compiled, wrong], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness, *dependencies))),
                                    "PrivateInstanceNamesHarness", "1", "1", "0", "combine-value", "combine-value"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned private instance name contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies} != dependency_hashes
                or {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs} != input_hashes):
            raise RuntimeError("Original inputs, immutable callers or compiler bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "synthetic": args.synthetic,
                    "fresh_jvm_cases_per_stage": len(baseline), "stages": 2, "attempts_per_case": 2,
                    "private_direct_and_public_virtual_names": "distinct", "wide_wrapping_and_receiver_state": "matched",
                    "independent_compiler_counterexamples": len(mutations), "partial_writes_and_failure_identity": "matched",
                    "source_name_and_synthetic_flag_changes": "disclosed", "other_metadata_and_exceptions": "unchanged",
                    "compiler_CLASS_annotation_loss_sites": len(dropped), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
