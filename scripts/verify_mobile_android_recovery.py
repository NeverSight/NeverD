#!/usr/bin/env python3
"""Compile a built-in Android Java export and publish compiler-verified method coverage."""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory
import zipfile

if __package__:
    from .mobile_android_class_identity import ClassFile, require
    from .mobile_real_apps_android import (
        MAX_DUMP_BYTES, combine_inventory, dex_header, extract_dex_inputs, parse_dexdump, source_artifacts,
    )
else:
    from mobile_android_class_identity import ClassFile, require
    from mobile_real_apps_android import (
        MAX_DUMP_BYTES, combine_inventory, dex_header, extract_dex_inputs, parse_dexdump, source_artifacts,
    )


HERE = Path(__file__).resolve().parent
FIELD_TOOL = "rebuild/RestoreFieldFlags.java"
FIELD_MANIFEST = "rebuild/runtime-fields.tsv"
REFERENCE = re.compile(r"L[^;()]+;")


def read_inventory(original, dexdump, work):
    if original.suffix.lower() == ".apk":
        inputs = extract_dex_inputs(original, work / "dex")
    else:
        require(original.suffix.lower() == ".dex", "Expected an APK or DEX input")
        data = original.read_bytes()
        inputs = [{"input": original.name, "path": original, "header": dex_header(data),
                   "sha256": hashlib.sha256(data).hexdigest()}]
    descriptions = []
    for item in inputs:
        dump = work / "dexdump.txt"
        with dump.open("wb") as stream:
            subprocess.run([dexdump, "-f", "-h", "-l", "plain", item["path"]], stdout=stream, check=True)
        require(dump.stat().st_size <= MAX_DUMP_BYTES, "Android SDK declaration dump exceeds its limit")
        # Some SDK versions print supplementary characters in modified UTF-8.
        text = dump.read_bytes().decode("utf-8", "surrogatepass")
        text = text.encode("utf-16-le", "surrogatepass").decode("utf-16-le")
        descriptions.append(parse_dexdump(text, item["input"], item["header"]))
        dump.unlink()
    return combine_inventory(descriptions), {item["input"]: item["sha256"] for item in inputs}


def compiler_plugin(javac, work):
    tools = work / "tools"
    tools.mkdir()
    subprocess.run([javac, "-proc:none", "-d", tools, HERE / "mobile/RecoveryClassNames.java"], check=True)
    jar = work / "recovery-compiler.jar"
    with zipfile.ZipFile(jar, "w") as archive:
        for cls in tools.glob("RecoveryClassNames*.class"):
            archive.write(cls, cls.name)
        archive.writestr("META-INF/services/com.sun.source.util.Plugin", "RecoveryClassNames\n")
    return jar


def compile_export(javac, options, report, export, work, classes):
    coverage = report["android_method_recovery"]
    plugin = compiler_plugin(javac, work)
    names = work / "class-names.tsv"
    sources = work / "sources.args"
    with sources.open("w", encoding="utf-8") as stream:
        for name in report["java_sources"]:
            path = str(export / name).replace("\\", "\\\\").replace('"', '\\"')
            stream.write('"' + path + '"\n')
    defaults = [] if any(arg.split("=")[0] in {"--release", "--source", "-source"} for arg in options) else [
        "--release", str(coverage.get("minimum_java_release", 8))]
    require(all(option == "-Xpkginfo:always" for option in coverage.get("java_compiler_options", [])),
            "Unsupported compiler option in Android recovery metadata")
    subprocess.run([javac, *defaults, *options, *coverage.get("java_compiler_options", []),
                    "-proc:none", "-processorpath", plugin, "-Xplugin:NeverDClassNames",
                    "-J-Dneverd.class.names=" + str(names), "-encoding", "UTF-8", "-d", classes,
                    "@" + str(sources)], check=True)
    return names


def rebuild_fields(javac, export, work, classes):
    # Run the shipped metadata utility, never code supplied by the original app.
    header = (HERE.parent / "tools/neverd/mobile/MobileJavaFieldRestore.h").read_text(encoding="utf-8")
    trusted = header.split('R"neverd_java(', 1)[1].split(')neverd_java";', 1)[0]
    utility = export / FIELD_TOOL
    require(utility.read_text(encoding="utf-8") == trusted, "Unexpected field restoration utility")
    tool_classes = work / "tools"
    subprocess.run([javac, "-proc:none", "-d", tool_classes, utility], check=True)
    java = Path(javac).with_name("java.exe" if os.name == "nt" else "java")
    subprocess.run([java, "-cp", tool_classes, "RestoreFieldFlags", classes, export / FIELD_MANIFEST], check=True)


def compiled_classes(directory):
    facts, offsets = {}, {}
    for path in directory.rglob("*.class"):
        parsed = ClassFile(path.read_bytes(), read_annotations=False)
        fact = parsed.facts()
        name = fact["name"]
        require(name not in facts and path.relative_to(directory).as_posix() == name[1:-1] + ".class",
                "Compiled class path or identity mismatch")
        facts[name] = fact
        offsets[name] = parsed.method_access_offsets
    return facts, offsets


def class_names(coverage, original, compiler, path):
    owners = coverage["original_classes"]
    require(len(owners) == len(set(owners)) == coverage["class_count"]
            and set(owners) == set(original["classes"]), "Original class inventory differs from DEX")
    mapping = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        encoded, binary = line.split("\t")
        owner = bytes.fromhex(encoded).decode("utf-8")
        require(owner in original["classes"] and owner not in mapping, "Invalid compiler class origin")
        mapping[owner] = "L" + binary.replace(".", "/") + ";"
    for item in coverage.get("package_metadata_classes", []):
        name = item["class"]
        require(name not in mapping and name.endswith("/package-info;"), "Invalid package metadata origin")
        mapping[name] = name
    require(set(mapping) == set(owners) and len(set(mapping.values())) == len(mapping)
            and set(mapping.values()) == set(compiler), "Missing, duplicate or unexpected compiled class")
    projected = {row["class"] for row in coverage.get("class_source_bindings", [])}
    for name, binary in mapping.items():
        require(name == binary or name in projected, "Undisclosed source class renaming: " + name)
    for row in coverage.get("class_source_bindings", []):
        binary = mapping[row["class"]]
        cls = compiler[binary]
        if row.get("binary_name_status") == "preserved":
            require(binary == row["class"], "Expected preserved compiler binary name: " + row["class"])
        require(cls["source_file"] == Path(row["source_unit"]).name, "Compiler SourceFile disagrees with class binding")
        row["source_binary_name_status"] = row["binary_name_status"]
        row["binary_name_status"] = "preserved" if binary == row["class"] else "changed"
        row["compiled_identity"] = binary
    return mapping


def method_identity(source, classes, mapping):
    owner, tail = source.split("->", 1)
    cls = classes[mapping[owner]]
    name, rest = tail.split("(", 1)
    descriptor = REFERENCE.sub(lambda match: mapping.get(match[0], match[0]), "(" + rest)
    prefix = cls["name"] + "->" + name
    direct = prefix + descriptor
    if direct in cls["methods"]:
        return direct
    # A named local in an instance method has a javac enclosing-instance
    # parameter in addition to its explicit DEX captures.
    enc, inner = cls["enclosing_method"], cls["inner_class"]
    if name == "<init>" and enc and enc["name"] and inner and inner["name"] and not inner["access"] & 8:
        outer = classes[enc["owner"]]["methods"].get(enc["owner"] + "->" + enc["name"] + enc["prototype"])
        captured = prefix + "(" + enc["owner"] + descriptor[1:]
        if outer and not outer["access"] & 8 and captured in cls["methods"]:
            return captured
    raise ValueError("No compiled method for " + source)


def verify_methods(coverage, inventory, compiler, offsets, mapping, classes_directory):
    rows = coverage["methods"]
    require(len(rows) == coverage["method_count"] == len(inventory["methods"])
            and {row["identity"] for row in rows} == set(inventory["methods"]), "Missing or duplicate original method")
    # The bridge slot uses its DEX name. A source return-slot alias also
    # changes the name of the bridge generated by javac.
    bridge_helpers = {}
    for helper in coverage.get("generated_source_helpers", []):
        if helper["kind"] in {"compiler-covariant-interface-bridge", "compiler-parameter-interface-bridge",
                              "compiler-covariant-class-bridge", "compiler-parameter-class-bridge"}:
            key = (helper["bridge_target"], helper["class"], helper["prototype"])
            require(key not in bridge_helpers, "Duplicate generated bridge helper")
            bridge_helpers[key] = helper["class"] + "->" + helper["name"] + helper["prototype"]
    counts, kinds, seen, patches = Counter(), Counter(), set(), defaultdict(list)
    for row in rows:
        identity = row["identity"]
        original = inventory["methods"][identity]
        role = original["role"]
        require(all(original[key] == row[key] for key in ("class", "input", "name", "prototype"))
                and identity == row["class"] + "->" + row["name"] + row["prototype"],
                "Method ownership disagrees with DEX: " + identity)
        require((0 < row["instruction_count"] <= original["code_units"]) if role == "body"
                else row["instruction_count"] == 0, "Original method code/declaration mismatch: " + identity)
        status = row["status"]
        require(status in ("source-projected", "recovered" if role == "body" else "declaration-only"),
                "Unrecovered or mismatched source method: " + identity)
        if status == "source-projected":
            require(row.get("projection_kind") and row.get("reason"), "Missing source projection evidence")
            kinds[row["projection_kind"]] += 1
        counts[status] += 1
        source = row.get("source_identity", identity)
        require(source.partition("->")[0] == row["class"], "Source method changed class without a binding")
        target = method_identity(source, compiler, mapping)
        owner = mapping[row["class"]]
        members = compiler[owner]["methods"]
        require(members[target]["code"] == (role == "body"), "Compiled source body/declaration mismatch: " + identity)
        for generated in row.get("generated_bridges", []):
            declaring, signature = generated.split("->", 1)
            prototype = signature[signature.index("("):]
            helper = bridge_helpers.get((identity, declaring, prototype))
            require(helper is not None, "Missing generated bridge source binding: " + generated)
            compiled_bridge = method_identity(helper, compiler, mapping)
            bridge = compiler[mapping[declaring]]["methods"][compiled_bridge]
            require(bridge["access"] & 0x1048 == 0x1040 and bridge["code"],
                    "Missing inherited compiler bridge: " + generated)
            row.setdefault("compiled_generated_bridges", []).append(compiled_bridge)
        regenerated = row.get("regenerated_bridge")
        if regenerated:
            require(regenerated == identity, "Regenerated bridge has a different original identity")
            bridge = method_identity(regenerated, compiler, mapping)
            require(members[bridge]["access"] & 0x1040 == 0x1040 and members[bridge]["code"],
                    "Missing executable compiler bridge: " + regenerated)
            row["compiled_source_adapter"] = target
            target = bridge
        require(target not in seen, "Original methods share one compiled entry: " + identity)
        seen.add(target)
        fact = members[target]
        flags = fact["access"]
        if row.get("compiler_generated_bridge"):
            require(flags & 0x1040 == 0x1040 and fact["code"], "Expected a compiler-generated bridge: " + identity)
        original_flags = original["access_flags"]
        require(flags & 0x508 == original_flags & 0x508 and fact["code"] == (role == "body"),
                "Compiled static/native/abstract role differs from DEX: " + identity)
        # ACC_BRIDGE and ACC_SYNTHETIC are original method metadata. Access
        # control, dispatch and the Code attribute are left to javac.
        restored = (flags & ~0x1040) | (original_flags & 0x1040)
        if restored != flags:
            patches[owner].append((offsets[owner][target], flags, restored))
        row["dex_access_flags"] = original_flags
        row["compiled_access_flags"] = restored
        if restored != flags:
            row["javac_access_flags"] = flags
        row["compiled_identity"] = target
        if status == "source-projected":
            row["source_status"] = status
        row["status"] = "recovered" if role == "body" else "declaration-only"
    require(counts["source-projected"] == coverage.get("projected_method_count", 0)
            and counts["recovered"] == coverage["recovered_method_count"]
            and counts["declaration-only"] == coverage["declaration_only_method_count"]
            and coverage["unrecovered_method_count"] == 0, "Source coverage counters disagree")
    for owner, changes in patches.items():
        path = classes_directory / (owner[1:-1] + ".class")
        data = bytearray(path.read_bytes())
        for offset, before, after in changes:
            require(int.from_bytes(data[offset:offset+2], "big") == before, "Compiled access flags changed")
            data[offset:offset+2] = after.to_bytes(2, "big")
        path.write_bytes(data)
    coverage["source_projected_method_count"] = coverage.pop("projected_method_count", 0)
    coverage["source_report_status"] = coverage["status"]
    coverage["status"] = "recovered"
    coverage["schema_version"] = 2
    coverage["recovered_method_count"] = inventory["body_count"]
    coverage["declaration_only_method_count"] = inventory["native_count"] + inventory["abstract_count"]
    return {"distinct_compiled_methods": len(seen), "compiled_classes": len(compiler),
            "source_projection_kinds": dict(sorted(kinds.items())),
            "restored_method_flags": sum(map(len, patches.values())), "application_code_executed": False}


def qualify(export, output, original, dexdump, javac, options, keep_classes):
    report = json.loads((export / "report.json").read_bytes())
    coverage = report["android_method_recovery"]
    require(report["platform"] == "android" and report["status"] == "success" and
            report["backend"]["name"] == "neverd" and report["backend"]["execution"] == "builtin" and
            report["input_kind"] in {"apk", "dex"} and coverage["schema_version"] == 1 and
            coverage == json.loads((export / "metadata/android-methods.json").read_bytes()),
            "Expected an unverified built-in Android APK or DEX export")
    require(isinstance(report.get("input_code_sha256"), dict) and isinstance(report.get("java_artifact_sha256"), dict)
            and isinstance(coverage.get("original_classes"), list),
            "Re-export the input with a NeverD version that records source digests and class origins")
    require(not output.exists() and not output.is_symlink() and output != export and export not in output.parents,
            "Choose a new output directory outside the source export")
    artifacts = source_artifacts(report, export)
    fields = report.get("java_rebuild_artifacts", [])
    require(fields in ([], [FIELD_TOOL, FIELD_MANIFEST])
            and bool(fields) == bool(coverage.get("requires_java_rebuild_postprocessing")),
            "Unrecognized Android rebuild artifacts")
    expected = report["java_artifact_sha256"]
    names = [item["path"] for item in artifacts] + fields
    require(set(expected) == set(names), "Incomplete source digest inventory")

    with TemporaryDirectory(prefix=".neverd-verify-", dir=output.parent) as tmp:
        work = Path(tmp)
        verified = work / "export"
        for name in names:
            source = export / name
            require(source.is_file() and not source.is_symlink(), "Missing generated artifact: " + name)
            destination = verified / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            require(hashlib.sha256(destination.read_bytes()).hexdigest() == expected[name],
                    "Generated artifact changed since decompilation: " + name)
        with TemporaryDirectory(prefix="inventory-", dir=work) as sdk_work:
            inventory, digests = read_inventory(original, dexdump, Path(sdk_work))
        require(digests == report["input_code_sha256"] and sorted(digests) == sorted(report["input_code_files"]),
                "The supplied original DEX bytes differ from the decompiled input")
        classes = work / "classes"
        classes.mkdir()
        origins = compile_export(javac, options, report, verified, work, classes)
        if fields:
            rebuild_fields(javac, verified, work, classes)
        compiler, offsets = compiled_classes(classes)
        mapping = class_names(coverage, inventory, compiler, origins)
        summary = verify_methods(coverage, inventory, compiler, offsets, mapping, classes)
        summary["javac"] = subprocess.check_output([javac, "-version"], text=True, stderr=subprocess.STDOUT).strip()
        summary["compiled_java_sources"] = len(artifacts)
        summary["field_restoration_applied"] = bool(fields)
        coverage["compilation_verification"] = summary
        (verified / "metadata").mkdir()
        for name, data in [("metadata/android-methods.json", coverage), ("report.json", report)]:
            with (verified / name).open("w", encoding="utf-8") as stream:
                json.dump(data, stream, ensure_ascii=True, separators=(",", ":"))
                stream.write("\n")
        if keep_classes:
            classes.rename(verified / "classes")
        verified.rename(output)
    return summary


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("export", type=Path, help="NeverD mobile source export")
    parser.add_argument("--input", type=Path, required=True, help="original APK or DEX")
    parser.add_argument("-o", "--output", type=Path, required=True, help="new verified source export")
    parser.add_argument("--javac", default="javac", help="JDK compiler")
    parser.add_argument("--dexdump", default="dexdump", help="Android SDK build-tools dexdump executable")
    parser.add_argument("--keep-classes", action="store_true", help="include rebuilt class files in the export")
    split = argv.index("--") if "--" in argv else len(argv)
    args = parser.parse_args(argv[:split])
    compiler = shutil.which(args.javac)
    if not compiler or not shutil.which(args.dexdump):
        parser.error("Supply a JDK javac and the Android SDK dexdump executable")
    result = qualify(args.export.resolve(), args.output.absolute(), args.input.resolve(), args.dexdump,
                     Path(compiler).resolve(), argv[split+1:], args.keep_classes)
    print(f"Verified {result['compiled_java_sources']} Java sources, {result['compiled_classes']} classes "
          f"and {result['distinct_compiled_methods']} distinct original method entries.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (OSError, KeyError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
