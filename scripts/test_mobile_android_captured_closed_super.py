#!/usr/bin/env python3
"""Verify captured coroutine fields compose with a closed superclass contract."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_synthetic_method_names import KotlinFixtureClassFile
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile

OWNER = "Lfixture/Factory;"
LOCAL = "Lfixture/Factory$make$1;"
FACTORY = OWNER + "->make-impl(Ljava/util/concurrent/atomic/AtomicReference;Lkotlin/jvm/functions/Function1;Lkotlin/jvm/functions/Function2;)Lkotlin/jvm/functions/Function2;"
CTOR = LOCAL + "-><init>(Lkotlin/jvm/functions/Function1;Ljava/util/concurrent/atomic/AtomicReference;Lkotlin/jvm/functions/Function2;Lkotlin/coroutines/Continuation;)V"


class CoroutineClassFile(KotlinFixtureClassFile):
    VISIBLE_ANNOTATIONS = KotlinFixtureClassFile.VISIBLE_ANNOTATIONS | {"Lkotlin/coroutines/jvm/internal/DebugMetadata;"}
    SCHEMAS = {**KotlinFixtureClassFile.SCHEMAS,
               "Lkotlin/coroutines/jvm/internal/DebugMetadata;":
               {"c": "s", "f": "s", "i": "[I", "l": "[I", "m": "s", "n": "[s", "s": "[s"}}


def compiler_facts(root):
    result = {}
    for path in sorted(root.rglob("*.class")):
        row = CoroutineClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix() != row["name"][1:-1] + ".class":
            raise RuntimeError("Duplicate or misplaced compiler definition")
        result[row["name"]] = row
    return result


def verify_metadata(coverage, records, exceptions, rebuilt):
    after = compiler_facts(rebuilt)
    rows = {r["identity"]: r for r in coverage["methods"]}
    bindings = coverage.get("class_source_bindings", [])
    if (len(bindings) != 1 or bindings[0].get("class") != LOCAL
            or bindings[0].get("binding_kind") != "anonymous-to-named-local"
            or not bindings[0].get("metadata_differences")
            or bindings[0].get("source_enclosing_method", {}).get("identity") != rows[FACTORY].get("source_identity")):
        raise RuntimeError("Missing exact coroutine source ownership and naming disclosure")
    binding = bindings[0]
    candidates = [c for c, r in after.items() if r["inner_class"]
                  and r["inner_class"]["name"] == binding["source_name"]
                  and r["enclosing_method"] == {k: binding["source_enclosing_method"][k] for k in ("owner", "name", "prototype")}]
    if len(candidates) != 1:
        raise RuntimeError("Recompiled coroutine has no unique disclosed class binding")
    renamed = candidates[0]
    rename = lambda text: text.replace(LOCAL, renamed) if isinstance(text, str) else text
    aliases = {}
    for owner, record in records.items():
        for identity, method in record["methods"].items():
            row = rows[identity]
            if "-" in method["name"]:
                if (not row.get("source_name") or not row.get("metadata_differences")
                        or row.get("source_identity") != owner + "->" + row["source_name"] + method["prototype"]):
                    raise RuntimeError("Missing exact original method identifier projection")
                aliases[identity] = row["source_identity"]
    helpers = {rename(r["class"] + "->" + r["name"] + r["prototype"])
               for r in coverage.get("generated_source_helpers", [])}
    expected_helpers = {rename(c) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in (OWNER, LOCAL)}
    expected_methods = {rename(aliases.get(m, m)) for r in records.values() for m in r["methods"]}
    if (set(after) != {rename(c) for c in records} or helpers != expected_helpers
            or {m for r in after.values() for m in r["methods"]} != expected_methods | helpers):
        raise RuntimeError("Recompiled coroutine omitted or invented a runtime definition/helper")
    expected_projections = {CTOR}
    expected_projections.update(k for k, v in records[LOCAL]["fields"].items() if "TT;" in (v["signature"] or ""))
    expected_projections.update(k for k, v in records[OWNER]["methods"].items()
                                if "TT;" in (v["signature"] or "") and k in aliases
                                and not v["signature"].startswith("<T:"))
    projections = {r["declaration"]: r for r in coverage.get("generic_signature_projections", [])}
    if set(projections) != expected_projections or len(projections) != 9:
        raise RuntimeError("Static carrier scope projections changed")
    signatures = {}
    for identity, receipt in projections.items():
        table = records[identity.split("->", 1)[0]]
        original = (table["methods"] if receipt["declaration_kind"] == "method" else table["fields"])[identity]["signature"]
        offsets = [match.start() for match in re.finditer("TT;", original)]
        changes = receipt["changes"]
        if (receipt.get("original_signature") != original or not receipt.get("metadata_differences")
                or sorted(c["offset"] for c in changes) != offsets
                or any(c["kind"] != "static-carrier-type-variable" or c.get("scope_origin") != OWNER
                       or c["original_type"] != OWNER + "::T" or c["source_type"] != "Ljava/lang/Object;" for c in changes)):
            raise RuntimeError("Carrier projection lost its exact original scope and substitution")
        result = original
        for offset in reversed(offsets):
            result = result[:offset] + "Ljava/lang/Object;" + result[offset + 3:]
        signatures[identity] = result
    actual_exceptions = exception_inventory(rebuilt)
    annotation_loss = {}
    for owner, record in records.items():
        wanted = deepcopy(record)
        for context, metadata in [(owner, wanted), *wanted["fields"].items(), *wanted["methods"].items()]:
            for key in ("runtime_invisible_annotations", "runtime_invisible_parameter_annotations"):
                if metadata.get(key):
                    annotation_loss.setdefault(context, {})[key] = metadata[key]
                metadata[key] = []
        wanted["signature"] = rename(wanted["signature"])
        if owner == LOCAL:
            wanted["inner_class"] = {"inner": renamed, "outer": None, "name": binding["source_name"], "access": 0x10}
            wanted["enclosing_method"]["name"] = rows[FACTORY]["source_name"]
        fields = {}
        for identity, field in wanted["fields"].items():
            field["descriptor"] = rename(field["descriptor"])
            field["signature"] = rename(signatures.get(identity, field["signature"]))
            if owner == LOCAL and field["access"] & 0x1000:
                if field["name"] not in {"L$0", "$seed", "$arg0", "$operation"}:
                    raise RuntimeError("Unexpected synthetic coroutine field")
                field["access"] &= ~0x1000
            fields[rename(identity)] = field
        wanted["fields"] = fields
        for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                    "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
            if after[rename(owner)].get(key) != wanted.get(key):
                raise RuntimeError("Undisclosed coroutine class metadata change: " + owner + ":" + key)
        for identity, method in wanted["methods"].items():
            target = rename(aliases.get(identity, identity))
            if identity in aliases:
                method["name"] = rows[identity]["source_name"]
            method["prototype"] = rename(method["prototype"])
            method["signature"] = rename(signatures.get(identity, method["signature"]))
            if method["access"] & 0x1000 and rows[identity].get("projection_kind") != "generic-interface-bridge":
                if not rows[identity].get("metadata_differences"):
                    raise RuntimeError("Missing synthetic method flag disclosure")
                method["access"] &= ~0x1000
            if after[rename(owner)]["methods"].get(target) != method or actual_exceptions.get(target) != exceptions[identity]:
                raise RuntimeError("Undisclosed coroutine method metadata or Exceptions change: " + identity)
    return {"original": records, "rebuilt": after, "d8_class_annotation_loss": annotation_loss}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--release-dex", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-captured-closed-super-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "captured_closed_super/kotlin/fixture/Factory.kt"
        kotlin = args.kotlin_lib.resolve()
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        input_hashes = {str(p): sha(p) for p in inputs}
        dependencies = []
        for name in ("kotlin-stdlib.jar", "annotations-13.0.jar"):
            target = work / name
            with zipfile.ZipFile(kotlin / name) as before, zipfile.ZipFile(target, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe compiler dependency entry")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(target)
        dependency_hashes = {str(p): sha(p) for p in dependencies}
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*",
                    "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect", "-jvm-target", "1.8",
                    "-classpath", kotlin / "kotlin-stdlib.jar"]
        originals, harness = work / "originals", work / "harness"
        verify.run([*compiler, "-d", originals, fixture], "kotlinc-original")
        verify.compile_local([FIXTURES / "harness/CapturedClosedSuperHarness.java"], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        if (len(classes), len(methods)) != (3, 18):
            raise RuntimeError("Original coroutine definition inventory changed")
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if records[LOCAL]["methods"][CTOR]["signature"] is None or records[OWNER]["methods"][FACTORY]["signature"] is None:
            raise RuntimeError("Original generic scope or captured constructor Signature disappeared")

        def execute(runtime, label, name):
            outputs = []
            for route in range(4):
                for mode in range(15):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                                         "CapturedClosedSuperHarness", str(route), str(mode), name], f"{label}-{route}-{mode}")
                    expected = f"route={route},mode={mode},attempt=[01],events=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Frozen coroutine observer changed: " + output)
                    outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline", "make-impl")
        classpath = ["--classpath", args.android_jar.resolve()]
        for target in dependencies:
            classpath += ["--classpath", target]
        dex = work / "dex"
        dex.mkdir()
        verify.run([*verify.d8, "--release" if args.release_dex else "--debug", "--min-api", "24", *classpath,
                    "--output", dex, *sorted(originals.rglob("*.class"))], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original coroutine DEX count changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != set(methods):
            raise RuntimeError("D8 changed an original coroutine definition")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-captured-super-disassembly")
        witnessed = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            if block[1] == LOCAL and name and name[1] == "<init>":
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)([^\n]*)", block[2])
                stores = [i for i, (op, ref) in enumerate(ops) if op == "iput-object" and LOCAL + "." in ref]
                calls = [i for i, (op, ref) in enumerate(ops) if op.startswith("invoke-direct") and "Lkotlin/coroutines/jvm/internal/SuspendLambda;.<init>:(ILkotlin/coroutines/Continuation;)V" in ref]
                if len(stores) != 3 or len(calls) != 1 or max(stores) >= calls[0]:
                    raise RuntimeError("D8 changed capture-before-super initialization")
                witnessed = True
        if not witnessed:
            raise RuntimeError("Missing compiler capture-before-super witness")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != set(methods) or coverage["class_count"] != 3 or coverage["method_count"] != 18
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("minimum_java_release") != 25):
            raise RuntimeError("Coroutine recovery changed original definitions or source release")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=25)
        name = rows[FACTORY].get("source_name", "make-impl")
        if execute((rebuilt, harness), "rebuilt", name) != baseline:
            raise RuntimeError("Recovered coroutine changed captures, suspension, snapshots or failures")
        # Metadata and source bindings are checked independently below; no
        # runtime equality can excuse a missing original declaration.
        metadata = verify_metadata(coverage, records, exceptions, rebuilt)
        (work / "compiler-metadata.json").open("x").write(json.dumps(metadata, indent=2) + "\n")
        body = fixture.read_text()
        mutations = (
            ("duplicate-seed", "val value = seed(this)", "seed(this); val value = seed(this)", 0),
            ("wrong-input", "operation(value)", "operation(null as T)", 0),
            ("dropped-store", "holder.set(value); ", "", 0),
            ("late-store", "holder.set(value); operation(value)", "val result = operation(value); holder.set(value); result", 0),
            ("wrong-result", "operation(value)", "operation(value); null as R", 0),
            ("duplicate-operation", "operation(value)", "operation(value); operation(value)", 10),
            ("replaced-holder", "holder.set(value)", "java.util.concurrent.atomic.AtomicReference<T>().set(value)", 0),
            ("swallowed-failure", "operation(value)", "try { operation(value) } catch (failure: Throwable) { null as R }", 8),
        )
        for label, anchor, changed, mode in mutations:
            if body.count(anchor) != 1:
                raise RuntimeError("Owned coroutine counterexample anchor changed: " + label)
            source = work / label / "Factory.kt"
            source.parent.mkdir()
            source.open("x").write(body.replace(anchor, changed))
            variant = work / (label + "-classes")
            verify.run([*compiler, "-d", variant, source], "kotlinc-" + label)
            result = subprocess.run([str(verify.java), "-cp", os.pathsep.join(map(str, (variant, harness, *dependencies))),
                                     "CapturedClosedSuperHarness", "0", str(mode), "make-impl"],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned captured closed super" not in result.stderr:
                raise RuntimeError("Frozen coroutine observer missed " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or {str(p): sha(p) for p in inputs} != input_hashes
                or {str(p): sha(p) for p in dependencies} != dependency_hashes):
            raise RuntimeError("Frozen compiler, observer or dependency bytes changed")
        summary = {"status": "verified", "original_classes": 3, "original_methods": 18,
                   "fresh_jvm_cases": 60, "attempts_per_case": 2, "wrong_programs": len(mutations),
                   "release_dex": args.release_dex}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
