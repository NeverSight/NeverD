#!/usr/bin/env python3
"""Verify complete dependent carrier inputs and an opaque subtype-array result."""
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
    parser.add_argument("--interface-slot", action="store_true")
    args = parser.parse_args()
    if args.work_dir: args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-dependent-array-carrier-factory-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        fixture = FIXTURES / "dependent_array_carrier_factory/java"
        originals, harness = work / "originals", work / "harness"
        sources = sorted(fixture.rglob("*.java"))
        verify.compile_local(sources, originals, release=8)
        verify.compile_local([FIXTURES / ("harness/" + name + ".java") for name in
                              ("DependentArrayCarrierCaller", "DependentArrayCarrierHarness")], harness, classpath=originals, release=8)
        signature = b"<T:Ljava/lang/Object;E:TT;>(Lfixture/Token<TT;>;Lfixture/Carrier<TE;>;)Lfixture/Carrier<[TE;>;"
        target_identity = "Lfixture/Provider;->make(Lfixture/Token;Lfixture/Carrier;)Lfixture/Carrier;"
        if args.interface_slot:
            encoded = signature.replace(b"E:TT;", b"E::TT;")
            receipt = change_signature(originals / "fixture/Provider.class", signature, encoded)
            (work / "owned-bound-position.json").open("x").write(json.dumps(receipt, indent=2) + "\n")
            signature = encoded
        frozen = {"originals": hashes(originals), "harness": hashes(harness)}
        (work / "frozen-input-hashes.json").open("x").write(json.dumps(frozen, indent=2) + "\n")
        (work / "baseline-cli-sha256.txt").open("x").write(sha(args.neverd) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 10): raise RuntimeError("Owned array/list original inventory changed")
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        excluded = all_classes - {"Lfixture/Core;"} if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {method for method in all_methods if method.split("->", 1)[0] in classes}
        provider = work / "owned-provider.jar"
        with zipfile.ZipFile(provider, "x") as archive:
            for owner in sorted(all_classes - {"Lfixture/Core;"}):
                name = owner[1:-1] + ".class"
                archive.writestr(name, (originals / name).read_bytes())
        inputs = {str(path): sha(path) for path in (args.android_jar.resolve(), provider)}
        classpath = ["--classpath", args.android_jar.resolve()]
        if excluded: classpath += ["--classpath", provider]

        def execute(runtime, label):
            results = []
            for route in range(4):
                for checked in (False, True):
                    for mode in range(24):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "DependentArrayCarrierHarness", str(route), str(checked).lower(), str(mode)], f"{label}-{route}-{checked}-{mode}")
                        if text != "attempts=2,dependent-array-carrier=matched\n": raise RuntimeError("Original dependent array observer changed")
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
                or {row["identity"] for row in inventory["methods"]} != methods): raise RuntimeError("D8 changed an original array/list definition")
        (work / "inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        report = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if (report["status"] != ("partial" if args.interface_slot and not excluded else "recovered") or report["unrecovered_method_count"] or report.get("projected_method_count", 0)
                or report["class_count"] != len(classes) or report["method_count"] != len(methods)
                or {row["identity"] for row in report["methods"]} != methods
                or report.get("class_source_bindings")):
            raise RuntimeError("Original array/list coverage or source contract changed")
        projections = report.get("generic_signature_projections", [])
        if args.interface_slot and not excluded:
            if (len(projections) != 1 or projections[0]["declaration"] != target_identity
                    or len(projections[0]["changes"]) != 1
                    or projections[0]["changes"][0]["kind"] != "type-variable-bound-position"):
                raise RuntimeError("Dependent method bound spelling change was not disclosed exactly")
        elif projections:
            raise RuntimeError("Canonical/dependency invocation introduced a source projection")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=provider if excluded else None, release=report.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in report.get("generated_source_helpers", [])}
        expected = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes if owner not in {"Lfixture/Token;", "Lfixture/Carrier;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected or helpers != expected:
            raise RuntimeError("Original array/list definition or helper role changed")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key): raise RuntimeError("Original array/list class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                wanted = copy.deepcopy(metadata)
                if identity == target_identity and args.interface_slot:
                    wanted["signature"] = signature.replace(b"E::TT;", b"E:TT;").decode()
                if after[owner]["methods"].get(identity) != wanted or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original array/list method metadata or Exceptions changed: " + identity)
        if execute((rebuilt, harness, *([provider] if excluded else [])), "rebuilt") != baseline:
            raise RuntimeError("Original array/list effects, backing storage or failures changed")
        texts = {name: (fixture / ("fixture/" + name + ".java")).read_text() for name in ("Core", "Provider")}
        wrongs = [
            ("lost-token-call", "Provider", "Object first = token.read();", "Object first = null;", 0),
            ("repeated-token-call", "Provider", "Object first = token.read();", "token.read(); Object first = token.read();", 0),
            ("lost-input-call", "Provider", "Object second = input.read();", "Object second = null;", 0),
            ("late-result", "Provider", "return result;", "return (Carrier<E[]>)Trace.output;", 19),
            ("inferred-result", "Provider", "return result;", "return (Carrier<E[]>)(Object)token;", 0),
            ("invented-result-read", "Core", "Carrier result = Provider.make(token, input);", "Carrier result = Provider.make(token, input); result.read();", 0),
            ("swapped-inputs", "Core", "Provider.make(token, input)", "Provider.make((Token)(Object)input, (Carrier)(Object)token)", 0),
            ("lost-suffix", "Core", "Trace.hit(5, token, input, null, result);", ";", 0),
        ]
        for label, owner, before, changed, mode in wrongs:
            text = texts[owner]
            if not text.count(before): raise RuntimeError("Owned bounded array mutation anchor changed")
            source = work / label / (owner + ".java")
            source.parent.mkdir()
            replacement = text.replace(before, changed)
            source.open("x").write(replacement)
            compiled = work / (label + "-classes")
            verify.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                "DependentArrayCarrierHarness", "0", "false", str(mode)], text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned dependent array carrier" not in result.stderr:
                raise RuntimeError("Frozen bounded array observer missed " + label)
        controls = [
            ("nonobject-root", signature.replace(b"T:Ljava/lang/Object;", b"T:Ljava/lang/Number;")),
            ("variant-root", signature.replace(b"Token<TT;>", b"Token<+TT;>")),
            ("variant-subtype", signature.replace(b"Carrier<TE;>", b"Carrier<+TE;>")),
            ("variant-result", signature.replace(b"Carrier<[TE;>", b"Carrier<+[TE;>")),
            ("wrong-component", signature.replace(b"Carrier<[TE;>", b"Carrier<[TT;>")),
            ("extra-formal", signature.replace(b"E::TT;>" if args.interface_slot else b"E:TT;>",
                b"E::TT;U:Ljava/lang/Object;>" if args.interface_slot else b"E:TT;U:Ljava/lang/Object;>")),
            ("declared-throw", signature + b"^Ljava/lang/Exception;"),
            ("unbound-component", signature.replace(b"Carrier<[TE;>", b"Carrier<[TMissing;>")),
        ]
        for label, changed in controls:
            source = work / (label + "-classes")
            import shutil
            shutil.copytree(originals, source)
            path = source / "fixture/Provider.class"
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
            if result.returncode != 1 or rejected.exists() or not result.stderr.startswith("error: "): raise RuntimeError("Unproved bounded array Signature accepted: " + label)
        if (frozen != {"originals": hashes(originals), "harness": hashes(harness)} or sha(source_dex) != dex_hash
                or any(sha(Path(path)) != value for path, value in inputs.items())): raise RuntimeError("Frozen array/list inputs changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods), "fresh_jvm_cases": len(baseline), "interface_slot": args.interface_slot,
            "attempts_per_case": 2, "dependency_provider": args.dependency_provider, "wrong_programs": len(wrongs),
            "same_code_controls": [row[0] for row in controls], "minimum_java_release": report.get("minimum_java_release", 8),
            "all_original_metadata_exceptions_definitions_and_helpers": "verified; bound position disclosed", "frozen_inputs": "unchanged"}
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__": raise SystemExit(main())
