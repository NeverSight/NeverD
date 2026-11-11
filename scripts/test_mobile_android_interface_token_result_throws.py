#!/usr/bin/env python3
"""Verify interface token results retain original dispatch, opaque tokens and closed exceptions."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import copy
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_monitor_sparse_cleanup import compiler_facts
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def scoped_default_sources(work, fixture):
    """Compile a separate original with class-owned context and default dispatch."""
    source = work / "scoped-source"
    shutil.copytree(fixture / "java", source / "java")
    harness = source / "harness"
    harness.mkdir()
    for name in ("InterfaceTokenResultThrowsHarness", "InterfaceTokenResultThrowsCaller"):
        shutil.copyfile(FIXTURES / "harness" / (name + ".java"), harness / (name + ".java"))

    def replace(path, before, after, count=1):
        text = path.read_text()
        if text.count(before) != count:
            raise RuntimeError("Scoped interface fixture anchor changed: " + path.name)
        path.write_text(text.replace(before, after))

    java = source / "java/fixture"
    body = (java / "Provider.java").read_text()
    body = body.replace("public class Provider implements DerivedApi {", "public interface Api<K> {")
    body = body.replace("    public int calls;\n", "")
    body = body.replace("public <T> T fetch(Class<T> token)", "default <E> E fetch(K key,Class<E> token,boolean option)")
    body = body.replace("calls++;", "Trace.calls++;").replace("return (T)value;", "return (E)value;")
    body = body.replace("Trace.token=token;", "Trace.token=token; Trace.key=key; Trace.option=option;")
    (java / "Api.java").write_text(body)
    (java / "DerivedApi.java").write_text("""package fixture;
import java.io.IOException;
public interface DerivedApi<K> extends Api<K> {
    default <R> R fetch(K key,Class<R> token) throws IOException {
        return fetch(key,token,Trace.requested);
    }
}
""")
    (java / "Provider.java").write_text("package fixture;\npublic class Provider implements DerivedApi<Object> {}\n")
    replace(java / "Core.java", "public DerivedApi receiver;", "public DerivedApi receiver; public Object key;")
    replace(java / "Core.java", "return receiver.fetch(token);", "return (R)receiver.fetch(key,token);")
    replace(java / "Trace.java", "public static int overrides;", "public static int overrides,calls; public static Object key; public static boolean option,requested;")
    observer = harness / "InterfaceTokenResultThrowsHarness.java"
    replace(observer, "fetch(Class<T> token)", "fetch(Object key,Class<T> token,boolean option)")
    replace(observer, "super.fetch(token)", "super.fetch(key,token,option)")
    replace(observer, "for(int attempt=0;", "Trace.calls=0;\n        for(int attempt=0;")
    replace(observer, "if(core!=null)core.receiver=receiver;", "final Object key=mode==3?null:new Object();\n            if(core!=null){core.receiver=receiver;core.key=key;}\n            Trace.key=null;Trace.option=false;Trace.requested=operation==1;")
    replace(observer, "Trace.events%16==3);", "Trace.events%16==3&&Trace.key==key&&Trace.option==(operation==1));")
    replace(observer, "core.receiver=null;Trace.token=null;", "core.receiver=null;core.key=new Object();Trace.requested=!Trace.requested;Trace.token=null;")
    replace(observer, "receiver.calls", "Trace.calls")
    replace(observer, "check(Trace.produced==", "check(Trace.key==(bypass?null:key)&&Trace.option==(!bypass&&operation==1));\n            check(Trace.produced==")
    return source, harness


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar", "java-home", "compiler-java-home"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--scoped-default", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-interface-token-result-throws-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "interface_token_result_throws"
        observer_sources = FIXTURES / "harness"
        if args.scoped_default:
            fixture, observer_sources = scoped_default_sources(work, fixture)
        originals, harness = work / "originals", work / "harness"
        name = "fixture.InterfaceTokenResultThrowsHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted((fixture / "java").rglob("*.java")), originals, release=8)
        compiler.compile_local([observer_sources / (n + ".java") for n in ("InterfaceTokenResultThrowsHarness", "InterfaceTokenResultThrowsCaller")], harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (5, 9):
            raise RuntimeError("Owned interface token result throws inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        providers = {"Lfixture/Api;", "Lfixture/DerivedApi;", "Lfixture/Provider;"}
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
            for route in range(4):
                for branch in range(2):
                    for mode in range(24):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), str(branch), str(mode)],
                                          f"{method}-{route}-{branch}-{mode}")
                        if text != "attempts=2,interface-token-result-throws=matched\n":
                            raise RuntimeError("Frozen interface token result throws observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline, indent=2) + "\n")
        wrong = [('lost-call', 'Core.java', 'Object result=core.read((Class)token);', 'Object result=null;', 1, 0, 0, 0), ('repeated-call', 'InterfaceTokenResultThrowsCaller.java', 'if(operation==0) return Core.call(core,token);', 'if(operation==0) {Core.call(core,token);return Core.call(core,token);}', 1, 1, 0, 0), ('lost-token', 'Provider.java', 'Trace.token=token;', 'Trace.token=null;', 1, 0, 0, 0), ('changed-token', 'Core.java', 'receiver.fetch(token)', '(R)receiver.fetch((Class)Object.class)', 1, 0, 0, 0), ('eager-token-use', 'Provider.java', 'calls++; Trace.hit(3);', 'token.getName(); calls++; Trace.hit(3);', 1, 0, 0, 1), ('invented-token-cast', 'Provider.java', 'return (T)value;', 'return token.cast(value);', 1, 0, 0, 2), ('lost-action', 'Provider.java', 'Trace.action.run();', ';', 1, 0, 0, 0), ('wrapped-action', 'Provider.java', 'Trace.action.run();', 'try {Trace.action.run();} catch(Throwable fault){throw new IllegalStateException(fault);}', 1, 0, 0, 14), ('early-checked-read', 'Provider.java', 'Trace.action.run();', 'if(Trace.checked!=null)throw Trace.checked;Trace.action.run();', 1, 0, 0, 18), ('lost-checked-throw', 'Provider.java', 'if(Trace.checked!=null) throw Trace.checked;', ';', 1, 0, 0, 15), ('early-value-read', 'Provider.java', 'Trace.action.run(); Trace.hit(4);\n        if(Trace.checked!=null) throw Trace.checked;\n        Object value=Trace.value;', 'Object value=Trace.value;Trace.action.run(); Trace.hit(4);\n        if(Trace.checked!=null) throw Trace.checked;', 1, 0, 0, 5), ('reread-result', 'Provider.java', 'return (T)value;', 'return (T)Trace.value;', 1, 0, 0, 6), ('premature-result-cast', 'Provider.java', 'Object value=Trace.value;', 'Object value=(String)Trace.value;', 1, 0, 0, 4), ('lost-provider-continuation', 'Provider.java', 'Trace.providerAfter.run();', ';', 1, 0, 0, 16), ('lost-caller-continuation', 'Core.java', 'Trace.after.run();', ';', 2, 0, 0, 17), ('lost-dispatch', 'Core.java', 'receiver.fetch(token)', 'new Provider().fetch(token)', 1, 0, 0, 8), ('reread-receiver', 'Core.java', 'return receiver.fetch(token);', 'Object value=receiver.fetch(token);receiver.fetch(token);return (R)value;', 1, 0, 0, 20)]
        if args.scoped_default:
            def scoped_control(row):
                label, filename, anchor, replacement, *rest = row
                if filename == "Provider.java":
                    filename = "Api.java"
                    anchor = anchor.replace("calls++;", "Trace.calls++;").replace("(T)value", "(E)value").replace("(T)Trace.value", "(E)Trace.value")
                    replacement = replacement.replace("calls++;", "Trace.calls++;").replace("(T)value", "(E)value").replace("(T)Trace.value", "(E)Trace.value")
                if filename == "Core.java":
                    anchor = anchor.replace("return receiver.fetch(token);", "return (R)receiver.fetch(token);").replace("fetch(token)", "fetch(key,token)")
                    replacement = replacement.replace("fetch(token)", "fetch(key,token)").replace("fetch((Class)Object.class)", "fetch(key,(Class)Object.class)")
                return (label, filename, anchor, replacement, *rest)
            wrong = [scoped_control(row) for row in wrong]
            wrong += [
                ("lost-key", "Api.java", "Trace.key=key;", "Trace.key=null;", 1, 0, 0, 0),
                ("changed-key", "Core.java", "receiver.fetch(key,token)", "receiver.fetch(new Object(),token)", 1, 0, 0, 0),
                ("changed-option", "DerivedApi.java", "fetch(key,token,Trace.requested)", "fetch(key,token,!Trace.requested)", 1, 0, 0, 0),
            ]
        for label, filename, anchor, replacement, occurrences, route, branch, mode in wrong:
            original_source = observer_sources / filename if filename.endswith("Caller.java") else fixture / "java/fixture" / filename
            text = original_source.read_text()
            if text.count(anchor) != occurrences:
                raise RuntimeError("Interface token result throws counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(branch), str(mode)],
                                    text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned interface token result throws mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)
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
            raise RuntimeError("D8 changed original interface token result throws definitions")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "--timeout", "600", "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        minimum_java_release = coverage.get("minimum_java_release", 8)
        expected_release = 9 if args.scoped_default and not args.dependency_provider else 8
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("field_source_bindings") or coverage.get("requires_java_rebuild_postprocessing") or minimum_java_release != expected_release):
            raise RuntimeError("Interface token result throws recovery changed definitions or generic metadata")
        declarations = {m for owner in classes for m,row in facts[owner]["methods"].items() if row["access"] & 0x500}
        if ({m["identity"] for m in coverage["methods"] if m["status"] == "declaration-only"} != declarations
                or coverage["declaration_only_method_count"] != len(declarations)
                or any(m["status"] not in ("recovered", "declaration-only") for m in coverage["methods"])
                or any(m["instruction_count"] for m in coverage["methods"] if m["status"] == "declaration-only")):
            raise RuntimeError("Interface token declarations changed Code/status roles")
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        allowed = {o + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for o in classes}
        rebuilt = work / "rebuilt"
        dependencies = [dependency] if excluded else []
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, release=minimum_java_release,
                             classpath=os.pathsep.join(map(str, dependencies)) if dependencies else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        if (actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed
                or any(actual_methods[m] != all_methods[m] for m in methods)):
            raise RuntimeError("Interface token result throws recovery lost or invented runtime definitions")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            wanted, actual = copy.deepcopy(facts[owner]), copy.deepcopy(after[owner])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + minimum_java_release:
                raise RuntimeError("Interface token result throws compiler release changed")
            if actual != wanted or any(after_exceptions[m] != exceptions[m] for m in facts[owner]["methods"]):
                raise RuntimeError("Interface token result throws metadata/roles/Exceptions changed: " + owner)
        if execute((rebuilt, harness, *dependencies), "rebuilt") != baseline:
            raise RuntimeError("Interface token result throws receiver dispatch, closed context identity, callback order, result casts or exception identity changed")
        # Mutate the exact interface entry invoked by emitted Core in both
        # source and SDK modes. Dependency-only default bodies are not inputs
        # to recovery, so their unreferenced parent slots prove no rejection.
        formal = "R" if args.scoped_default else "E"
        slot = "T" + formal + ";"
        context = "TK;" if args.scoped_default else ""
        short_signature = "<" + formal + ":Ljava/lang/Object;>(" + context + "Ljava/lang/Class<" + slot + ">;)" + slot
        controls = [
            ("covariant-token", short_signature.replace("Class<" + slot + ">", "Class<+" + slot + ">")),
            ("contravariant-token", short_signature.replace("Class<" + slot + ">", "Class<-" + slot + ">")),
            ("star-token", short_signature.replace("Class<" + slot + ">", "Class<*>")),
            ("raw-token", short_signature.replace("Class<" + slot + ">", "Class")),
            ("changed-bound", short_signature.replace(formal + ":Ljava/lang/Object;", formal + ":Ljava/lang/Number;")),
            ("closed-result", short_signature.replace(")" + slot, ")Ljava/lang/Object;")),
            ("unused-formal", short_signature.replace("<" + formal + ":", "<Unused:Ljava/lang/Object;" + formal + ":")),
        ]
        if args.scoped_default:
            controls += [
                ("owner-result", short_signature.replace(")" + slot, ")TK;")),
                ("owner-token", short_signature.replace("Class<" + slot + ">", "Class<TK;>")),
                ("unresolved-context", short_signature.replace("(TK;", "(TMissing;")),
            ]
        for label, changed_signature in controls:
            changed_originals = work / (label + "-originals")
            shutil.copytree(originals, changed_originals)
            target = changed_originals / ("fixture/DerivedApi.class" if args.scoped_default else "fixture/Api.class")
            receipt = change_signature(target, short_signature.encode(), changed_signature.encode())
            receipt["target_identity"] = ("Lfixture/DerivedApi;->fetch(Ljava/lang/Object;Ljava/lang/Class;)Ljava/lang/Object;"
                                          if args.scoped_default else "Lfixture/Api;->fetch(Ljava/lang/Class;)Ljava/lang/Object;")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            counter_classpath = ["--classpath", sdk]
            if excluded:
                counter_provider = work / (label + "-provider.jar")
                write_provider(counter_provider, changed_originals)
                counter_classpath += ["--classpath", counter_provider]
            counter_dex = work / (label + "-dex")
            counter_dex.mkdir()
            counter_paths = [p for p in sorted(changed_originals.rglob("*.class")) if "L" + p.relative_to(changed_originals).as_posix()[:-6] + ";" in classes]
            verify.run([*verify.d8, "--release", "--min-api", "24", *counter_classpath, "--output", counter_dex, *counter_paths], label + "-d8")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(counter_dex / "classes.dex"), *map(str, counter_classpath), "--timeout", "600", "-o", str(work / (label + "-rejected"))], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-rejection.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or (work / (label + "-rejected")).exists():
                raise RuntimeError("Unproved interface token result throws Signature was accepted: " + label)

        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_counterexamples": [r[0] for r in controls], "dependency_provider": args.dependency_provider,
                   "scoped_default": args.scoped_default,
                   "metadata_roles_exceptions_and_runtime_definitions": "unchanged", "minimum_java_release": minimum_java_release}
        (work / "summary.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
