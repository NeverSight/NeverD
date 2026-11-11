#!/usr/bin/env python3
"""Verify direct superclass formals retain exact incoming raw nominal references."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--wildcard-target", choices=("any", "closed"))
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-raw-nominal-superclass-constructor-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "raw_nominal_superclass_constructor"
        originals, harness = work / "originals", work / "harness"
        name = "RawNominalSuperclassConstructorHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        inputs = sorted((fixture / "java").rglob("*.java"))
        if args.wildcard_target:
            inputs = [p for p in inputs if p.name not in ("Child.java", "Core.java")]
            inputs += [fixture / ("variants/" + args.wildcard_target + "_target/java/fixture/Child.java"),
                       fixture / "variants/scoped_target/java/fixture/Core.java"]
        compiler.compile_local(inputs, originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / (name + ".java")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (6, 10):
            raise RuntimeError("Owned raw nominal superclass constructor inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        signature = "(TA;TB;Lfixture/Config;)V"
        identity = "Lfixture/Base;-><init>(Ljava/lang/Object;Ljava/lang/Object;Lfixture/Config;)V"
        if facts["Lfixture/Base;"]["methods"][identity]["signature"] != signature:
            raise RuntimeError("Original closed invariant context Signature changed")
        providers = {"Lfixture/Base;", "Lfixture/Sink;", "Lfixture/Config;", "Lfixture/Trace;"}
        excluded = providers if args.dependency_provider else set()
        classes = all_classes - excluded
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-provider.jar"

        def write_provider(destination, source):
            with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as jar:
                for owner in sorted(providers):
                    entry = owner[1:-1] + ".class"
                    jar.writestr(entry, (source / entry).read_bytes())

        write_provider(dependency, originals)
        dependency_hash = sha(dependency)

        def execute(runtime, method):
            outputs = []
            for route in range(5):
                for mode in range(12):
                    text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(mode)],
                                      f"{method}-{route}-{mode}")
                    if text != "attempts=2,raw-nominal-superclass-constructor=matched\n":
                        raise RuntimeError("Frozen raw nominal superclass constructor observer changed")
                    outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        texts = {p.name: p.read_text() for p in inputs}
        wrong = [('lost-ignored-construction',
          'Core.java',
          'new Child(source,target,type,config); return Trace.marker;',
          'return Trace.marker;',
          4,
          0),
         ('repeated-construction',
          'Core.java',
          'return new Child(source,target,type,config); ',
          'new Child(source,target,type,config); return new Child(source,target,type,config); ',
          0,
          0),
         ('wrong-map', 'Child.java', 'super(source,target,config);', 'super(source,null,config);', 0, 0),
         ('wrong-source', 'Child.java', 'super(source,target,config);', 'super(null,target,config);', 0, 0),
         ('wrapped-config',
          'Child.java',
          'super(source,target,config);',
          'super(source,target,new Config(config.tag,config.action));',
          0,
          0),
         ('early-type-effect',
          'Core.java',
          'return new Child(source,target,type,config);',
          'Trace.type = type; return new Child(source,target,type,config);',
          0,
          0),
         ('lost-source-store', 'Base.java', 'this.source = source;', 'this.source = null;', 0, 0),
         ('lost-target-store', 'Base.java', 'this.target = target;', 'this.target = null;', 0, 0),
         ('lost-config-store', 'Base.java', 'this.config = config;', 'this.config = null;', 0, 0),
         ('observed-map',
          'Base.java',
          'config.action.run();',
          'config.action.run(); ((java.util.Map)target).size();',
          0,
          0),
         ('observed-type', 'Child.java', 'this.type = type;', 'type.getTypeName(); this.type = type;', 0, 11),
         ('lost-callback', 'Base.java', '        config.action.run();', '        ;', 0, 0),
         ('repeated-callback',
          'Base.java',
          'config.action.run();',
          'config.action.run(); config.action.run();',
          0,
          0),
         ('swallowed-runtime',
          'Base.java',
          'config.action.run();',
          'try { config.action.run(); } catch (RuntimeException ignored) {}',
          0,
          5),
         ('replaced-error',
          'Base.java',
          'config.action.run();',
          'try { config.action.run(); } catch (Error error) { throw new AssertionError(error); }',
          0,
          6),
         ('lost-type-store', 'Child.java', 'this.type = type;', 'this.type = null;', 0, 0),
         ('lost-child-effect', 'Child.java', 'Trace.children++;', ';', 0, 0),
         ('reread-late-map',
          'Core.java',
          'return new Child(source,target,type,config).target;',
          'new Child(source,target,type,config); return Trace.nextTarget;',
          1,
          8),
         ('reread-late-source',
          'Core.java',
          'return new Child(source,target,type,config).source;',
          'new Child(source,target,type,config); return Trace.nextSource;',
          2,
          8),
         ('reread-late-type',
          'Core.java',
          'return new Child(source,target,type,config).type;',
          'new Child(source,target,type,config); return Trace.nextType;',
          3,
          8)]
        if args.wildcard_target:
            wrong = [(label, filename,
                      anchor.replace("return new Child(source,target,type,config).target;",
                                     "return (Map)new Child(source,target,type,config).target;"),
                      replacement, route, mode)
                     for label, filename, anchor, replacement, route, mode in wrong]
        for method, filename, anchor, replacement, route, mode in wrong:
            text = texts[filename]
            if anchor not in text:
                raise RuntimeError("Raw nominal superclass constructor counterexample anchor changed: " + method)
            source = work / method / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (method + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (method + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned raw nominal superclass constructor mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + method)
        classpath = ["--classpath", sdk]
        if excluded:
            classpath += ["--classpath", dependency]
        dexdir = work / "dex"
        dexdir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--release", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8-original")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed original raw nominal superclass constructor definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Raw nominal superclass constructor recovery changed definitions or generic metadata")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=8,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Raw nominal superclass constructor recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Raw nominal superclass constructor metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Raw nominal superclass constructor identity, original casts, arrays or effects changed")
        before = signature.encode()
        controls = [
            ("unbound-parent-formal", "Base", before, b"(TA;TC;Lfixture/Config;)V"),
            ("wrong-parent-erasure", "Base", before, b"(TA;Ljava/util/Map;Lfixture/Config;)V"),
            ("constructor-owned-formal", "Base", before, b"<C:Ljava/lang/Object;>(TA;TB;Lfixture/Config;)V"),
            ("generic-throws", "Base", before, before + b"^Ljava/lang/RuntimeException;"),
            ("unknown-raw-argument", "Child", b"Lfixture/Base<Ljava/lang/Object;Ljava/util/Map;>;",
             b"Lfixture/Base<Ljava/lang/Object;Lfixture/Missing;>;"),
            ("open-child-formal", "Child", b"Lfixture/Base<Ljava/lang/Object;Ljava/util/Map;>;",
             b"<U:Ljava/lang/Object;>Lfixture/Base<Ljava/lang/Object;TU;>;"),
        ]
        if args.wildcard_target:
            arguments = "**" if args.wildcard_target == "any" else "+Ljava/lang/String;-Ljava/lang/String;"
            target = "Ljava/util/Map<" + arguments + ">;"
            header = ("<T:Ljava/lang/Object;>Lfixture/Base<TT;" + target + ">;").encode()
            ctor = ("(TT;" + target + "Ljava/lang/reflect/Type;Lfixture/Config;)V").encode()
            controls = controls[:4] + [
                ("unknown-target", "Child", header, header.replace(target.encode(), b"Lfixture/Missing;")),
                ("unknown-scoped-formal", "Child", header, header.replace(b"Base<TT;", b"Base<TMissing;")),
                ("crossed-parent-slots", "Child", header, header.replace(b"Base<TT;" + target.encode(), b"Base<" + target.encode() + b"TT;")),
                ("constructor-owned-input", "Child", ctor, b"<C:Ljava/lang/Object;>" + ctor.replace(b"(TT;", b"(TC;")),
                ("unknown-input-argument", "Child", ctor, ctor.replace(target.encode(), b"Ljava/util/Map<TMissing;*>;")),
                ("constructor-generic-throws", "Child", ctor, ctor + b"^Ljava/lang/RuntimeException;"),
            ]
        for method, owner, before, changed in controls:
            negative = work / (method + "-classes")
            shutil.copytree(originals, negative)
            target = negative / ("fixture/" + owner + ".class")
            original_code = method_code(target.read_bytes())
            receipt = change_signature(target, before, changed)
            if method_code(target.read_bytes()) != original_code:
                raise RuntimeError("Signature-only control changed Code or runtime descriptors")
            (work / (method + "-metadata.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            declared = classpath
            if excluded:
                changed_dependency = work / (method + "-provider.jar")
                write_provider(changed_dependency, negative)
                declared = ["--classpath", sdk, "--classpath", changed_dependency]
            encoded = work / (method + "-dex")
            encoded.mkdir()
            verify.run([*verify.d8, "--release", "--min-api", "24", *declared, "--output", encoded,
                        *[negative / p.relative_to(originals) for p in paths]], "d8-" + method)
            rejected = work / (method + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(encoded / "classes.dex"),
                                     *map(str, declared), "-o", str(rejected)], text=True, capture_output=True,
                                    timeout=1200, env=verify.environment)
            (work / (method + "-recovery.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or not any(word in result.stderr for word in ("generic", "constructor", "scope", "throws", "bound")):
                raise RuntimeError("Raw nominal superclass constructor proof admitted unsupported metadata: " + method)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": 60, "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "wildcard_target": args.wildcard_target,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": 8}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
