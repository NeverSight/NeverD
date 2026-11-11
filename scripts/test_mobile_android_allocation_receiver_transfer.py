#!/usr/bin/env python3
"""Verify field receiver transfers retain the shared allocation value graph."""
from __future__ import annotations
import argparse, copy, hashlib, json, os, subprocess, tempfile, zipfile
from contextlib import nullcontext
from pathlib import Path
try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts": raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code



def normalize_receiver_transfer(data: bytes) -> tuple[bytes, dict]:
    """Alpha-rename two authored temporary versions without changing effects."""
    import struct, zlib
    u32 = lambda at: struct.unpack_from("<I", data, at)[0]
    string_count, strings_at = u32(56), u32(60)
    type_count, types_at = u32(64), u32(68)
    field_count, fields_at = u32(80), u32(84)
    method_count, methods_at = u32(88), u32(92)
    def string(index):
        if not 0 <= index < string_count: raise RuntimeError("Invalid authored string index")
        at = u32(strings_at + 4 * index)
        while data[at] & 0x80: at += 1
        at += 1
        return data[at:data.index(0, at)].decode("ascii")
    def type_name(index):
        if not 0 <= index < type_count: raise RuntimeError("Invalid authored type index")
        return string(u32(types_at + 4 * index))
    found = []
    for start in range(16, len(data) - 31, 4):
        words = list(struct.unpack_from("<16H", data, start))
        fixed = {0:0x0722,2:0x0808,3:25,4:0x8655,6:0x7007,7:0x9107,8:0xa207,9:0xb307,10:0xc407,11:0xd501,12:0x0776,14:0,15:0x0711}
        if any(words[i] != value for i,value in fixed.items()): continue
        registers, incoming, outgoing, tries, debug, units = struct.unpack_from("<HHHHII", data, start - 16)
        if (registers,incoming,outgoing,tries,units) != (26,17,7,0,16): continue
        if type_name(words[1]) != "Lfixture/Target;" or words[5] >= field_count or words[13] >= method_count: continue
        owner, field_type, name = struct.unpack_from("<HHI", data, fields_at + 8 * words[5])
        cls, proto, method_name = struct.unpack_from("<HHI", data, methods_at + 8 * words[13])
        if (type_name(owner),type_name(field_type),string(name)) != ("Lfixture/Owner;","Z","flag"): continue
        if (type_name(cls),string(method_name)) != ("Lfixture/Target;","<init>"): continue
        found.append((start,words))
    if len(found) != 1: raise RuntimeError("D8 changed the authored allocation temporary witness")
    start, words = found[0]; before = list(words)
    # Allocate directly into the original range-call receiver, replacing its
    # now redundant copy with nop. The old field receiver is renamed to the
    # scalar argument's otherwise undefined temporary. Every parameter, field,
    # reference, effect, instruction boundary and debug parameter stays intact.
    words[0] = 0x0022; words[2] = 0x0508; words[4] = 0x5655
    words[6] = 0; words[15] = 0x0011
    changed = bytearray(data); struct.pack_into("<16H", changed, start, *words)
    if changed[:start] != data[:start] or changed[start+32:] != data[start+32:]: raise RuntimeError("Temporary normalization changed another DEX region")
    changed[12:32] = hashlib.sha1(changed[32:]).digest()
    struct.pack_into("<I", changed, 8, zlib.adler32(changed[12:]) & 0xffffffff)
    return bytes(changed), {"status":"verified","kind":"temporary-register-alpha-renaming",
        "code_units":16,"before_words":before,"after_words":words,"instruction_boundaries_and_effects":"unchanged",
        "all_other_regions_except_checksums":"unchanged","class_and_member_definitions":"unchanged"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"): parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"): parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-allocation-receiver-transfer-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        fixture = FIXTURES / "allocation_receiver_transfer/java"
        originals, harness = work / "originals", work / "harness"
        sources = sorted(fixture.rglob("*.java"))
        verify.compile_local(sources, originals, release=8)
        verify.compile_local([FIXTURES / "harness/AllocationReceiverTransferHarness.java", FIXTURES / "harness/AllocationReceiverTransferCaller.java"], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 8): raise RuntimeError("Owned allocation receiver transfer original inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        excluded = {"Lfixture/Target;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {method for method in all_methods if method.split("->", 1)[0] in classes}
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x") as archive:
            for owner in ("Lfixture/Target;",):
                name = owner[1:-1] + ".class"
                archive.writestr(name, (originals / name).read_bytes())
        inputs = {str(path): sha(path) for path in (args.android_jar.resolve(), provider)}
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded: classpath += ["--classpath", provider]

        def execute(runtime, label):
            results = []
            for route in range(4):
                for shape in range(2):
                    for mode in range(16):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "AllocationReceiverTransferHarness", str(route), str(shape), str(mode)], f"{label}-{route}-{shape}-{mode}")
                        if text != "attempts=2,allocation-receiver-transfer=matched\n": raise RuntimeError("Original package factory observer changed")
                        results.append(text)
            return results

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        paths = [path.relative_to(originals) for path in sorted(originals.rglob("*.class"))
                 if "L" + path.relative_to(originals).as_posix()[:-6] + ";" in classes]

        def dex(source, label, declarations=None):
            target = work / label
            target.mkdir()
            output = verify.run([*verify.d8, "--debug", "--min-api", "24", *(classpath if declarations is None else declarations),
                "--output", target, *[source / path for path in paths]], label)
            if "Signature is ignored" in output: raise RuntimeError("D8 ignored an original constructor Signature")
            path = target / "classes.dex"
            before = path.read_bytes(); after, receipt = normalize_receiver_transfer(before)
            (target / "d8-before-normalization.dex").open("xb").write(before)
            path.write_bytes(after)
            (target / "register-normalization.json").open("x").write(json.dumps(receipt, indent=2) + "\n")
            return path

        source_dex = dex(originals, "dex")
        dex_hash = sha(source_dex)
        (work / "source-dex-sha256.txt").open("x").write(dex_hash + "\n")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods): raise RuntimeError("D8 changed an original allocation receiver transfer definition")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (report["status"] != "recovered" or report["unrecovered_method_count"] or report.get("projected_method_count", 0)
                or report["class_count"] != len(classes) or report["method_count"] != len(methods)
                or {row["identity"] for row in report["methods"]} != methods
                or report.get("class_source_bindings") or report.get("generic_signature_projections")):
            raise RuntimeError("Original allocation receiver transfer coverage or source contract changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=provider if excluded else None, release=report.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in report.get("generated_source_helpers", [])}
        expected = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if not records[owner]["access"] & 0x0200}
        if actual_classes != classes or set(actual_methods) != methods | expected or helpers != expected:
            raise RuntimeError("Original allocation receiver transfer definition or helper role changed")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key): raise RuntimeError("Original allocation receiver transfer class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original allocation receiver transfer method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness, *([provider] if excluded else [])), "rebuilt") != baseline:
            raise RuntimeError("Original allocation receiver transfer effects, backing storage or failures changed")
        texts = {name: (fixture / ("fixture/" + name + ".java")).read_text() for name in ("Core", "Target")}
        call = "new Target(first, second, third, fourth, option, owner.flag)"
        wrongs = [
            ("lost-field-read", "Core", call, "new Target(first, second, third, fourth, option, false)", 0),
            ("early-field-read", "Core", "return " + call + ";", "boolean captured = owner.flag; return new Target(first, second, third, fourth, option, captured);", 0),
            ("repeated-field-read", "Core", "return " + call + ";", "Target value = " + call + "; value.captured = owner.flag; return value;", 0),
            ("wrong-option", "Core", call, "new Target(first, second, third, fourth, !option, owner.flag)", 0),
            ("lost-constructor", "Core", "return " + call + ";", "return null;", 0),
            ("repeated-constructor", "Core", "return " + call + ";", call + "; return " + call + ";", 0),
            ("wrong-receiver", "Core", call, "new Target(first, second, third, fourth, option, new Owner().flag)", 0),
            ("wrong-first", "Core", call, "new Target(null, second, third, fourth, option, owner.flag)", 0),
            ("wrapped-constructor", "Core", "return " + call + ";", "try { return " + call + "; } catch (Throwable problem) { throw new IllegalStateException(problem); }", 5),
            ("lost-captured-store", "Target", "this.captured = captured;", ";", 0),
            ("lost-actual-result", "Core", "return " + call + ";", "Target value = " + call + "; return null;", 0),
            ("lost-constructor-callback", "Target", "Trace.hit(2, this);", ";", 0),
        ]
        for label, owner, before, changed, mode in wrongs:
            text = texts[owner]
            if not text.count(before): raise RuntimeError("Owned allocation receiver transfer mutation anchor changed")
            source = work / label / (owner + ".java")
            source.parent.mkdir()
            replacement = text.replace(before, changed)
            source.open("x").write(replacement)
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "AllocationReceiverTransferHarness", "0", "0", str(mode)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned allocation receiver transfer" not in result.stderr:
                raise RuntimeError("Frozen allocation receiver transfer observer missed " + label)
        def access(data, member_kind, wanted_name, flags):
            import struct
            result = bytearray(data); count = int.from_bytes(data[8:10], "big"); at = 10; index = 1; strings = {}
            while index < count:
                tag = data[at]; at += 1
                if tag == 1:
                    size = int.from_bytes(data[at:at+2], "big"); at += 2; strings[index] = data[at:at+size].decode(); at += size
                elif tag in (3,4,9,10,11,12,17,18): at += 4
                elif tag in (5,6): at += 8; index += 1
                elif tag in (7,8,16,19,20): at += 2
                elif tag == 15: at += 3
                else: raise RuntimeError("Unexpected fixture constant pool tag")
                index += 1
            at += 6; at += 2 + 2 * int.from_bytes(data[at:at+2], "big"); changed = 0
            for kind in ("field", "method"):
                entries = int.from_bytes(data[at:at+2], "big"); at += 2
                for _ in range(entries):
                    name = strings[int.from_bytes(data[at+2:at+4], "big")]
                    if kind == member_kind and name == wanted_name: struct.pack_into(">H", result, at, flags); changed += 1
                    attributes = int.from_bytes(data[at+6:at+8], "big"); at += 8
                    for _ in range(attributes): at += 6 + int.from_bytes(data[at+2:at+6], "big")
            if changed != 1: raise RuntimeError("Owned access mutation did not bind one member")
            return bytes(result)
        controls = ("field-type", "field-name", "constructor-type", "private-constructor")
        for label in controls:
            import shutil
            source = work / (label + "-classes"); shutil.copytree(originals, source)
            path = source / ("fixture/Owner.class" if label in ("field-type", "field-name") else "fixture/Target.class")
            before = method_code(path.read_bytes())
            if label == "field-type": change_signature(path, b"Z", b"I")
            elif label == "field-name": change_signature(path, b"flag", b"missingFlag")
            elif label == "constructor-type":
                descriptor = b"(" + b"Ljava/lang/Object;" * 4 + b"ZZ)V"
                data = path.read_bytes(); changed = descriptor.replace(b"ZZ)V", b"IZ)V")
                original_utf = b"\x01" + len(descriptor).to_bytes(2, "big") + descriptor
                changed_utf = b"\x01" + len(changed).to_bytes(2, "big") + changed
                if data.count(original_utf) != 1: raise RuntimeError("Constructor descriptor mutation is ambiguous")
                path.write_bytes(data.replace(original_utf, changed_utf, 1))
            else: path.write_bytes(access(path.read_bytes(), "field" if label == "private-field" else "method", "flag" if label == "private-field" else "<init>", 0x0042 if label == "private-field" else 0x0002))
            if list(before.values()) != list(method_code(path.read_bytes()).values()): raise RuntimeError("Metadata control changed original Code")
            declared = classpath
            if excluded:
                jar = work / (label + "-declarations.jar")
                with zipfile.ZipFile(jar, "x") as archive:
                    for descriptor in sorted(excluded):
                        name = descriptor[1:-1] + ".class"; archive.writestr(name, (source / name).read_bytes())
                declared = ["--classpath", args.android_jar.resolve(), "--classpath", jar]
            data = dex(source, label + "-dex", declared); rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(data), *map(str, declared), "-o", str(rejected)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode != 1 or rejected.exists() or not result.stderr.startswith("error: "): raise RuntimeError("Unproved allocation metadata accepted: " + label)
        if (frozen != {"originals": hashes(originals), "harness": hashes(harness)} or sha(source_dex) != dex_hash
                or any(sha(Path(path)) != value for path, value in inputs.items())): raise RuntimeError("Frozen allocation receiver transfer inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "fresh_jvm_cases": 128,
            "attempts_per_case": 2, "dependency_provider": args.dependency_provider, "wrong_programs": len(wrongs),
            "same_code_controls": list(controls), "minimum_java_release": report.get("minimum_java_release", 8),
            "all_original_metadata_exceptions_definitions_and_helpers": "verified", "frozen_inputs": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
