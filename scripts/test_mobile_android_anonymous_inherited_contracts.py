#!/usr/bin/env python3
"""Verify anonymous Object scope projections through original inherited bodies."""
from __future__ import annotations

import argparse
import copy
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def drop_owned_read_exception(path: Path) -> dict:
    """Change only the owned read declaration's two-entry Exceptions attribute."""
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(before[position:position + 2], "big")

    def attributes(offset: int, count: int) -> tuple:
        rows = []
        for _ in range(count):
            name, size = struct.unpack_from(">HI", before, offset)
            end = offset + 6 + size
            rows.append((strings[name], offset, end))
            offset = end
        return offset, rows

    fields = int.from_bytes(before[position:position + 2], "big")
    position += 2
    for _ in range(fields):
        position, _ = attributes(position + 8, int.from_bytes(before[position + 6:position + 8], "big"))
    methods = int.from_bytes(before[position:position + 2], "big")
    position += 2
    selected = []
    for _ in range(methods):
        flags, name, descriptor, count = struct.unpack_from(">HHHH", before, position)
        position, rows = attributes(position + 8, count)
        if strings[name] != b"read" or strings[descriptor] != b"()Ljava/lang/Object;":
            continue
        if flags != 0x401:
            raise RuntimeError("Owned abstract read declaration changed")
        selected += [(start, end) for key, start, end in rows if key == b"Exceptions"]
    if len(selected) != 1:
        raise RuntimeError("Missing exact owned read Exceptions attribute")
    start, end = selected[0]
    if end - start != 12 or int.from_bytes(before[start + 6:start + 8], "big") != 2:
        raise RuntimeError("Owned read exception set changed")
    last = int.from_bytes(before[start + 10:start + 12], "big")
    changed = before[:start] + before[start:start + 2] + struct.pack(">IHH", 4, 1, last) + before[end:]
    if method_code(before) != method_code(changed):
        raise RuntimeError("Owned exception edit changed Code or runtime descriptors")
    path.write_bytes(changed)
    return {"method": "read()Ljava/lang/Object;", "original_exception_count": 2,
            "changed_exception_count": 1, "Code_Signatures_and_descriptors": "unchanged"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-parent", action="store_true")
    parser.add_argument("--fixed-throws", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-anonymous-inherited-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "anonymous_inherited_contracts/java").rglob("*.java"))
        harness_source = FIXTURES / "harness/AnonymousInheritedContractsHarness.java"
        if args.fixed_throws:
            declarations = {
                "Reader.java": [
                    ("    R read();", "    R read() throws java.lang.ReflectiveOperationException, java.io.IOException;\n"
                     "    R readTimed(long duration, java.util.concurrent.TimeUnit unit) throws java.util.concurrent.TimeoutException, java.lang.ReflectiveOperationException, java.io.IOException;"),
                    ("    void listen(Runnable task, Executor executor);", "    void listen(Runnable task, Executor executor) throws java.io.IOException;")],
                "Root.java": [
                    ("    public final R read() { return pending(); }", "    public java.io.IOException readFailure, listenerFailure;\n"
                     "    public java.lang.ReflectiveOperationException reflectionFailure;\n"
                     "    public java.util.concurrent.TimeoutException timeoutFailure;\n"
                     "    public long lastDuration;\n"
                     "    public java.util.concurrent.TimeUnit lastUnit;\n"
                     "    public final R read() throws java.io.IOException, java.lang.ReflectiveOperationException {\n"
                     "        if (readFailure != null) throw readFailure;\n"
                     "        if (reflectionFailure != null) throw reflectionFailure;\n"
                     "        return pending();\n    }\n"
                     "    public final R readTimed(long duration, java.util.concurrent.TimeUnit unit) throws java.io.IOException, java.lang.ReflectiveOperationException, java.util.concurrent.TimeoutException {\n"
                     "        lastDuration = duration; lastUnit = unit;\n"
                     "        unit.toNanos(duration);\n"
                     "        if (timeoutFailure != null) throw timeoutFailure;\n"
                     "        return read();\n    }"),
                    ("public final void listen(Runnable task, Executor executor) {", "public final void listen(Runnable task, Executor executor) throws java.io.IOException {"),
                    ("        executor.execute(task);", "        if (listenerFailure != null) throw listenerFailure;\n        executor.execute(task);")],
                "Owner.java": [("public T read() {", "public T read() throws java.io.IOException, java.lang.ReflectiveOperationException {")],
            }
            owned_inputs = []
            for source in inputs:
                content = source.read_text()
                for before, after in declarations[source.name]:
                    if content.count(before) != 1:
                        raise RuntimeError("Missing exact owned fixed-throws declaration: " + source.name)
                    content = content.replace(before, after, 1)
                replacement = work / "throws-java/fixture" / source.name
                replacement.parent.mkdir(parents=True, exist_ok=True)
                replacement.write_text(content)
                owned_inputs.append(replacement)
            inputs = owned_inputs
            content = harness_source.read_text()
            content = content.replace("    private static void failure(Runnable action, Class<?> type, RuntimeException sentinel) {",
                "    private interface CheckedAction { void run() throws Exception; }\n"
                "    private static void failure(CheckedAction action, Class<?> type, Exception sentinel) {", 1)
            content = content.replace("catch (RuntimeException actual)", "catch (Exception actual)", 1)
            anchor = "            value(root.listenerCalls == 5);\n        }\n"
            if content.count(anchor) != 1:
                raise RuntimeError("Missing exact owned fixed-throws harness boundary")
            extra = '''            java.io.IOException io = new java.io.IOException("owned IO failure");
            ReflectiveOperationException reflection = new ReflectiveOperationException("owned reflection failure");
            root.readFailure = io; root.reflectionFailure = reflection;
            failure(() -> root.read(), io.getClass(), io);
            value(owner.calls == 7);
            root.readFailure = null;
            failure(() -> root.read(), reflection.getClass(), reflection);
            value(owner.calls == 7);
            root.reflectionFailure = null;
            root.listenerFailure = io; trace[0] = 0;
            failure(() -> reader.listen(task, executor), io.getClass(), io);
            value(trace[0] == 0);
            value(root.listenerCalls == 6);
            identity(root.lastTask, task);
            root.listenerFailure = null;
            for (long duration : new long[] {Long.MIN_VALUE, -1, 0, 1, Long.MAX_VALUE})
                for (java.util.concurrent.TimeUnit unit : new java.util.concurrent.TimeUnit[] {
                        java.util.concurrent.TimeUnit.MILLISECONDS, java.util.concurrent.TimeUnit.DAYS}) {
                    identity(root.readTimed(duration, unit), next);
                    value(root.lastDuration == duration);
                    identity(root.lastUnit, unit);
                }
            value(owner.calls == 17);
            java.util.concurrent.TimeoutException timeout = new java.util.concurrent.TimeoutException("owned timeout failure");
            root.timeoutFailure = timeout;
            failure(() -> root.readTimed(42, java.util.concurrent.TimeUnit.SECONDS), timeout.getClass(), timeout);
            value(root.lastDuration == 42);
            identity(root.lastUnit, java.util.concurrent.TimeUnit.SECONDS);
            value(owner.calls == 17);
            root.timeoutFailure = null;
            failure(() -> root.readTimed(99, null), NullPointerException.class, null);
            value(root.lastDuration == 99);
            identity(root.lastUnit, null);
            value(owner.calls == 17);
            root.readFailure = io;
            failure(() -> root.readTimed(-77, java.util.concurrent.TimeUnit.MICROSECONDS), io.getClass(), io);
            value(owner.calls == 17);
            root.readFailure = null;
'''
            content = content.replace(anchor, "            value(root.listenerCalls == 5);\n" + extra + "        }\n", 1)
            anchor = "        System.out.println(\"values=\" + values"
            if content.count(anchor) != 1:
                raise RuntimeError("Missing exact owned reflection harness boundary")
            reflection = '''        value(java.util.Arrays.equals(Root.class.getDeclaredMethod("read").getExceptionTypes(),
                new Class<?>[] {java.io.IOException.class, ReflectiveOperationException.class}));
        value(java.util.Arrays.equals(Reader.class.getDeclaredMethod("read").getExceptionTypes(),
                new Class<?>[] {ReflectiveOperationException.class, java.io.IOException.class}));
        value(java.util.Arrays.equals(Root.class.getDeclaredMethod("readTimed", long.class, java.util.concurrent.TimeUnit.class).getExceptionTypes(),
                new Class<?>[] {java.io.IOException.class, ReflectiveOperationException.class, java.util.concurrent.TimeoutException.class}));
        value(java.util.Arrays.equals(Reader.class.getDeclaredMethod("readTimed", long.class, java.util.concurrent.TimeUnit.class).getExceptionTypes(),
                new Class<?>[] {java.util.concurrent.TimeoutException.class, ReflectiveOperationException.class, java.io.IOException.class}));
        value(java.util.Arrays.equals(Root.class.getDeclaredMethod("listen", Runnable.class, Executor.class).getExceptionTypes(),
                new Class<?>[] {java.io.IOException.class}));
        value(java.util.Arrays.equals(Reader.class.getDeclaredMethod("listen", Runnable.class, Executor.class).getExceptionTypes(),
                new Class<?>[] {java.io.IOException.class}));
        value(java.util.Arrays.equals(Owner.class.getDeclaredMethod("read").getExceptionTypes(),
                new Class<?>[] {java.io.IOException.class, ReflectiveOperationException.class}));
'''
            content = content.replace(anchor, reflection + anchor, 1)
            harness_source = work / "AnonymousInheritedContractsHarness.java"
            harness_source.write_text(content)
        verify.compile_local(inputs, originals)
        verify.compile_local([harness_source], harness,
                             classpath=originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        records = class_identity.compiler_classes(originals)
        all_classes, all_methods = compiler_inventory(verify, originals)
        parent_classes = {"Lfixture/Reader;", "Lfixture/Root;"}
        classes = all_classes - parent_classes if args.dependency_parent else all_classes
        methods = {k: v for k, v in all_methods.items() if k.split("->", 1)[0] in classes}
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x", zipfile.ZIP_DEFLATED) as archive:
            for name in ("Reader", "Root"):
                archive.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "AnonymousInheritedContractsHarness", "original"], "baseline")
        value_cases, identity_cases, failures = (178, 241, 53) if args.fixed_throws else (91, 105, 29)
        if baseline.splitlines() != [f"values={value_cases},identities={identity_cases},failures={failures}",
                "inherited-bodies=retained,default-dispatch=matched,mutable-capture-and-constructor-order=matched"]:
            raise RuntimeError("Owned anonymous inheritance baseline missed expected behavior: " + baseline)

        def dex(source: Path, destination: Path) -> Path:
            destination.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--classpath", dependency, "--output", destination,
                        *[p for p in sorted(source.rglob("*.class"))
                          if "L" + p.relative_to(source).as_posix()[:-6] + ";" in classes]], "d8-" + destination.name)
            if list(destination.glob("classes*.dex")) != [destination / "classes.dex"]:
                raise RuntimeError("D8 changed the owned DEX inventory")
            return destination / "classes.dex"

        owned = dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", owned], "inventory")
        inventory = parse_dexdump(dump, owned.name, dex_header(owned.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original anonymous inherited declarations")
        output = work / "recovered"
        classpath = ["--classpath", str(args.android_jar.resolve())]
        if args.dependency_parent:
            classpath += ["--classpath", str(dependency)]
        verify.run([args.neverd.resolve(), "mobile", owned, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage["minimum_java_release"] != 25):
            raise RuntimeError("Anonymous inheritance lost input entries or source disclosure")
        changes = [c for r in coverage.get("generic_signature_projections", []) for c in r["changes"]]
        if (len(changes) != 2 or any(c["kind"] != "anonymous-member-type-variable" or
                c["source_type"] != "Ljava/lang/Object;" or c.get("scope_origin") != "Lfixture/Owner;"
                or c["original_type"] != "Lfixture/Owner;::T" for c in changes)):
            raise RuntimeError("Inherited proof changed beyond the original Object scope receipt")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("binding_kind") != "anonymous-to-named-member" or
                bindings[0].get("class") != "Lfixture/Owner$1;" or not bindings[0].get("metadata_differences")):
            raise RuntimeError("Anonymous member lost its exact source binding")
        binding = bindings[0]
        owner = binding["class"]
        renamed = binding["source_enclosing_class"][:-1] + "$" + binding["source_name"] + ";"

        def rename(text: str) -> str:
            return text.replace(owner, renamed)

        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_parent else None, release=25)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {rename(r["class"] + "->" + r["name"] + r["prototype"])
                   for r in coverage.get("generated_source_helpers", [])}
        if (actual_classes != {rename(c) for c in classes} or
                set(actual_methods) != {rename(m) for m in methods} | helpers):
            raise RuntimeError("javac changed original inventory beyond the reported member and helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for original_owner in classes:
            wanted = copy.deepcopy(records[original_owner])
            wanted["fields"] = {rename(k): v for k, v in wanted["fields"].items()}
            wanted["methods"] = {rename(k): v for k, v in wanted["methods"].items()}
            if original_owner == owner:
                wanted["signature"] = "Lfixture/Root<Ljava/lang/Object;>;"
                wanted["enclosing_method"] = None
                wanted["inner_class"] = {"inner": renamed, "outer": "Lfixture/Owner;",
                                         "name": binding["source_name"], "access": 8}
                capture = rename(owner + "->this$0:Lfixture/Owner;")
                wanted["fields"][capture]["access"] &= ~0x1000
                wanted["methods"][rename(owner + "->pending()Ljava/lang/Object;")]["signature"] = None
            after = actual_records[rename(original_owner)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class",
                        "enclosing_method", "deprecated_attribute", "runtime_visible_annotations"):
                if after.get(key) != wanted.get(key):
                    raise RuntimeError("Original metadata changed beyond the member receipt: " + original_owner + ":" + key)
            for identity, metadata in wanted["methods"].items():
                if after["methods"][identity] != metadata:
                    raise RuntimeError("Original method metadata changed beyond Object scope: " + identity)
        runtime_path = (rebuilt, harness, dependency) if args.dependency_parent else (rebuilt, harness)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime_path)),
                             "AnonymousInheritedContractsHarness", "recovered"], "rebuilt-execution")
        if actual != baseline:
            raise RuntimeError("Original inheritance, mutable capture or callback failures changed")
        negatives = [("outer-bound", "Owner", b"<T:Ljava/lang/Object;>Ljava/lang/Object;",
                      b"<T:Ljava/lang/Object;:Ljava/lang/CharSequence;>Ljava/lang/Object;"),
                     ("complete-result", "Reader", b"()TR;", b"()Ljava/lang/String;")]
        if args.fixed_throws:
            negatives.append(("exception-set", "Reader", None, None))
        for label, declaration, before, after in negatives:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            target = negative / ("fixture/" + declaration + ".class")
            receipt = drop_owned_read_exception(target) if label == "exception-set" else change_signature(target, before, after)
            (work / (label + "-encoding.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            rejected = work / (label + "-rejected")
            negative_dex = dex(negative, work / (label + "-dex"))
            negative_classpath = list(classpath)
            if args.dependency_parent:
                changed_dependency = work / (label + "-parent.jar")
                with zipfile.ZipFile(changed_dependency, "x", zipfile.ZIP_DEFLATED) as archive:
                    for name in ("Reader", "Root"):
                        archive.writestr("fixture/" + name + ".class", (negative / ("fixture/" + name + ".class")).read_bytes())
                negative_classpath[-1] = str(changed_dependency)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *negative_classpath, "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Original same-erasure scope or complete inherited contract was bypassed")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, independently compiled harness or declaration bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_parent": args.dependency_parent, "fixed_declared_throws": args.fixed_throws,
                    "value_cases": value_cases, "identity_cases": identity_cases,
                    "ordered_failure_cases": failures, "inherited_bodies_and_default_dispatch": "matched",
                    "original_declared_exception_order": "matched" if args.fixed_throws else "not applicable",
                    "constructor_order_and_mutable_outer_capture": "matched",
                    "complete_original_metadata_and_inventory": "checked against exact reported differences",
                    "same_erasure_bound_and_result_counterexamples": "rejected",
                    "declared_exception_set_counterexample": "rejected with unchanged Code and Signatures" if args.fixed_throws else "not applicable",
                    "original_input_harness_and_sdk_bytes": "unchanged", "coverage": "partial"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
