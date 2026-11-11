#!/usr/bin/env python3
"""Verify unary covariant void calls retain generic interface and override dispatch."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--dependency-provider", action="store_true")
    parser.add_argument("--contextual-static", action="store_true")
    parser.add_argument("--reifiable-setter", action="store_true")
    args = parser.parse_args()
    if args.reifiable_setter and not args.contextual_static:
        parser.error("--reifiable-setter requires --contextual-static")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-unary-void-carrier-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "unary_void_carrier/java"
        providers = {"Lfixture/Provider;", "Lfixture/Factory;"}
        inputs = sorted(fixture.rglob("*.java"))
        observers = [FIXTURES / "harness" / (name + ".java") for name in
                     ("UnaryVoidCarrierHarness", "UnaryVoidCarrierCaller")]
        if args.contextual_static:
            authored = work / "contextual-java/fixture"; authored.mkdir(parents=True)
            core = fixture / "fixture/Core.java"; text = core.read_text()
            if text.count("provider.send(factory);") != 1: raise RuntimeError("Original contextual void caller anchor changed")
            call = "ObserverEntry.visit(provider,(Factory)factory,Trace.property,Trace.seed);" if args.reifiable_setter else "ObserverEntry.visit(provider,factory);"
            caller = authored / "Core.java"; caller.open("x").write(text.replace("provider.send(factory);", call, 1))
            entry = authored / "ObserverEntry.java"
            entry_text = '''package fixture;
public final class ObserverEntry {
    public static <T> void visit(Provider provider, Factory<? extends T> factory) {
        provider.send(factory);
    }
}
'''
            if args.reifiable_setter:
                entry_text = entry_text.replace("Factory<? extends T> factory)", "Factory<T> factory, java.util.List<?> property, T seed)")
                entry_text = entry_text.replace("        provider.send(factory);", "        Trace.seenSeed=seed; Trace.seenProperty=property;\n        provider.send(factory);")
                trace = fixture / "fixture/Trace.java"; trace_text = trace.read_text()
                for before, after in (
                    ("    public static RuntimeException failure;", "    public static RuntimeException failure;\n    public static Object seed,seenSeed,seenProperty;\n    public static java.util.List<?> property;"),
                    ("        events = events * 10 + event;", "        events = events * 10 + event;\n        if(event==1||event==4){seed=new Object();property=new java.util.ArrayList<Object>();}")):
                    if trace_text.count(before) != 1: raise RuntimeError("Original reifiable setter state anchor changed")
                    trace_text = trace_text.replace(before, after, 1)
                changed_trace = authored / "Trace.java"; changed_trace.open("x").write(trace_text)
                inputs = [changed_trace if p == trace else p for p in inputs]
            entry.open("x").write(entry_text)
            inputs = [caller if p == core else p for p in inputs] + [entry]
            providers.add("Lfixture/ObserverEntry;")
            text = observers[0].read_text(); before = '        Trace.failure = new IllegalArgumentException("owned factory failure");'
            if text.count(before) != 1: raise RuntimeError("Original contextual void observer anchor changed")
            metadata = '''        java.lang.reflect.Method method = ObserverEntry.class.getMethod("visit",Provider.class,Factory.class);
        java.lang.reflect.TypeVariable<?>[] vars = method.getTypeParameters();
        require(vars.length==1&&vars[0].getName().equals("T")&&java.util.Arrays.equals(vars[0].getBounds(),new java.lang.reflect.Type[]{Object.class}));
        require(java.lang.reflect.Modifier.isPublic(method.getModifiers())&&java.lang.reflect.Modifier.isStatic(method.getModifiers())&&!method.isBridge()&&!method.isSynthetic());
        require(method.getGenericReturnType()==void.class&&method.getGenericParameterTypes()[0]==Provider.class);
        java.lang.reflect.ParameterizedType input = (java.lang.reflect.ParameterizedType)method.getGenericParameterTypes()[1];
        java.lang.reflect.WildcardType slot = (java.lang.reflect.WildcardType)input.getActualTypeArguments()[0];
        require(input.getRawType()==Factory.class&&slot.getLowerBounds().length==0&&java.util.Arrays.equals(slot.getUpperBounds(),new java.lang.reflect.Type[]{vars[0]}));
'''
            if args.reifiable_setter:
                metadata = metadata.replace('"visit",Provider.class,Factory.class)', '"visit",Provider.class,Factory.class,java.util.List.class,Object.class)')
                begin = metadata.index("        java.lang.reflect.WildcardType slot =")
                metadata = metadata[:begin] + '''        require(input.getRawType()==Factory.class&&input.getActualTypeArguments()[0].equals(vars[0]));
        java.lang.reflect.ParameterizedType propertyType=(java.lang.reflect.ParameterizedType)method.getGenericParameterTypes()[2];
        java.lang.reflect.WildcardType star=(java.lang.reflect.WildcardType)propertyType.getActualTypeArguments()[0];
        require(propertyType.getRawType()==java.util.List.class&&star.getLowerBounds().length==0&&java.util.Arrays.equals(star.getUpperBounds(),new java.lang.reflect.Type[]{Object.class}));
        require(method.getGenericParameterTypes()[3].equals(vars[0]));
'''
                before_runtime = "            Object oldLast = worker.last;"
                after_runtime = before_runtime + '''
            Object seed=mode==8?null:mode==9?new Object():Integer.valueOf(97+attempt);
            java.util.List<?> property=mode==7?null:new java.util.ArrayList<String>();
            Trace.seed=seed;Trace.property=property;Trace.seenSeed=Trace.seenProperty=null;'''
                if text.count(before_runtime) != 1: raise RuntimeError("Original reifiable setter observer setup changed")
                text = text.replace(before_runtime, after_runtime, 1)
                before_result = '            System.out.println("variant="'
                if text.count(before_result) != 1: raise RuntimeError("Original reifiable setter observer result changed")
                text = text.replace(before_result, "            require(Trace.seenSeed==seed&&Trace.seenProperty==property);\n" + before_result, 1)
            observer = work / "contextual-observer/UnaryVoidCarrierHarness.java"; observer.parent.mkdir()
            observer.open("x").write(text.replace(before, metadata + before, 1)); observers[0] = observer
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (7 if args.contextual_static else 6) or len(all_methods) != (12 if args.contextual_static else 10):
            raise RuntimeError("Owned unary void carrier inventory changed")
        classes = all_classes - (providers if args.dependency_provider else set())
        methods = {identity for identity in all_methods if identity.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        dependency = work / "owned-provider.jar"
        with zipfile.ZipFile(dependency, "x") as archive:
            for owner in sorted(providers):
                name = owner[1:-1] + ".class"
                archive.writestr(name, (originals / name).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_provider:
            classpath += ["--classpath", dependency]

        def execute(runtime, label):
            outputs = []
            for variant in range(2):
                for route in range(4):
                    for mode in range(10):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                            "UnaryVoidCarrierHarness", str(variant), str(route), str(mode)], f"{label}-{variant}-{route}-{mode}")
                        expected = f"variant={variant},route={route},mode={mode},attempt=[01],trace=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(expected, line) for line in output.splitlines()):
                            raise RuntimeError("Original unary void matrix changed: " + output)
                        outputs.append(output)
            return outputs

        baseline = execute((originals, harness), "baseline")
        dex_dir = work / "dex"
        dex_dir.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex_dir, *paths], "d8-original")
        source_dex = dex_dir / "classes.dex"
        if list(dex_dir.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Owned unary void carrier fixture changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original unary void carrier runtime entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        signature = "<T:Ljava/lang/Object;>(Lfixture/Factory<+TT;>;)V"
        for owner in ("Provider", "Worker", "Child"):
            identity = "Lfixture/" + owner + ";"
            if records[identity]["methods"][identity + "->send(Lfixture/Factory;)V"]["signature"] != signature:
                raise RuntimeError("Compiler changed the complete unary callback contract")
        if args.contextual_static:
            identity = "Lfixture/ObserverEntry;->visit(Lfixture/Provider;Lfixture/Factory;" + ("Ljava/util/List;Ljava/lang/Object;" if args.reifiable_setter else "") + ")V"
            signature = "<T:Ljava/lang/Object;>(Lfixture/Provider;Lfixture/Factory<" + ("TT;>;Ljava/util/List<*>;TT;" if args.reifiable_setter else "+TT;>;") + ")V"
            if records["Lfixture/ObserverEntry;"]["methods"][identity]["signature"] != signature:
                raise RuntimeError("Compiler changed the complete observer/producer void contract")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != methods
                or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                or coverage["unrecovered_method_count"] or coverage.get("projected_method_count", 0)
                or coverage["status"] != "recovered" or coverage.get("class_source_bindings")
                or coverage.get("generic_signature_projections") or coverage.get("minimum_java_release", 8) != 8):
            raise RuntimeError("Unary void carrier recovery changed original inventory or contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=8, classpath=dependency if args.dependency_provider else None)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {owner + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for owner in classes - {"Lfixture/Provider;", "Lfixture/Factory;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Unary void carrier recovery invented or omitted a runtime entry")
        actual_records, actual_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original payload class metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                if after["methods"].get(identity) != metadata or actual_exceptions.get(identity) != exceptions[identity]:
                    raise RuntimeError("Original payload method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_provider else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Unary void call changed effects, aliases or failure identity")
        mutations = (
            ("repeated-factory", "Worker", "calls++;", "factory.make(); calls++;"),
            ("lost-store", "Worker", "last = value;", "last = null;"),
            ("lost-override-effect", "Child", "Trace.hit(4);", "Trace.hit(5);"),
        )
        if args.contextual_static:
            mutations += (
                ("lost-observer", "ObserverEntry", "provider.send(factory);", "((Provider)null).send(factory);"),
                ("lost-producer", "ObserverEntry", "provider.send(factory);", "provider.send(null);"),
                ("early-producer", "ObserverEntry", "provider.send(factory);", "factory.make(); provider.send(factory);"),
            )
        if args.reifiable_setter:
            mutations += (
                ("lost-seed", "ObserverEntry", "Trace.seenSeed=seed;", "Trace.seenSeed=null;"),
                ("lost-property", "ObserverEntry", "Trace.seenProperty=property;", "Trace.seenProperty=null;"),
            )
        for label, owner, old, new in mutations:
            text = next(p for p in inputs if p.stem == owner).read_text()
            if old not in text:
                raise RuntimeError("Independent scalar carrier counterexample anchor changed")
            wrong = work / label / ("fixture/" + owner + ".java")
            wrong.parent.mkdir(parents=True)
            wrong.write_text(text.replace(old, new))
            compiled = work / (label + "-classes")
            verify.compile_local([wrong], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))),
                                    "UnaryVoidCarrierHarness", "1" if owner == "Child" else "0", "1", "0"], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned unary void contract" not in result.stderr:
                raise RuntimeError("Independent oracle did not detect " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, immutable callers or SDK bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "dependency_provider": args.dependency_provider,
                    "contextual_static": args.contextual_static,
                    "reifiable_setter": args.reifiable_setter,
                    "fresh_jvm_cases": len(baseline), "attempts_per_case": 2,
                    "original_inventory_metadata_and_exceptions": "unchanged",
                    "original_interface_and_override_dispatch": "matched",
                    "mutable_callback_aliases_and_failure_identity": "matched",
                    "independent_compiler_counterexamples": len(mutations), "input_and_oracle_bytes": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
