#!/usr/bin/env python3
"""Verify exact constructor exception contracts and captured local projections."""
from __future__ import annotations

import argparse
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


OWNER = "Lfixture/Outer$1;"
CTOR = OWNER + "-><init>(Lfixture/Outer;Lfixture/Trace;Ljava/lang/Object;)V"
TARGET = "Lfixture/Base;-><init>(Lfixture/Trace;Ljava/lang/Object;)V"
CHECKED = ["Ljava/io/IOException;"]


def omit_constructor_exceptions(path: Path) -> dict:
    """Remove one authored constructor attribute; retain every other byte."""
    before = path.read_bytes()
    shape = class_records(before)
    strings = shape["strings"]
    position = shape["pool_end"] + 6
    position += 2 + 2 * int.from_bytes(before[position:position + 2], "big")

    def attributes(offset, count):
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
    patches, selected = [], 0
    for _ in range(methods):
        flags, name, descriptor, count = struct.unpack_from(">HHHH", before, position)
        end, rows = attributes(position + 8, count)
        if strings[name] == b"<init>":
            if flags != 0 or strings[descriptor].decode() != CTOR.split("-><init>")[1]:
                raise RuntimeError("Authored anonymous constructor identity changed")
            attributes_to_remove = [(start, stop) for key, start, stop in rows if key == b"Exceptions"]
            if len(attributes_to_remove) != 1:
                raise RuntimeError("Authored anonymous constructor has no unique Exceptions attribute")
            start, stop = attributes_to_remove[0]
            payload = before[start + 6:stop]
            if len(payload) != 4 or int.from_bytes(payload[:2], "big") != 1:
                raise RuntimeError("Authored anonymous constructor exception count changed")
            exception = "L" + strings[shape["classes"][int.from_bytes(payload[2:], "big")]].decode() + ";"
            if [exception] != CHECKED:
                raise RuntimeError("Authored anonymous constructor checked exception changed")
            patches = [(position + 6, position + 8, (count - 1).to_bytes(2, "big")), (start, stop, b"")]
            selected += 1
        position = end
    if selected != 1:
        raise RuntimeError("Authored anonymous constructor is not unique")
    after = before
    for start, stop, replacement in sorted(patches, reverse=True):
        after = after[:start] + replacement + after[stop:]
    if method_code(before) != method_code(after) or before[:shape["pool_end"]] != after[:shape["pool_end"]]:
        raise RuntimeError("Authored exception omission changed Code or constant pool")
    class_records(after)
    path.write_bytes(after)
    return {"method": CTOR, "original_declared_throws": CHECKED, "encoded_declared_throws": [],
            "changed_attributes": ["Exceptions"], "all_Code_and_other_bytes": "unchanged",
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
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-constructor-closed-exceptions-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "constructor_closed_exceptions/java"
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        shutil.copytree(originals, work / "unencoded-originals")
        encoding = omit_constructor_exceptions(originals / "fixture/Outer$1.class")
        (work / "original-constructor-exceptions-encoding.json").open("x").write(json.dumps(encoding, indent=2) + "\n")
        name = "ConstructorClosedExceptionsHarness"
        verify.compile_local([FIXTURES / "harness" / (name + ".java")], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness),
                  "unencoded_originals": hashes(work / "unencoded-originals")}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (6, 14):
            raise RuntimeError("Authored constructor exception inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        if (records[OWNER]["signature"] is not None or exceptions[CTOR]
                or records[OWNER]["methods"][CTOR]["signature"] is not None
                or records[OWNER]["fields"][OWNER + "->this$0:Lfixture/Outer;"]["access"] != 0x1010
                or exceptions[TARGET] != CHECKED):
            raise RuntimeError("Authored captured constructor and parent exception contract changed")
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
            for route in range(4):
                for mode in range(9):
                    for shape in range(2):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name,
                                           str(route), str(mode), str(shape)], f"{label}-{route}-{mode}-{shape}")
                        if text != "attempts=2,constructor-closed-exceptions=matched\n":
                            raise RuntimeError("Authored constructor exception observer changed")
                        results.append(text)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {path.name: path.read_text() for path in sorted(fixture.rglob("*.java"))}
        wrong = [
            ("wrong-argument", "Outer.java", "new Base(trace, value)", "new Base(trace, null)", 0, 0),
            ("wrong-capture", "Outer.java", "return Outer.this.captured;", "return null;", 0, 0),
            ("lost-prefix", "Outer.java", "trace.hit(0, this, value);", ";", 0, 0),
            ("lost-suffix", "Outer.java", "trace.hit(3, this, result);", ";", 0, 0),
            ("wrong-result", "Outer.java", "return result;", "return null;", 0, 0),
            ("lost-parent-store", "Base.java", "trace.parent = this;", "trace.parent = null;", 0, 0),
            ("lost-value-store", "Base.java", "this.saved = value;", "this.saved = null;", 1, 0),
            ("lost-observer-store", "Base.java", "this.observer = trace;", "this.observer = null;", 3, 0),
            ("lost-derived-effect", "Exact.java", "trace.hit(4, this, value);", ";", 1, 0),
            ("lost-unchecked-effect", "Exact.java", "trace.unchecked(7, this, value);", ";", 3, 0),
            ("wrapped-super-failure", "Base.java", "trace.hit(2, this, value);",
             "try { trace.hit(2, this, value); } catch (java.io.IOException failure) { throw new java.io.IOException(failure); }", 0, 3),
        ]
        for label, filename, anchor, replacement, route, mode in wrong:
            text = texts[filename]
            expected_occurrences = 2 if label in {"lost-parent-store", "lost-value-store", "lost-observer-store"} else 1
            if text.count(anchor) != expected_occurrences:
                raise RuntimeError("Authored constructor exception mutation anchor changed: " + label)
            changed = work / label / filename
            changed.parent.mkdir()
            changed.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            verify.compile_local([changed], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                     name, str(route), str(mode), "0"], text=True, capture_output=True,
                                    timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned constructor closed exceptions mismatch" not in result.stderr:
                raise RuntimeError("Observer accepted an incorrect constructor program: " + label)

        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [path for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]
        d8_output = verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8-original")
        if "Signature is ignored" in d8_output:
            raise RuntimeError("D8 ignored authored generic metadata")
        dex = dexdir / "classes.dex"
        dex_sha = sha(dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an authored constructor exception definition")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        (work / "source-dex-sha256.txt").open("x").write(dex_sha + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in report["methods"]}
        if (set(rows) != methods or report["class_count"] != len(classes)
                or report["method_count"] != len(methods) or report["unrecovered_method_count"]
                or report.get("projected_method_count") != 3
                or report.get("declaration_only_method_count") != 2
                or report.get("generic_signature_projections")
                or report.get("minimum_java_release", 8) != 25):
            raise RuntimeError("Constructor exception recovery changed definition roles or source contracts")
        bindings = report.get("class_source_bindings", [])
        if len(bindings) != 1:
            raise RuntimeError("Constructor exception projection has no unique local class binding")
        binding = bindings[0]
        if (binding["class"] != OWNER or binding["binding_kind"] != "anonymous-to-named-local"
                or binding["binary_name_status"] != "changed" or not binding.get("metadata_differences")
                or not any("constructor parameters may differ" in text for text in binding["metadata_differences"])):
            raise RuntimeError("Constructor exception local binding omitted its metadata differences")
        changed = [row for row in rows.values() if "source_constructor_declared_throws" in row]
        if (len(changed) != 1 or changed[0]["identity"] != CTOR
                or changed[0]["source_constructor_declared_throws"] != CHECKED
                or changed[0]["constructor_exception_source"] != TARGET
                or not isinstance(changed[0].get("constructor_exception_call_pc"), int)
                or changed[0]["projection_kind"] != "anonymous-to-named-local"
                or not changed[0].get("metadata_differences") or changed[0].get("declared_throws")):
            raise RuntimeError("Named local constructor exception delta lacks its exact delegation proof")

        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=provider if excluded else None)
        after = class_identity.compiler_classes(rebuilt)
        after_exceptions = exception_inventory(rebuilt)
        candidates = [owner for owner, facts in after.items()
                      if facts["inner_class"] and facts["inner_class"]["name"] == binding["source_name"]
                      and facts["enclosing_method"] == records[OWNER]["enclosing_method"]]
        if len(candidates) != 1:
            raise RuntimeError("Compiled constructor local identity is not uniquely tied to its original enclosure")
        replacement = candidates[0]
        mapped = lambda text: text.replace(OWNER, replacement)
        # Java retains the implicit enclosing-instance ABI parameter in addition
        # to the original explicit capture parameter reported by the projection.
        source_ctor = mapped(CTOR).replace("-><init>(", "-><init>(Lfixture/Outer;", 1)
        source_methods = {source_ctor if identity == CTOR else mapped(row.get("source_identity", identity))
                          for identity, row in rows.items()}
        helpers = {mapped(row["class"] + "->" + row["name"] + row["prototype"])
                   for row in report.get("generated_source_helpers", [])}
        expected_helpers = {mapped(owner) + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for owner in classes}
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != {mapped(owner) for owner in classes} or helpers != expected_helpers
                or set(actual_methods) != source_methods | helpers):
            raise RuntimeError("Compiled constructor exception definitions differ beyond reported source mappings")
        for owner in classes:
            before, actual = records[owner], after[mapped(owner)]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                wanted = json.loads(mapped(json.dumps(before.get(key))))
                if owner == OWNER and key == "inner_class":
                    wanted["name"] = binding["source_name"]
                if owner == OWNER and key == "fields":
                    capture = replacement + "->this$0:Lfixture/Outer;"
                    wanted[capture]["access"] &= ~0x1000
                if wanted != actual.get(key):
                    raise RuntimeError("Constructor class metadata differs beyond disclosure: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                wanted = json.loads(mapped(json.dumps(metadata)))
                target = mapped(identity)
                declared = exceptions[identity]
                if identity == CTOR:
                    target = source_ctor
                    wanted["prototype"] = target.split("-><init>")[1]
                    declared = CHECKED
                if actual["methods"].get(target) != wanted or after_exceptions.get(target) != declared:
                    raise RuntimeError("Constructor method role or metadata differs beyond declaration proof: " + identity)
        runtime = (rebuilt, harness, provider) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Constructor projection changed capture, arguments, effects or throwable identity")
        if (hashes(originals) != frozen["originals"] or hashes(harness) != frozen["harness"]
                or hashes(work / "unencoded-originals") != frozen["unencoded_originals"] or sha(dex) != dex_sha
                or any(sha(Path(path)) != digest for path, digest in inputs.items())):
            raise RuntimeError("Frozen constructor exception input changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "all_java_units": len(list((output / "sources").rglob("*.java"))),
                   "minimum_java_release": 25, "reported_anonymous_bindings": 1,
                   "reported_constructor_exception_differences": 1,
                   "fresh_jvm_cases": 72, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "dependency_provider": args.dependency_provider}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
