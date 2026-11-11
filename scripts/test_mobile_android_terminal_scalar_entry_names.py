#!/usr/bin/env python3
"""Verify fixed scalar names beside a parameter-independent terminal consumer."""
from __future__ import annotations
import argparse
import copy
from contextlib import nullcontext
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import method_code, class_records, change_signature
    from scripts.test_mobile_android_protected_array_callbacks import change_method_flags
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import method_code, class_records, change_signature
    from test_mobile_android_protected_array_callbacks import change_method_flags

OWNER = "Lfixture/Core;"
BRIDGE = OWNER + "->add(Ljava/lang/Object;)Z"
ENTRIES = {"byteEntry": "(B)Z", "longEntry": "(J)J", "doubleEntry": "(D)D", "rejectInt": "(I)Z"}
NAME = "entry-owned"
MAIN = "TerminalScalarEntryNamesHarness"
SOURCE_SIGNATURE = b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/Object;>;Lfixture/Marker;"
INPUT_SIGNATURE = b"Ljava/lang/Object;Lfixture/Api<Lfixture/Core;>;Lfixture/Marker;"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-support", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-terminal-scalar-entry-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "terminal_scalar_entry_names/java"
        sources = sorted(fixture.rglob("*.java"))
        observers = [FIXTURES / "harness" / ("TerminalScalarEntryNames" + n + ".java") for n in ("Caller", "Harness")]
        sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        source_hashes = {str(p): sha(p) for p in [*sources, *observers]}
        authored, originals, harness = work / "authored", work / "originals", work / "harness"
        verify.compile_local(sources, authored, release=8)
        verify.compile_local(observers, harness, classpath=authored, release=8)
        shutil.copytree(authored, originals)
        target = originals / "fixture/Core.class"
        edits = [change_signature(target, SOURCE_SIGNATURE, INPUT_SIGNATURE),
                 change_method_flags(target, b"(Ljava/lang/Object;)Z", 0x1, 0x1041, name=b"add")]
        spelling = {}
        observed = {name: 0 for name in ENTRIES}
        for path in sorted(originals.rglob("*.class")):
            before = path.read_bytes()
            after = before
            changed_names = []
            for name in ENTRIES:
                pattern = b"\x01" + len(name).to_bytes(2, "big") + name.encode()
                count = after.count(pattern)
                if count not in (0, 1):
                    raise RuntimeError("Owned accessor UTF8 entry is not unique")
                if count:
                    after = after.replace(pattern, b"\x01" + len(NAME).to_bytes(2, "big") + NAME.encode(), 1)
                    observed[name] += 1
                    changed_names.append(name)
            expected_code = {}
            for identity, code in method_code(before).items():
                old_name, prototype = identity.split(b"(", 1)
                expected_code[(NAME.encode() if old_name.decode() in ENTRIES else old_name) + b"(" + prototype] = code
            if (method_code(after) != expected_code
                    or before[class_records(before)["pool_end"]:] != after[class_records(after)["pool_end"]:]):
                raise RuntimeError("Accessor spelling changed Code, flags, descriptors or attributes")
            if changed_names:
                path.write_bytes(after)
                spelling[path.relative_to(originals).as_posix()] = {
                    "names": changed_names, "before_sha256": hashlib.sha256(before).hexdigest(), "after_sha256": sha(path)}
        if set(spelling) != {"fixture/Core.class", "fixture/Client.class"} or set(observed.values()) != {2}:
            raise RuntimeError("Accessor spelling missed a declaration or symbolic caller")
        for path in originals.rglob("*.class"):
            original_code = method_code((authored / path.relative_to(originals)).read_bytes())
            expected_code = {}
            for identity, code in original_code.items():
                name, prototype = identity.split(b"(", 1)
                expected_code[(NAME.encode() if name.decode() in ENTRIES else name) + b"(" + prototype] = code
            if method_code(path.read_bytes()) != expected_code:
                raise RuntimeError("Owned input encoding changed an original Code attribute")
        encoding = {"signature_and_flags": edits, "method_names": spelling,
                    "all_original_Code_and_descriptors": "unchanged; only disclosed UTF8 names, Signature and method flags differ"}
        (work / "encoding-witness.json").write_text(json.dumps(encoding, indent=2) + "\n")
        frozen = {"authored": hashes(authored), "originals": hashes(originals), "harness": hashes(harness), "sources": source_hashes}
        (work / "frozen-input-hashes.json").write_text(json.dumps(frozen, indent=2) + "\n")
        sdk_hash, cli_hash = sha(args.android_jar), sha(args.neverd)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 5 or len(all_methods) != 16:
            raise RuntimeError("Original terminal scalar entry compiler inventory changed")
        external = {"Lfixture/Api;", "Lfixture/Marker;", "Lfixture/Trace;"} if args.dependency_support else set()
        classes = all_classes - external
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-support.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for name in ("Api", "Marker", "Trace"):
                jar.writestr("fixture/" + name + ".class", (originals / ("fixture/" + name + ".class")).read_bytes())
        dependency_hash = sha(dependency)
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_support:
            classpath += ["--classpath", dependency]

        def execute(runtime, names, label):
            outputs = []
            for route in range(4):
                for mode in range(16):
                    out = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), MAIN, str(route), str(mode), *names],
                                     f"{label}-{route}-{mode}")
                    if out != "attempts=2,terminal-scalar-entries=matched\n":
                        raise RuntimeError("Frozen terminal scalar entry observer output changed")
                    outputs.append(out)
            return outputs

        baseline = execute((authored, harness), list(ENTRIES), "authored")
        if execute((originals, harness), [NAME] * len(ENTRIES), "encoded") != baseline:
            raise RuntimeError("Accessor name/flag/Signature encoding changed original behavior")
        (work / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        mutations = [
            ("Core", "return (value & 255) >= 128;", "return false;", 0),
            ("Core", "return (value & 255) >= 128;", "return value >= 128;", 0),
            ("Core", "Long.rotateLeft(value, 17)", "Long.rotateLeft((int) value, 17)", 0),
            ("Core", "Long.rotateLeft(value, 17)", "Long.rotateLeft(value, 32)", 0),
            ("Core", " ^ 0x123456789abcdef0L", "", 0),
            ("Core", "return value;", "return Double.longBitsToDouble(Double.doubleToLongBits(value));", 2),
            ("Core", "return value;", "return (float) value;", 0),
            ("Core", 'throw new UnsupportedOperationException("owned scalar consumer");', "return false;", 0),
            ("Core", 'throw new UnsupportedOperationException("owned scalar consumer");',
             'throw new UnsupportedOperationException("different owned failure");', 0),
            ("Core", 'throw new UnsupportedOperationException("owned erased consumer");', "return false;", 0),
            ("Core", 'throw new UnsupportedOperationException("owned erased consumer");',
             'Core ignored = (Core) value; throw new UnsupportedOperationException("owned erased consumer");', 2),
            ("Trace", "if (step == failAt)", "if (step == failAt && step != 3)", 8),
        ]
        for index, (owner, anchor, replacement, mode) in enumerate(mutations):
            source = (fixture / ("fixture/" + owner + ".java")).read_text()
            if source.count(anchor) != 1:
                raise RuntimeError("Independent terminal scalar wrong-program anchor changed")
            changed = work / (f"wrong-{index}-source/" + owner + ".java")
            changed.parent.mkdir()
            changed.write_text(source.replace(anchor, replacement, 1))
            wrong = work / f"wrong-{index}-classes"
            verify.compile_local([changed], wrong, classpath=authored, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, authored, harness))), MAIN, "1", str(mode), *ENTRIES],
                                    text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / f"wrong-{index}-counterexample.log").write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned terminal scalar entry names" not in result.stderr:
                raise RuntimeError("Independent observer missed terminal scalar wrong program " + str(index))
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8-original")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original terminal scalar entry DEX set changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 invented or omitted an original terminal scalar entry entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-body-disassembly")
        witnesses = {}
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            name = re.search(r"(?m)^\s+name\s+: '([^']+)'", block[2])
            proto = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != OWNER or not name or not proto:
                continue
            identity = OWNER + "->" + name[1] + proto[1]
            instructions = re.findall(r"\|[0-9a-fA-F]+:\s+([^\n]+)", block[2])
            if name[1] == NAME:
                prototype = proto[1]
                flags = re.search(r"(?m)^\s+access\s+: 0x([0-9a-fA-F]+)", block[2])
                if prototype not in ENTRIES.values() or not flags or int(flags[1], 16) != 1:
                    raise RuntimeError("Original fixed scalar descriptor/access witness changed")
                opcodes = [op.split()[0] for op in instructions]
                if prototype == "(I)Z":
                    if (opcodes != ["new-instance", "const-string", "invoke-direct", "throw"]
                            or "Ljava/lang/UnsupportedOperationException;." not in instructions[2]):
                        raise RuntimeError("Original typed terminal entry witness changed")
                elif (sum("Lfixture/Trace;.hit:(I)V" in op for op in instructions) != 1
                        or opcodes[-1] != ("return" if prototype == "(B)Z" else "return-wide")):
                    raise RuntimeError("Original scalar effect and result witness changed")
                witnesses[identity] = instructions
            elif identity == BRIDGE:
                if ([op.split()[0] for op in instructions] != ["new-instance", "const-string", "invoke-direct", "throw"]
                        or "Ljava/lang/UnsupportedOperationException;." not in instructions[2]):
                    raise RuntimeError("Original parameter-independent terminal bridge witness changed")
                witnesses[identity] = instructions
        entries = [OWNER + "->" + NAME + t for t in ENTRIES.values()]
        if set(witnesses) != {BRIDGE, *entries}:
            raise RuntimeError("Missing original terminal/accessor body witness")
        (work / "input-method-witness.json").write_text(json.dumps({"dex_sha256": sha(source_dex), "bodies": witnesses, "encoding": encoding}, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != len(ENTRIES) + 1
                or coverage["status"] != "partial" or coverage.get("class_source_bindings")):
            raise RuntimeError("Terminal scalar entry recovery changed original entries or roles")
        projection = coverage.get("generic_signature_projections", [])
        if (len(projection) != 1 or projection[0].get("declaration") != OWNER
                or projection[0].get("original_signature") != INPUT_SIGNATURE.decode()
                or len(projection[0].get("changes", [])) != 1):
            raise RuntimeError("Missing exact terminal scalar source-scope receipt")
        change = projection[0]["changes"][0]
        if (change.get("kind") != "erased-terminal-consumer-raw-interface" or change.get("scope_origin") != BRIDGE
                or change.get("source_type") != "Lfixture/Api;"
                or rows[BRIDGE].get("projection_kind") != "erased-terminal-consumer-bridge"):
            raise RuntimeError("Terminal source scope lost its canonical original bridge")
        names = []
        for identity in entries:
            row = rows[identity]
            if (row.get("projection_kind") != "virtual-method-identifier" or not row.get("source_name")
                    or row.get("source_name_declaration") != identity or row.get("source_name_family") != [identity]
                    or len(row.get("metadata_differences", [])) != 3
                    or row["metadata_differences"][2] != "method access flags, descriptors, Signatures and bodies remain unchanged; retained compiler annotations still describe original runtime names"):
                raise RuntimeError("Pure entry lacks its exact independent name receipt")
            names.append(row["source_name"])
        if len(set(names)) != len(ENTRIES):
            raise RuntimeError("Fixed scalar overloads did not retain independent Java names")
        (work / "disclosed-metadata-differences.json").write_text(json.dumps({"scope": projection, "methods": {key: rows[key] for key in [BRIDGE, *entries]}}, indent=2) + "\n")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             classpath=dependency if args.dependency_support else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        def mapped(identity):
            return identity.split("->", 1)[0] + "->" + rows[identity].get("source_name", rows[identity]["name"]) + identity[identity.index("("):]
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes
                            if any(m["code"] for m in records[c]["methods"].values())}
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != {mapped(i) for i in methods} | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Terminal scalar entry recovery omitted or invented declarations/helpers")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for c in classes:
            expected = copy.deepcopy(records[c])
            if c == OWNER:
                expected["signature"] = None
            for key in expected:
                if key not in ("sha256", "path", "size", "methods") and after[c].get(key) != expected[key]:
                    raise RuntimeError("Undisclosed terminal scalar class/field metadata change: " + c + ":" + key)
            for identity, metadata in expected["methods"].items():
                wanted = copy.deepcopy(metadata)
                if identity == BRIDGE:
                    wanted["access"] &= ~0x1040
                elif identity in entries:
                    wanted["name"] = rows[identity]["source_name"]
                if after[c]["methods"].get(mapped(identity)) != wanted or after_exceptions[mapped(identity)] != exceptions[identity]:
                    raise RuntimeError("Undisclosed original method/Exceptions change: " + identity)
        for identity in expected_helpers:
            fact = after[identity.split("->", 1)[0]]["methods"][identity]
            if (fact["access"] != 0xA or not fact["code"]
                    or fact["signature"] != "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;"):
                raise RuntimeError("Terminal scalar entry exception helper metadata changed")
        runtime = (rebuilt, harness, dependency) if args.dependency_support else (rebuilt, harness)
        if execute(runtime, names, "rebuilt") != baseline:
            raise RuntimeError("Terminal scalar entry names changed scalar values, effects or exact failures")
        if (hashes(authored) != frozen["authored"] or hashes(originals) != frozen["originals"] or hashes(harness) != frozen["harness"]
                or {str(p): sha(p) for p in [*sources, *observers]} != source_hashes
                or sha(args.android_jar) != sdk_hash or sha(args.neverd) != cli_hash or sha(dependency) != dependency_hash
                or sha(source_dex) != json.loads((work / "input-method-witness.json").read_text())["dex_sha256"]):
            raise RuntimeError("Original terminal scalar inputs or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "stages": 3, "attempts_per_case": 2,
                    "independent_counterexamples": len(mutations), "dependency_support": args.dependency_support,
                    "all_original_Code_after_exact_input_metadata_encoding": "unchanged",
                    "original_scalar_bits_effects_failures_and_Exceptions": "matched",
                    "original_metadata_after_exact_scope_names_and_bridge_flags": "matched",
                    "original_inputs_and_frozen_observers": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
