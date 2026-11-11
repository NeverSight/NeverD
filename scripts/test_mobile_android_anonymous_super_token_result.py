#!/usr/bin/env python3
"""Verify captured anonymous superclass calls retain closed Class<?> results."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


OWNER = "Lfixture/Host$1;"
CTOR = OWNER + "-><init>(Lfixture/Host;)V"
TARGET = "Lfixture/Base;->read(Lfixture/Input;)Ljava/lang/Class;"
OVERRIDE = OWNER + "->read(Lfixture/Input;)Ljava/lang/Class;"
SIGNATURE = "(Lfixture/Input;)Ljava/lang/Class<*>;"
PARENT_EXCEPTIONS = ["Ljava/lang/ClassNotFoundException;", "Ljava/io/IOException;"]
CHILD_EXCEPTIONS = list(reversed(PARENT_EXCEPTIONS))


def change_metadata_text(path: Path, original: bytes, replacement: bytes) -> dict:
    """Change one metadata UTF8 entry and prove the classfile tail and Code equal."""
    before = path.read_bytes()
    old = b"\x01" + len(original).to_bytes(2, "big") + original
    new = b"\x01" + len(replacement).to_bytes(2, "big") + replacement
    if before.count(old) != 1:
        raise RuntimeError("Authored superclass metadata entry is not unique")
    after = before.replace(old, new, 1)
    if (method_code(before) != method_code(after)
            or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
        raise RuntimeError("Superclass metadata control changed Code or classfile declarations")
    path.write_bytes(after)
    return {"original_metadata_text": original.decode(), "changed_metadata_text": replacement.decode(),
            "all_Code_and_classfile_tail": "unchanged",
            "before_sha256": hashlib.sha256(before).hexdigest(),
            "after_sha256": hashlib.sha256(after).hexdigest()}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-anonymous-super-token-result-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "anonymous_super_token_result/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        name = "fixture.AnonymousSuperTokenResultHarness"
        verify.compile_local([FIXTURES / "harness/AnonymousSuperTokenResultHarness.java"], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 10):
            raise RuntimeError("Authored anonymous superclass token inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if (records["Lfixture/Base;"]["signature"] is not None or records[OWNER]["signature"] is not None
                or records["Lfixture/Base;"]["methods"][TARGET]["signature"] != SIGNATURE
                or records[OWNER]["methods"][OVERRIDE]["signature"] != SIGNATURE
                or records["Lfixture/Base;"]["methods"][TARGET]["access"] != 4
                or records[OWNER]["methods"][OVERRIDE]["access"] != 4
                or exceptions[TARGET] != PARENT_EXCEPTIONS or exceptions[OVERRIDE] != CHILD_EXCEPTIONS
                or records[OWNER]["fields"][OWNER + "->this$0:Lfixture/Host;"]["access"] != 0x1010):
            raise RuntimeError("Authored closed result, protected slot, capture or ordered Exceptions changed")
        excluded = {"Lfixture/Base;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x") as archive:
            archive.writestr("fixture/Base.class", (originals / "fixture/Base.class").read_bytes())
        inputs = {str(path.resolve()): sha(path) for path in (args.neverd, args.android_jar, provider)}
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded:
            classpath += ["--classpath", provider]

        def execute(runtime, label):
            results = []
            for shape in range(6):
                for mode in range(7):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name,
                                       str(shape), str(mode)], f"{label}-{shape}-{mode}")
                    if text != "attempts=2,anonymous-super-token-result=matched\n":
                        raise RuntimeError("Authored anonymous superclass token observer changed")
                    results.append(text)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {path.name: path.read_text() for path in sorted(fixture.rglob("*.java"))}
        call = "Class<?> result = super.read(input);"
        wrong = [
            ("missing-super", "Host.java", call, "Class<?> result = input.token;", 2, 0),
            ("repeated-super", "Host.java", call, "super.read(input); " + call, 2, 0),
            ("wrong-receiver", "Host.java", call, "Class<?> result = new Base().read(input);", 2, 0),
            ("wrong-input", "Host.java", call, "Class<?> result = super.read(new Input());", 2, 0),
            ("missing-null-branch", "Host.java", "if (input == null)", "if (false)", 0, 0),
            ("wrong-fallback", "Host.java", "Class<?> fallback = Host.this.fallback;", "Class<?> fallback = null;", 0, 0),
            ("lost-capture-effect", "Host.java", "Host.this.calls++;", ";", 2, 0),
            ("lost-prefix", "Host.java", "Trace.hit(1, this, input, Host.this);", ";", 2, 0),
            ("lost-suffix", "Host.java", "Trace.hit(4, this, input, result);", ";", 2, 0),
            ("late-parent-result", "Base.java", "return result;", "return input.token;", 2, 0),
            ("late-branch-result", "Host.java", "return fallback;", "return Host.this.fallback;", 0, 0),
            ("wrong-captured-host", "Host.java", "Trace.hit(1, this, input, Host.this);", "Trace.hit(1, this, input, null);", 2, 0),
            ("wrapped-checked-failure", "Trace.java", "if (current != null) current.observe(phase, owner, input, value);",
             "try { if (current != null) current.observe(phase, owner, input, value); } catch (IOException failure) { throw new IOException(failure); }", 2, 2),
        ]
        for label, filename, anchor, replacement, shape, mode in wrong:
            text = texts[filename]
            if text.count(anchor) != 1:
                raise RuntimeError("Authored superclass token mutation anchor changed: " + label)
            changed = work / label / filename
            changed.parent.mkdir()
            changed.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([changed], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     name, str(shape), str(mode)], text=True, capture_output=True,
                                    timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned anonymous super token result mismatch" not in result.stderr:
                raise RuntimeError("Observer accepted an incorrect superclass token program: " + label)

        paths = [path.relative_to(originals) for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(source, label, declarations):
            dexdir = work / label
            dexdir.mkdir()
            text = verify.run([*verify.d8, "--debug", "--min-api", "24", *declarations,
                               "--output", dexdir, *[source / path for path in paths]], label)
            if "Signature is ignored" in text:
                raise RuntimeError("D8 ignored an authored superclass token Signature")
            return dexdir / "classes.dex"

        source_dex = dex(originals, "dex", classpath)
        dex_sha = sha(source_dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an authored superclass token definition")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        (work / "source-dex-sha256.txt").open("x").write(dex_sha + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in report["methods"]}
        if (set(rows) != methods or report["class_count"] != len(classes)
                or report["method_count"] != len(methods) or report["unrecovered_method_count"]
                or report.get("projected_method_count") != 3
                or report.get("declaration_only_method_count") != 1
                or report.get("generic_signature_projections")
                or report.get("minimum_java_release", 8) != 25
                or any("source_constructor_declared_throws" in row for row in rows.values())):
            raise RuntimeError("Superclass token recovery changed definition roles or source contracts")
        projected = {CTOR, OVERRIDE, "Lfixture/Host;->make()Lfixture/Base;"}
        if any(row["status"] != ("source-projected" if identity in projected else
                   "declaration-only" if all_methods[identity] == "declaration" else "recovered")
               or (identity in projected and row.get("projection_kind") != "anonymous-to-named-local")
               for identity, row in rows.items()):
            raise RuntimeError("Superclass token original method roles changed")
        bindings = report.get("class_source_bindings", [])
        if len(bindings) != 1:
            raise RuntimeError("Superclass token projection has no unique local class binding")
        binding = bindings[0]
        if (binding["class"] != OWNER or binding["binding_kind"] != "anonymous-to-named-local"
                or binding["binary_name_status"] != "changed" or not binding.get("metadata_differences")
                or not any("constructor parameters may differ" in text for text in binding["metadata_differences"])):
            raise RuntimeError("Superclass token local binding omitted its metadata differences")

        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=provider if excluded else None)
        after = class_identity.compiler_classes(rebuilt)
        after_exceptions = exception_inventory(rebuilt)
        candidates = [owner for owner, facts in after.items()
                      if facts["inner_class"] and facts["inner_class"]["name"] == binding["source_name"]
                      and facts["enclosing_method"] == records[OWNER]["enclosing_method"]]
        if len(candidates) != 1:
            raise RuntimeError("Compiled superclass token local identity lacks its original enclosure")
        replacement = candidates[0]
        mapped = lambda text: text.replace(OWNER, replacement)
        source_ctor = mapped(CTOR).replace("-><init>(", "-><init>(Lfixture/Host;", 1)
        source_methods = {source_ctor if identity == CTOR else mapped(row.get("source_identity", identity))
                          for identity, row in rows.items()}
        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"])
                   for row in report.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != {mapped(owner) for owner in classes} or helpers != expected_helpers
                or set(actual_methods) != source_methods | helpers):
            raise RuntimeError("Compiled superclass token definitions differ beyond reported source mappings")
        for owner in classes:
            before, actual = records[owner], after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                wanted = json.loads(mapped(json.dumps(before.get(key))))
                if owner == OWNER and key == "inner_class":
                    wanted["name"] = binding["source_name"]
                if owner == OWNER and key == "fields":
                    capture = replacement + "->this$0:Lfixture/Host;"
                    wanted[capture]["access"] &= ~0x1000
                if wanted != actual.get(key):
                    raise RuntimeError("Superclass token class metadata differs beyond disclosure: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = json.loads(mapped(json.dumps(metadata)))
                target = mapped(identity)
                if identity == CTOR:
                    target = source_ctor
                    wanted["prototype"] = target.split("-><init>")[1]
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != exceptions[identity]:
                    raise RuntimeError("Superclass token method role, Signature or ordered Exceptions changed: " + identity)
        runtime = (rebuilt, harness, provider) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Superclass token projection changed capture, dispatch, result or exception identity")

        controls = [
            ("incompatible-result", SIGNATURE.encode(), b"(Lfixture/Input;)Ljava/lang/Class<Ljava/lang/String;>;"),
            ("unbound-result", SIGNATURE.encode(), b"(Lfixture/Input;)Ljava/lang/Class<TMissing;>;"),
            ("input-erasure", SIGNATURE.encode(), b"(Ljava/lang/Object;)Ljava/lang/Class<*>;"),
            ("unknown-throws", b"java/lang/ClassNotFoundException", b"fixture/MissingFailure"),
        ]
        for label, original, replacement in controls:
            negative = work / (label + "-originals")
            shutil.copytree(originals, negative)
            receipt = change_metadata_text(negative / "fixture/Base.class", original, replacement)
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            negative_classpath = list(classpath)
            if excluded:
                negative_jar = work / (label + "-provider.jar")
                with zipfile.ZipFile(negative_jar, "x") as archive:
                    archive.writestr("fixture/Base.class", (negative / "fixture/Base.class").read_bytes())
                negative_classpath[-1] = negative_jar
            negative_dex = dex(negative, label + "-dex", negative_classpath)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex),
                                     *map(str, negative_classpath), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if (result.returncode != 1 or rejected.exists()
                    or not any(token in result.stderr.lower() for token in ("generic", "signature", "throws", "super", "bound"))):
                raise RuntimeError("Unproved superclass token metadata was accepted: " + label)
        if (hashes(originals) != frozen["originals"] or hashes(harness) != frozen["harness"] or sha(source_dex) != dex_sha
                or any(sha(Path(path)) != digest for path, digest in inputs.items())):
            raise RuntimeError("Frozen anonymous superclass token input changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "minimum_java_release": 25, "reported_anonymous_bindings": 1,
                   "fresh_jvm_cases": 42, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_controls": len(controls), "dependency_provider": args.dependency_provider}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
