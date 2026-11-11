#!/usr/bin/env python3
"""Verify scoped void interface defaults against untouched Kotlin originals."""
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
import zlib

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_interface_default_access import compiler_facts
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_interface_default_access import compiler_facts

OWNER = "Lfixture/DefaultApi;"
PROTO = "(Lkotlin/jvm/functions/Function2;Ljava/lang/Object;)V"
DEFAULT = OWNER + "->work" + PROTO
ACCESS = OWNER + "->access$work$jd(Lfixture/DefaultApi;Lkotlin/jvm/functions/Function2;Ljava/lang/Object;)V"
SIGNATURE = "(Lkotlin/jvm/functions/Function2<-TN;Ljava/lang/Object;Lkotlin/Unit;>;Ljava/lang/Object;)V"


def private_default_dex(data: bytes) -> tuple[bytes, dict]:
    """Owned negative: move work to direct methods without rewriting any Code.

    Mutating the classfile before D8 would also change the original super call
    into a valid direct call. This explicit DEX mutation preserves that call.
    It is never used to prepare the positive compiler fixture.
    """
    def u32(offset):
        return int.from_bytes(data[offset:offset + 4], "little")

    def uleb(offset):
        result = 0
        for shift in range(0, 35, 7):
            byte = data[offset]
            offset += 1
            result |= (byte & 127) << shift
            if byte < 128:
                return result, offset
        raise RuntimeError("Malformed owned DEX ULEB")

    def encode(value):
        result = bytearray()
        while value >= 128:
            result.append((value & 127) | 128)
            value >>= 7
        result.append(value)
        return bytes(result)

    strings = []
    for index in range(u32(56)):
        _, offset = uleb(u32(u32(60) + index * 4))
        # Only ASCII fixture names/descriptors are compared below; Kotlin's
        # unrelated modified-UTF8 annotation constants stay opaque bytes.
        strings.append(data[offset:data.index(0, offset)])
    types = [strings[u32(u32(68) + index * 4)] for index in range(u32(64))]
    selected = []
    for index in range(u32(88)):
        at = u32(92) + index * 8
        owner = types[int.from_bytes(data[at:at + 2], "little")]
        proto = u32(76) + int.from_bytes(data[at + 2:at + 4], "little") * 12
        parameters = u32(proto + 8)
        args = [] if not parameters else [types[int.from_bytes(data[parameters + 4 + n * 2:parameters + 6 + n * 2], "little")]
                                              for n in range(u32(parameters))]
        prototype = b"(" + b"".join(args) + b")" + types[u32(proto + 4)]
        if owner == OWNER.encode() and strings[u32(at + 4)] == b"work" and prototype == PROTO.encode():
            selected.append(index)
    if len(selected) != 1:
        raise RuntimeError("Missing unique owned default method ID")
    for index in range(u32(96)):
        at = u32(100) + index * 32
        if types[u32(at)] != OWNER.encode():
            continue
        start = offset = u32(at + 24)
        counts = []
        for _ in range(4):
            value, offset = uleb(offset)
            counts.append(value)
        fields_start = offset
        for _ in range(counts[0] + counts[1]):
            _, offset = uleb(offset)
            _, offset = uleb(offset)
        fields = data[fields_start:offset]
        groups = []
        for count in counts[2:]:
            methods, method_id = [], 0
            for _ in range(count):
                delta, offset = uleb(offset)
                method_id += delta
                flags, offset = uleb(offset)
                code, offset = uleb(offset)
                methods.append((method_id, flags, code))
            groups.append(methods)
        target = [m for m in groups[1] if m[0] == selected[0]]
        if len(target) != 1 or target[0][1] != 1 or not target[0][2] or any(m[0] == selected[0] for m in groups[0]):
            raise RuntimeError("Owned default is not one public virtual method")
        groups[0].append((selected[0], 2, target[0][2]))
        groups[0].sort()
        groups[1] = [m for m in groups[1] if m[0] != selected[0]]
        counts[2] += 1
        counts[3] -= 1
        replacement = b"".join(encode(n) for n in counts) + fields
        for methods in groups:
            previous = 0
            for method_id, flags, code in methods:
                replacement += encode(method_id - previous) + encode(flags) + encode(code)
                previous = method_id
                if code and not (code + 16 + u32(code + 12) * 2 <= start or code >= offset):
                    raise RuntimeError("Owned Code overlaps the declaration mutation")
        if len(replacement) != offset - start:
            raise RuntimeError("Owned private declaration cannot retain all original offsets")
        changed = bytearray(data)
        changed[start:offset] = replacement
        changed[12:32] = hashlib.sha1(changed[32:]).digest()
        changed[8:12] = zlib.adler32(changed[12:]).to_bytes(4, "little")
        if changed[32:start] != data[32:start] or changed[offset:] != data[offset:]:
            raise RuntimeError("Owned negative changed Code or another declaration")
        return bytes(changed), {"owner": OWNER, "method": DEFAULT, "original_access": 1, "changed_access": 2,
                               "declaration_start": start, "declaration_end": offset,
                               "instruction_bytes_and_code_offsets": "unchanged", "dispatch": "original invoke-super retained"}
    raise RuntimeError("Missing owned default class data")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "kotlin-lib", "kotlin-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-trace", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-owner-void-default-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "owner_void_default_access"
        kotlin = args.kotlin_lib.resolve()
        inputs = [args.android_jar.resolve(), *sorted(kotlin.glob("*.jar"))]
        if not (kotlin / "kotlin-compiler.jar").is_file():
            raise RuntimeError("Untouched Kotlin compiler is required")
        digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        input_hashes = {str(p): digest(p) for p in inputs}
        dependencies = []
        for source in (kotlin / "kotlin-stdlib.jar", kotlin / "annotations-13.0.jar"):
            target = work / source.name
            with zipfile.ZipFile(source) as before, zipfile.ZipFile(target, "x") as after:
                for entry in before.infolist():
                    if entry.filename.endswith(".class") and not entry.filename.startswith("META-INF/"):
                        if ".." in Path(entry.filename).parts or entry.filename.startswith("/"):
                            raise RuntimeError("Unsafe original compiler dependency")
                        after.writestr(entry.filename, before.read(entry))
            dependencies.append(target)
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        verify.compile_local(sorted((fixture / "java").rglob("*.java")), original_java, release=8)
        originals.mkdir()
        compiler = [choose_jdk(args.kotlin_java_home) / "bin/java", "-cp", kotlin / "*",
                    "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-jdk-home", choose_jdk(args.kotlin_java_home),
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (kotlin / "kotlin-stdlib.jar", original_java))),
                    "-jvm-target", "1.8", "-Xjvm-default=all-compatibility"]
        verify.run([*compiler, "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in original_java.rglob("*.class"):
            target = originals / path.relative_to(original_java)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
        observers = sorted((fixture / "harness").glob("*.java"))
        verify.compile_local(observers, harness, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        classes, methods = compiler_inventory(verify, originals)
        methods = set(methods)
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        if (len(classes), len(methods)) != (5, 10) or records[OWNER]["methods"][DEFAULT]["signature"] != SIGNATURE:
            raise RuntimeError("Original scoped void compiler inventory or Signature changed: " + repr((len(classes), len(methods))))
        if args.dependency_trace:
            classes.remove("Lfixture/Trace;")
            methods = {m for m in methods if m.split("->", 1)[0] in classes}
            dependency = work / "owned-trace.jar"
            with zipfile.ZipFile(dependency, "x") as jar:
                jar.writestr("fixture/Trace.class", (originals / "fixture/Trace.class").read_bytes())
            dependencies.append(dependency)
        dependency_hashes = {str(p): digest(p) for p in dependencies}

        def execute(runtime, name, label):
            outputs = []
            for route in range(4):
                for variant in range(2):
                    for mode in range(12):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                            "OwnerVoidDefaultHarness", str(route), str(variant), str(mode), name], f"{label}-{route}-{variant}-{mode}")
                        pattern = f"route={route},variant={variant},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Original scoped void observer matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "work", "baseline")
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]

        def dex(root, destination):
            destination.mkdir()
            paths = [p for p in sorted(root.rglob("*.class")) if "L" + p.relative_to(root).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", destination, *paths], "d8-" + destination.name)
            result = destination / "classes.dex"
            if list(destination.glob("classes*.dex")) != [result]:
                raise RuntimeError("Original scoped void DEX count changed")
            return result

        source_dex = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original scoped void entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-default-disassembly")
        accessor_witness = False
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            kind = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] == OWNER and kind and kind[1] == ACCESS.split("access$work$jd", 1)[1]:
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
                if len(ops) != 2 or ops[0] not in ("invoke-super", "invoke-super/range") or ops[1] != "return-void":
                    raise RuntimeError("D8 changed the exact original default accessor")
                accessor_witness = True
        if not accessor_witness or "invoke-interface" not in disassembly:
            raise RuntimeError("D8 omitted the default dispatch or callback witness")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        visible = Counter(re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations))
        expected_visible = Counter()
        for owner in classes:
            record = records[owner]
            for row in (record, *record["fields"].values(), *record["methods"].values()):
                expected_visible.update(a["type"] for a in row.get("runtime_visible_annotations", []))
        if "VISIBILITY_BUILD" in annotations or visible != expected_visible:
            raise RuntimeError("D8 changed original scoped void annotation retention")
        # D8 omits original CLASS-retained compiler annotations. Runtime
        # annotations remain compared in full against original classfiles.
        def dex_metadata(value):
            if isinstance(value, dict):
                return {k: dex_metadata(v) for k, v in value.items() if k not in
                        {"runtime_invisible_annotations", "runtime_invisible_parameter_annotations"}}
            if isinstance(value, list):
                return [dex_metadata(v) for v in value]
            return value
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage["status"] != "partial"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or rows[DEFAULT].get("projection_kind") != "interface-default-body"
                or rows[ACCESS].get("projection_kind") != "static-interface-default-accessor"):
            raise RuntimeError("Scoped void recovery omitted an entry or changed a source contract")
        helper = rows[DEFAULT]["default_body_helper"]
        name = helper.split("->", 1)[1].split("(", 1)[0]
        if rows[ACCESS].get("default_body_helper") != helper:
            raise RuntimeError("Default body was duplicated across original entries")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=os.pathsep.join(map(str, dependencies)), release=coverage["minimum_java_release"])
        after_classes, after_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]: r for r in coverage["generated_source_helpers"]}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        expected_helpers |= {helper, "Lfixture/DefaultApi$DefaultImpls;-><init>()V"}
        if after_classes != classes or set(after_methods) != methods | expected_helpers or set(helpers) != expected_helpers:
            raise RuntimeError("Scoped void recovery invented or omitted an entry/helper")
        if helpers[helper].get("generic_signature") != SIGNATURE:
            raise RuntimeError("Scoped void helper lacks the complete original Signature receipt")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, actual = dex_metadata(records[owner]), dex_metadata(after[owner])
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if before.get(key) != actual.get(key):
                    raise RuntimeError("Scoped void class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = deepcopy(metadata)
                if identity == ACCESS:
                    wanted["access"] &= ~0x1000
                if actual["methods"].get(identity) != wanted or after_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Scoped void method metadata or Exceptions changed: " + identity)
        if after[OWNER]["methods"][helper]["signature"] != SIGNATURE:
            raise RuntimeError("Private helper lost its original owner scope")
        if execute((rebuilt, harness), name, "rebuilt") != baseline:
            raise RuntimeError("Scoped void default changed dispatch, input identity, order or failure")
        original_source = (fixture / "kotlin/fixture/DefaultApi.kt").read_text()
        for label, old, new in (
            ("reversed-inputs", "block!!(input, value)", "block!!(value as N, input)"),
            ("repeated-callback", "block!!(input, value)", "block!!(input, value)\n        block(input, value)"),
            ("lost-effect", "Trace.hit(3)", "Trace.hit(8)"),
        ):
            if original_source.count(old) != 1:
                raise RuntimeError("Scoped void independent mutation anchor changed")
            candidate = work / label / "DefaultApi.kt"
            candidate.parent.mkdir()
            candidate.open("x").write(original_source.replace(old, new))
            variant = work / (label + "-classes")
            variant.mkdir()
            verify.run([*compiler, "-d", variant, candidate], "kotlinc-" + label)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness, *dependencies))),
                "OwnerVoidDefaultHarness", "1", "1", "0", "work"], text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned scoped void default" not in result.stderr:
                raise RuntimeError("Frozen observer missed " + label)
        wrong = work / "virtual-dispatch/OwnerVoidDefaultCaller.java"
        wrong.parent.mkdir()
        original = (fixture / "harness/OwnerVoidDefaultCaller.java").read_text()
        wrong.open("x").write(original.replace("DefaultApi.DefaultImpls.work(receiver, block, value);", "receiver.work(block, value);"))
        variant = work / "virtual-dispatch-classes"
        verify.compile_local([wrong], variant, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (variant, originals, harness, *dependencies))),
            "OwnerVoidDefaultHarness", "1", "1", "0", "work"], text=True, capture_output=True, timeout=600, env=verify.environment)
        (work / "virtual-dispatch-counterexample.log").open("x").write(result.stdout + result.stderr)
        if result.returncode == 0 or "owned scoped void default" not in result.stderr:
            raise RuntimeError("Frozen observer missed virtual default dispatch")
        changed, witness = private_default_dex(source_dex.read_bytes())
        (work / "private-witness.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        negative = work / "private-dex"
        negative.mkdir()
        negative_dex = negative / "classes.dex"
        negative_dex.open("xb").write(changed)
        negative_dump = verify.run([verify.dexdump, "-d", negative_dex], "private-disassembly")
        if "invoke-super" not in negative_dump:
            raise RuntimeError("Owned private counterexample lost its original super call")
        negative_dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", negative_dex], "private-inventory")
        negative_inventory = parse_dexdump(negative_dump, negative_dex.name, dex_header(changed))
        original_methods = {m["identity"]: m for m in inventory["methods"]}
        if {m["identity"] for m in negative_inventory["methods"]} != set(original_methods):
            raise RuntimeError("Owned negative changed its original method inventory")
        for row in negative_inventory["methods"]:
            before = original_methods[row["identity"]]
            flags = 2 if row["identity"] == DEFAULT else before["access_flags"]
            if (row["access_flags"] != flags or row["has_code"] != before["has_code"]
                    or row.get("code_units") != before.get("code_units")
                    or (row["identity"] == DEFAULT and row["section"] != "Direct methods")):
                raise RuntimeError("Owned negative changed Code or another method flag")
        result = subprocess.run([args.neverd.resolve(), "mobile", negative_dex, *classpath, "--timeout", "600", "-o", work / "private-rejected"],
                                text=True, capture_output=True, timeout=600, env=verify.environment)
        (work / "private-recovery.log").open("x").write(result.stdout + result.stderr)
        if (result.returncode == 0 or (work / "private-rejected").exists() or "private" not in result.stderr
                or not any(word in result.stderr for word in ("dispatch", "super"))):
            raise RuntimeError("Private same-Code default gained a direct-default receipt")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(digest(Path(p)) != h for p, h in input_hashes.items())
                or any(digest(Path(p)) != h for p, h in dependency_hashes.items())):
            raise RuntimeError("Original compiler, input, dependencies or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "attempts": 2, "dependency_trace": args.dependency_trace,
                    "owner_scope_signatures_metadata_and_exceptions": "unchanged except disclosed accessor flag/helper",
                    "default_selection_callback_input_identity_order_and_failure": "matched",
                    "independent_compiled_counterexamples": 4, "same_code_private_default": "rejected",
                    "input_compiler_dependencies_and_observers": "unchanged", "coverage": "partial"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
