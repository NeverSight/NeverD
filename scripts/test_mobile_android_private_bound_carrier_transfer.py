#!/usr/bin/env python3
"""Verify private dependent carrier bounds preserve stores, scope and identity."""
from __future__ import annotations
import argparse, hashlib, json, os, subprocess, tempfile, zipfile
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"): parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"): parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-private-bound-carrier-transfer-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        fixture = FIXTURES / "private_bound_carrier_transfer/java"
        originals, harness = work / "originals", work / "harness"
        sources = sorted(fixture.rglob("*.java"))
        verify.compile_local(sources, originals, release=8)
        verify.compile_local([FIXTURES / "harness/PrivateBoundCarrierTransferHarness.java", FIXTURES / "harness/PrivateBoundCarrierTransferCaller.java"], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 12): raise RuntimeError("Owned private bound carrier transfer original inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        excluded = {"Lfixture/Slot;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {method for method in all_methods if method.split("->", 1)[0] in classes}
        if (len(classes), len(methods)) != ((3, 11) if excluded else (4, 12)):
            raise RuntimeError("Owned private bound carrier transfer source or provider inventory changed")
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x") as archive:
            for owner in ("Lfixture/Slot;",):
                name = owner[1:-1] + ".class"
                archive.writestr(name, (originals / name).read_bytes())
        inputs = {str(path): sha(path) for path in (args.neverd.resolve(), args.android_jar.resolve(), provider)}
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded: classpath += ["--classpath", provider]

        def execute(runtime, label):
            results = []
            for route in range(4):
                for shape in range(4):
                    for mode in range(16):
                        for implementation in range(2):
                            text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                "PrivateBoundCarrierTransferHarness", str(route), str(shape), str(mode), str(implementation)], f"{label}-{route}-{shape}-{mode}-{implementation}")
                            if text != "attempts=2,private-bound-carrier-transfer=matched\n": raise RuntimeError("Original private bound carrier transfer observer changed")
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
            if "Signature is ignored" in output: raise RuntimeError("D8 ignored an original private bound carrier transfer Signature")
            return target / "classes.dex"

        source_dex = dex(originals, "dex")
        dex_hash = sha(source_dex)
        (work / "source-dex-sha256.txt").open("x").write(dex_hash + "\n")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods): raise RuntimeError("D8 changed an original private bound carrier transfer definition")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (report["status"] != "recovered" or report["unrecovered_method_count"] or report.get("projected_method_count", 0)
                or report.get("declaration_only_method_count") != (1 if excluded else 2)
                or report["recovered_method_count"] != 10
                or report["class_count"] != len(classes) or report["method_count"] != len(methods)
                or {row["identity"] for row in report["methods"]} != methods
                or report.get("class_source_bindings") or report.get("generic_signature_projections")):
            raise RuntimeError("Original private bound carrier transfer coverage or source contract changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=provider if excluded else None, release=report.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helper_rows = report.get("generated_source_helpers", [])
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in helper_rows}
        expected = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if not records[owner]["access"] & 0x0200}
        if (actual_classes != classes or set(actual_methods) != methods | expected or helpers != expected
                or len(helper_rows) != len(expected)
                or any(row.get("kind") != "throw-helper" or row.get("static") is not True for row in helper_rows)):
            raise RuntimeError("Original private bound carrier transfer definition or helper role changed")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key): raise RuntimeError("Original private bound carrier transfer class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original private bound carrier transfer method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness, *([provider] if excluded else [])), "rebuilt") != baseline:
            raise RuntimeError("Original private bound carrier transfer effects, backing storage or failures changed")
        texts = {name: (fixture / ("fixture/" + name + ".java")).read_text() for name in ("Core", "Bag")}
        wrongs = [
            ("wrong-receiver", "Core", "return subject.read(input);", "return new Core().read(input);", 0, 0, 0),
            ("lost-prefix", "Core", "Trace.hit(1, this, input);", ";", 0, 0, 0),
            ("lost-saved-store", "Core", "saved = input;", ";", 0, 0, 0),
            ("lost-put-call", "Core", "input.put((E)Trace.value);", ";", 0, 0, 0),
            ("wrong-put-value", "Core", "input.put((E)Trace.value);", "input.put((E)Trace.replacement);", 0, 0, 0),
            ("early-saved-store", "Core", "Trace.hit(1, this, input);\n        saved = input;", "saved = input; Trace.hit(1, this, input);", 0, 0, 0),
            ("lost-suffix", "Core", "Trace.hit(2, this, input);", ";", 0, 0, 0),
            ("late-result-capture", "Core", "return result;", "return Trace.replace ? (S)Trace.replacement : input;", 11, 0, 0),
            ("wrapped-prefix", "Core", "Trace.hit(1, this, input);", "try { Trace.hit(1, this, input); } catch (Throwable problem) { throw new IllegalStateException(problem); }", 6, 0, 0),
            ("lost-put-store", "Bag", "this.value = value;", ";", 0, 0, 0),
            ("early-cast", "Core", "Slot<E> result = subject.read(input);", "Bag<E> result = (Bag<E>)subject.read(input);", 0, 3, 1),
            ("lost-post-call", "Core", "Trace.hit(3, subject, input);", ";", 0, 3, 0),
            ("lost-discard", "Core", "public static void discard(Core subject, Slot<?> input) { subject.read(input); }", "public static void discard(Core subject, Slot<?> input) {}", 0, 2, 0),
        ]
        for label, owner, before, changed, mode, shape, implementation in wrongs:
            text = texts[owner]
            if not text.count(before): raise RuntimeError("Owned private bound carrier transfer mutation anchor changed")
            source = work / label / (owner + ".java")
            source.parent.mkdir(); source.open("x").write(text.replace(before, changed))
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "PrivateBoundCarrierTransferHarness", "0", str(shape), str(mode), str(implementation)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned private bound carrier transfer" not in result.stderr:
                raise RuntimeError("Frozen private bound carrier transfer observer missed " + label)
        signature = b"<E:Ljava/lang/Object;S::Lfixture/Slot<TE;>;>(TS;)TS;"
        controls = [
            ("unknown-bound", signature.replace(b"Lfixture/Slot<", b"Lfixture/Missing<")),
            ("changed-element-bound", signature.replace(b"Ljava/lang/Object;", b"Ljava/lang/Number;")),
            ("unknown-bound-formal", signature.replace(b"Slot<TE;>", b"Slot<TMissing;>")),
            ("variant-bound-slot", signature.replace(b"Slot<TE;>", b"Slot<+TE;>")),
            ("extra-formal", signature.replace(b";>(", b";U:Ljava/lang/Object;>(")),
            ("declared-throw", signature + b"^Ljava/lang/Exception;"),
            ("changed-result", signature.replace(b")TS;", b")TE;")),
            ("changed-input", signature.replace(b"(TS;)", b"(TE;)")),
        ]
        for label, changed in controls:
            source = work / (label + "-classes")
            import shutil
            shutil.copytree(originals, source)
            path = source / "fixture/Core.class"
            code = method_code(path.read_bytes())
            receipt = change_signature(path, signature, changed)
            if method_code(path.read_bytes()) != code: raise RuntimeError("Signature-only control changed Code")
            (work / (label + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            declared = classpath
            if excluded:
                jar = work / (label + "-declarations.jar")
                with zipfile.ZipFile(jar, "x") as archive:
                    for descriptor in sorted(excluded):
                        name = descriptor[1:-1] + ".class"
                        archive.writestr(name, (source / name).read_bytes())
                declared = ["--classpath", args.android_jar.resolve(), "--classpath", jar]
            data = dex(source, label + "-dex", declared)
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(data), *map(str, declared), "-o", str(rejected)],
                text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode != 1 or rejected.exists() or not result.stderr.startswith("error: "): raise RuntimeError("Unproved private bound carrier transfer Signature accepted: " + label)
        if (frozen != {"originals": hashes(originals), "harness": hashes(harness)} or sha(source_dex) != dex_hash
                or any(sha(Path(path)) != value for path, value in inputs.items())): raise RuntimeError("Frozen private bound carrier transfer inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "fresh_jvm_cases": 512,
            "attempts_per_case": 2, "dependency_provider": args.dependency_provider, "wrong_programs": len(wrongs),
            "wrong_program_labels": [row[0] for row in wrongs],
            "baseline_oracle_sha256": sha(work / "baseline.json"),
            "same_code_controls": [row[0] for row in controls], "signature_control_target": "Lfixture/Core;->read(Lfixture/Slot;)Lfixture/Slot;", "minimum_java_release": report.get("minimum_java_release", 8),
            "declaration_only_method_count": report["declaration_only_method_count"],
            "all_original_metadata_exceptions_definitions_and_helpers": "verified", "frozen_inputs": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
