#!/usr/bin/env python3
"""Verify paired generic factories retain their original varargs array and result."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"): parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"): parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-final-prefix-cleanup-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        fixture = FIXTURES / "final_prefix_cleanup/java"
        originals, harness = work / "originals", work / "harness"
        sources = sorted(fixture.rglob("*.java"))
        verify.compile_local(sources, originals, release=8)
        verify.compile_local([FIXTURES / "harness/FinalPrefixCleanupHarness.java", FIXTURES / "harness/FinalPrefixCleanupCaller.java"], harness, classpath=originals, release=8)
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (4, 13): raise RuntimeError("Owned final prefix cleanup original inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        excluded = {"Lfixture/Context;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {method for method in all_methods if method.split("->", 1)[0] in classes}
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x") as archive:
            for owner in ("Lfixture/Context;",):
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
                            "FinalPrefixCleanupHarness", str(route), str(shape), str(mode)], f"{label}-{route}-{shape}-{mode}")
                        if text != "attempts=2,final-prefix-cleanup=matched\n": raise RuntimeError("Original package factory observer changed")
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
            return target / "classes.dex"

        source_dex = dex(originals, "dex")
        dex_hash = sha(source_dex)
        (work / "source-dex-sha256.txt").open("x").write(dex_hash + "\n")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods): raise RuntimeError("D8 changed an original final prefix cleanup definition")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (report["status"] != "recovered" or report["unrecovered_method_count"] or report.get("projected_method_count", 0)
                or report["class_count"] != len(classes) or report["method_count"] != len(methods)
                or {row["identity"] for row in report["methods"]} != methods
                or report.get("class_source_bindings") or report.get("generic_signature_projections")):
            raise RuntimeError("Original final prefix cleanup coverage or source contract changed")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=provider if excluded else None, release=report.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in report.get("generated_source_helpers", [])}
        expected = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if not records[owner]["access"] & 0x0200}
        if actual_classes != classes or set(actual_methods) != methods | expected or helpers != expected:
            raise RuntimeError("Original final prefix cleanup definition or helper role changed")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key): raise RuntimeError("Original final prefix cleanup class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original final prefix cleanup method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness, *([provider] if excluded else [])), "rebuilt") != baseline:
            raise RuntimeError("Original final prefix cleanup effects, backing storage or failures changed")
        text = (fixture / "fixture/Core.java").read_text()
        wrongs = [
            ("lost-first-producer", "first = context.first(this, a);", "first = null;", 0),
            ("lost-second-producer", "second = context.second(this, b);", "second = null;", 0),
            ("wrong-first-input", "context.first(this, a)", "context.first(this, b)", 0),
            ("wrong-second-input", "context.second(this, b)", "context.second(this, a)", 0),
            ("lost-enter", "context.enter(this);", ";", 0),
            ("lost-choice", "context.choose(this, choice)", "choice", 0),
            ("lost-step", "context.step(this);", ";", 0),
            ("lost-cleanup", "context.cleanup(this);", ";", 0),
            ("lost-after", "context.after(this);", ";", 0),
            ("wrong-cleanup-owner", "context.cleanup(this);", "context.cleanup(null);", 0),
            ("wrapped-step", "context.step(this);", "{ try { context.step(this); } catch (Throwable problem) { throw new IllegalStateException(problem); } }", 6),
            ("late-first-store", "first = context.first(this, a);", "Object held = context.first(this, a);", 0),
        ]
        for label, before, changed, mode in wrongs:
            if not text.count(before): raise RuntimeError("Owned final prefix cleanup mutation anchor changed")
            source = work / label / "Core.java"
            source.parent.mkdir()
            replacement = text.replace(before, changed)
            if label == "late-first-store": replacement = replacement.replace("second = context.second(this, b);", "second = context.second(this, b); first = held;")
            source.open("x").write(replacement)
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "FinalPrefixCleanupHarness", "0", "1", str(mode)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned final prefix cleanup" not in result.stderr:
                raise RuntimeError("Frozen final prefix cleanup observer missed " + label)
        controls = []
        if (frozen != {"originals": hashes(originals), "harness": hashes(harness)} or sha(source_dex) != dex_hash
                or any(sha(Path(path)) != value for path, value in inputs.items())): raise RuntimeError("Frozen final prefix cleanup inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "fresh_jvm_cases": 128,
            "attempts_per_case": 2, "dependency_provider": args.dependency_provider, "wrong_programs": len(wrongs),
            "same_code_controls": [row[0] for row in controls], "minimum_java_release": report.get("minimum_java_release", 8),
            "all_original_metadata_exceptions_definitions_and_helpers": "verified", "frozen_inputs": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
