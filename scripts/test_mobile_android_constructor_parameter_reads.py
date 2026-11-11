#!/usr/bin/env python3
"""Verify static reads into constructor parameter registers before guarded delegation."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_synthetic_method_names import KotlinFixtureClassFile


class GuardedClassFile(KotlinFixtureClassFile):
    SCHEMAS = {**KotlinFixtureClassFile.SCHEMAS, "Lkotlin/jvm/JvmOverloads;": {}}


def compiler_facts(root: Path) -> dict:
    result = {}
    for path in sorted(root.rglob("*.class")):
        row = GuardedClassFile(path.read_bytes()).facts()
        if row["name"] in result or path.relative_to(root).as_posix() != row["name"][1:-1] + ".class":
            raise RuntimeError("Duplicate/misplaced original compiler class")
        result[row["name"]] = row
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--kotlin-lib", type=Path, required=True)
    parser.add_argument("--kotlin-java-home", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-constructor-parameter-reads-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "constructor_parameter_reads"
        original_java, originals, harness = work / "original-java", work / "originals", work / "harness"
        kotlin = args.kotlin_lib.resolve()
        compiler_jars = sorted(kotlin.glob("*.jar"))
        if not (kotlin / "kotlin-compiler.jar").is_file() or not (kotlin / "kotlin-stdlib.jar").is_file():
            raise RuntimeError("The original witness requires the unmodified Kotlin compiler distribution")
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
        kotlin_jdk = choose_jdk(args.kotlin_java_home)
        verify.run([kotlin_jdk / "bin" / ("java.exe" if os.name == "nt" else "java"), "-cp", kotlin / "*", "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
                    "-jdk-home", kotlin_jdk,
                    "-no-stdlib", "-no-reflect", "-classpath", os.pathsep.join(map(str, (stdlib, original_java))),
                    "-jvm-target", "1.8", "-d", originals, *sorted((fixture / "kotlin").rglob("*.kt"))], "kotlinc-original")
        for path in sorted(original_java.rglob("*.class")):
            destination = originals / path.relative_to(original_java)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        verify.compile_local([FIXTURES / "harness" / (name + ".java") for name in
                              ("ConstructorParameterReadHarness", "ConstructorParameterReadCaller")], harness,
                             classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 10):
            raise RuntimeError("Owned constructor parameter read inventory changed: " + repr((len(all_classes), len(all_methods))))
        providers = {"Lfixture/Base;", "Lfixture/Provider;", "Lfixture/Defaults;"}
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = compiler_facts(originals), exception_inventory(originals)
        dependency = work / "owned-parent.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                jar.writestr(name, (originals / name).read_bytes())
        if args.dependency_provider:
            dependencies.append(dependency)
        dependency_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
        classpath = ["--classpath", args.android_jar.resolve()]
        for path in dependencies:
            classpath += ["--classpath", path]
        synthetic = {identity for identity, row in records["Lfixture/Core;"]["methods"].items() if row["access"] & 0x1000}
        default = "Lfixture/Core;-><init>(IILkotlin/jvm/internal/DefaultConstructorMarker;)V"
        if synthetic != {default} or records["Lfixture/Core;"]["methods"][default]["access"] != 0x1001:
            raise RuntimeError("Original Kotlin default constructor flags changed")

        def execute(runtime, label, original):
            cases = [(route, level, -1) for route in range(4) for level in range(2)]
            cases += [(route, 0, mask) for route in (2, 3) for mask in range(4)]
            result = []
            for route, level, mask in cases:
                for mode in range(13):
                    output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (*runtime, *dependencies))),
                        "ConstructorParameterReadHarness", str(route), str(level), str(mask), str(mode), str(original).lower()],
                        f"{label}-{route}-{level}-{mask}-{mode}")
                    expected = f"route={route},level={level},mask={mask},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                        raise RuntimeError("Original constructor parameter read matrix changed: " + output)
                    result.append(output)
            return result

        baseline = execute((originals, harness), "baseline", True)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned constructor parameter read DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original guarded constructor runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-delegation-disassembly")
        witnesses = set()
        for block in re.finditer(r"(?ms)^\s+#\d+\s+:\s+\(in ([^)]+)\)\s*\n(.*?)(?=^\s+#\d+\s+:\s+\(in |^Class #|\Z)", disassembly):
            kind = re.search(r"(?m)^\s+type\s+: '([^']+)'", block[2])
            if block[1] != "Lfixture/Core;" or not kind:
                continue
            if kind[1] == default.split("<init>", 1)[1]:
                ops = re.findall(r"\|[0-9a-fA-F]+:\s+(\S+)", block[2])
                registers = re.search(r"(?m)^\s+registers\s+: (\d+)", block[2])
                read = re.search(r"\|[0-9a-fA-F]+:\s+sget-object v(\d+), Lfixture/Defaults;.provider:", block[2])
                if (ops != ["and-int/lit8", "if-eqz", "sget-object", "invoke-virtual", "move-result", "invoke-direct", "return-void"]
                        or not registers or int(registers[1]) != 4 or not read or int(read[1]) != 1):
                    raise RuntimeError("Kotlin/D8 changed the parameter-overwriting static read witness")
                witnesses.add("guarded-parameter-read")
        if witnesses != {"guarded-parameter-read"}:
            raise RuntimeError("Missing an original guarded parameter read witness")
        annotations = verify.run([verify.dexdump, "-a", source_dex], "sdk-annotations")
        if "VISIBILITY_BUILD" in annotations or re.findall(r"(?m)^\s+VISIBILITY_RUNTIME (L[^;]+;)", annotations) != ["Lkotlin/Metadata;"]:
            raise RuntimeError("D8 changed Kotlin annotation retention")
        # D8 omits these original CLASS-retained compiler annotations; they
        # are absent from the actual recovery input, independently checked above.
        def dex_metadata(value):
            if isinstance(value, dict):
                return {k: dex_metadata(v) for k, v in value.items() if k not in
                        {"runtime_invisible_annotations", "runtime_invisible_parameter_annotations"}}
            if isinstance(value, list):
                return [dex_metadata(v) for v in value]
            return value
        output = work / "recovered"
        # The complete Kotlin declaration world exceeds the default 20M work
        # ceiling in both baseline and current builds. Request the existing
        # bounded 600-second/40M-work profile without changing global limits.
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath,
                    "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {row["identity"]: row for row in coverage["methods"]}
        projected = {key for key, row in rows.items() if row["status"] == "source-projected"}
        if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or projected != {default} or coverage.get("projected_method_count") != 1
                or coverage["status"] != "partial" or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")
                or coverage.get("minimum_java_release") != 25 or rows[default].get("projection_kind") != "synthetic-constructor"
                or rows[default].get("source_identity") != default or len(rows[default].get("metadata_differences", [])) != 1):
            raise RuntimeError("Constructor parameter read changed inventory or omitted synthetic flag disclosure")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=25,
                             classpath=os.pathsep.join(map(str, dependencies)))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage["generated_source_helpers"]}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Constructor parameter read invented a runtime entry or omitted helper accounting")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            original, actual = dex_metadata(records[owner]), dex_metadata(after[owner])
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations"):
                if original.get(key) != actual.get(key):
                    raise RuntimeError("Original constructor parameter read class metadata changed: " + owner + ":" + key)
            for method, metadata in original["methods"].items():
                wanted = copy.deepcopy(metadata)
                if method == default:
                    wanted["access"] &= ~0x1000
                if actual["methods"].get(method) != wanted or after_exceptions.get(method) != exceptions[method]:
                    raise RuntimeError("Original constructor parameter read metadata changed: " + method)
        if execute((rebuilt, harness), "rebuilt", False) != baseline:
            raise RuntimeError("Constructor parameter read changed original values, identities or failures")
        # These independent constructors keep the primary body but bypass or
        # repeat the getter. The unchanged observer must detect both mistakes.
        for label, expression in (("bypass", "Trace.result"), ("duplicate", "repeated()")):
            wrong = work / label / "fixture/Core.java"
            wrong.parent.mkdir(parents=True)
            wrong.write_text("""package fixture;
public final class Core extends Base {
 public final int value;
 public Core(int value) { super(value); this.value = value; Trace.core = this; Trace.hit(5); }
 public Core(int value, int mask, kotlin.jvm.internal.DefaultConstructorMarker marker) {
  this((mask & 1) != 0 ? """ + expression + """ : value);
 }
 public Core() { this(0, 1, null); }
 private static int repeated() { Defaults.provider.read(); return Defaults.provider.read(); }
}
""")
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=os.pathsep.join(map(str, (originals, *dependencies))), release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness, *dependencies))),
                "ConstructorParameterReadHarness", "2", "0", "1", "0", "false"], text=True, capture_output=True,
                timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "constructor parameter read changed" not in result.stderr:
                raise RuntimeError("Independent observer did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != h for p, h in input_hashes.items())
                or any(hashlib.sha256(Path(p).read_bytes()).hexdigest() != h for p, h in dependency_hashes.items())):
            raise RuntimeError("Original inputs, compiler, immutable callers or dependencies changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "fresh_jvm_cases": len(baseline), "dependency_provider": args.dependency_provider,
                    "original_dex_metadata_and_exceptions": "unchanged except disclosed synthetic flag",
                    "default_mask_register_reuse_dispatch_identity_and_null_order": "matched", "original_exception_identity": "matched",
                    "original_compiler_branches": "independently witnessed", "coverage": "partial", "minimum_java_release": 25,
                    "independent_bypass_and_repeated_getter_counterexamples": "detected", "attempts_per_case": 2, "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
