#!/usr/bin/env python3
"""Verify private generic members with independent input and result formals."""
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
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-api", action="store_true")
    parser.add_argument("--static-helper", action="store_true")
    parser.add_argument("--producer-only", action="store_true")
    parser.add_argument("--public-helper", action="store_true")
    parser.add_argument("--scoped-consumer", action="store_true")
    parser.add_argument("--reifiable-auxiliary", action="store_true")
    args = parser.parse_args()
    if args.public_helper and not args.producer_only:
        parser.error("--public-helper requires --producer-only")
    if args.scoped_consumer and not args.public_helper:
        parser.error("--scoped-consumer requires --public-helper")
    if args.reifiable_auxiliary and not args.public_helper:
        parser.error("--reifiable-auxiliary requires --public-helper")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-private-seeded-result-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 600)
        fixture = FIXTURES / "private_seeded_result/java"
        inputs = []
        for source in sorted(fixture.rglob("*.java")):
            text = source.read_text()
            if args.producer_only and source.name in {"Core.java", "Trace.java"}:
                if source.name == "Core.java":
                    before = "private final <R,S> R read(S seed,Callback<? super S,Token> callback,Factory<? extends R> producer)"
                    if text.count(before) != 1 or text.count("read(seed,callback,producer)") != 3:
                        raise RuntimeError("Original private producer source anchor changed")
                    text = text.replace(before, "private final <R> R read(Object seed,Callback<Object,Token> callback,Factory<? extends R> producer,Integer marker,java.util.List<? extends Callback<Token,? extends Object>> values)")
                    text = text.replace("read(seed,callback,producer)", "read(seed,callback,producer,Trace.marker,Trace.values)")
                    text = text.replace("R result=producer.get(); Trace.hit(3);", "R result=producer.get(); Trace.seenMarker=marker;Trace.seenValues=values; Trace.hit(3);")
                else:
                    text = text.replace("public final class Trace {", """public final class Trace {
    public static Integer marker;
    public static java.util.List<Callback<Token,Object>> values;
    public static Object seenMarker,seenValues;""")
                source = work / "producer-java/fixture" / source.name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.open("x").write(text)
            if args.static_helper and source.name == "Core.java":
                formals = "private final <R>" if args.producer_only else "private final <R,S>"
                if text.count(formals) != 1 or text.count("Trace.seenReceiver=this;") != 1:
                    raise RuntimeError("Original private member fixture changed")
                source = work / "static-java/fixture/Core.java"
                source.parent.mkdir(parents=True)
                text = text.replace(formals, formals.replace("private final", "private static final"), 1)
                text = text.replace("Trace.seenReceiver=this;", "Trace.seenReceiver=null;", 1)
                source.write_text(text)
            if args.public_helper and source.name == "Core.java":
                if text.count("private ") != 1:
                    raise RuntimeError("Original public producer source anchor changed")
                text = text.replace("private ", "public ", 1)
                source = work / "public-java/fixture/Core.java"
                source.parent.mkdir(parents=True)
                source.open("x").write(text)
            if args.scoped_consumer and source.name == "Core.java":
                if text.count("Callback<Object,Token> callback") != 1 or text.count("callback.apply(seed)") != 1:
                    raise RuntimeError("Original scoped producer source anchor changed")
                text = text.replace("Callback<Object,Token> callback", "Callback<? super R,Token> callback", 1)
                text = text.replace("callback.apply(seed)", "callback.apply((R)seed)", 1)
                source = work / "scoped-java/fixture/Core.java"
                source.parent.mkdir(parents=True)
                source.open("x").write(text)
            if args.reifiable_auxiliary and source.name == "Core.java":
                before = "java.util.List<? extends Callback<Token,? extends Object>> values"
                if text.count(before) != 1:
                    raise RuntimeError("Original reifiable auxiliary source anchor changed")
                text = text.replace(before, "java.util.List<?> values", 1)
                source = work / "reifiable-java/fixture/Core.java"
                source.parent.mkdir(parents=True)
                source.open("x").write(text)
            inputs.append(source)
        originals, harness = work / "originals", work / "harness"
        verify.compile_local(inputs, originals, release=8)
        observers = [FIXTURES / "harness" / ("PrivateSeededResult" + suffix + ".java")
                     for suffix in ("Caller", "Harness")]
        if args.producer_only:
            text = observers[1].read_text()
            begin = text.index('        Method method=Core.class.getDeclaredMethod("read",')
            end = text.index('        RuntimeException failure=', begin)
            text = text[:begin] + '''        Method method=Core.class.getDeclaredMethod("read",Object.class,Callback.class,Factory.class,Integer.class,java.util.List.class);
        require(Modifier.isPrivate(method.getModifiers())&&Modifier.isFinal(method.getModifiers())&&Modifier.isStatic(method.getModifiers())==isStatic&&!method.isBridge()&&!method.isSynthetic());
        TypeVariable<Method>[] vars=method.getTypeParameters();
        require(vars.length==1&&vars[0].getName().equals("R")&&vars[0].getBounds()[0]==Object.class&&method.getGenericReturnType().equals(vars[0]));
        Type[] params=method.getGenericParameterTypes();
        require(params[0]==Object.class&&params[3]==Integer.class);
        ParameterizedType cb=(ParameterizedType)params[1],factoryType=(ParameterizedType)params[2],list=(ParameterizedType)params[4];
        require(cb.getRawType()==Callback.class&&java.util.Arrays.equals(cb.getActualTypeArguments(),new Type[]{Object.class,Token.class}));
        require(factoryType.getRawType()==Factory.class&&((WildcardType)factoryType.getActualTypeArguments()[0]).getUpperBounds()[0].equals(vars[0]));
        require(list.getRawType()==java.util.List.class);
        ParameterizedType pair=(ParameterizedType)((WildcardType)list.getActualTypeArguments()[0]).getUpperBounds()[0];
        require(pair.getRawType()==Callback.class&&pair.getActualTypeArguments()[0]==Token.class&&((WildcardType)pair.getActualTypeArguments()[1]).getUpperBounds()[0]==Object.class);
''' + text[end:]
            before = "            Trace.value=produced;Trace.next=next;Trace.extra=extra;Trace.callbackValue=mode==15?new Object():token;"
            if text.count(before) != 1:
                raise RuntimeError("Original private producer observer setup changed")
            text = text.replace(before, before + '''
            Integer marker=mode==19?null:Integer.valueOf(101+attempt);
            java.util.List<Callback<Token,Object>> values=mode==19?null:new java.util.ArrayList<>();
            Trace.marker=marker;Trace.values=values;Trace.seenMarker=Trace.seenValues=sentinel;''')
            text = text.replace("Trace.observer=event->{if(event==3)", "Trace.observer=event->{if(event==1){Trace.marker=Integer.valueOf(-1);Trace.values=new java.util.ArrayList<>();}if(event==3)")
            text = text.replace("            require(Trace.token==(callbackDone?token:null));", "            require(Trace.token==(callbackDone?token:null));\n            require(Trace.seenMarker==(mutated?marker:sentinel)&&Trace.seenValues==(mutated?values:sentinel));")
            if args.scoped_consumer:
                before = "cb.getRawType()==Callback.class&&java.util.Arrays.equals(cb.getActualTypeArguments(),new Type[]{Object.class,Token.class})"
                if text.count(before) != 1:
                    raise RuntimeError("Original scoped producer observer anchor changed")
                text = text.replace(before, "cb.getRawType()==Callback.class&&cb.getActualTypeArguments()[1]==Token.class&&((WildcardType)cb.getActualTypeArguments()[0]).getLowerBounds()[0].equals(vars[0])", 1)
            if args.reifiable_auxiliary:
                before = '''        ParameterizedType pair=(ParameterizedType)((WildcardType)list.getActualTypeArguments()[0]).getUpperBounds()[0];
        require(pair.getRawType()==Callback.class&&pair.getActualTypeArguments()[0]==Token.class&&((WildcardType)pair.getActualTypeArguments()[1]).getUpperBounds()[0]==Object.class);'''
                if text.count(before) != 1:
                    raise RuntimeError("Original reifiable auxiliary reflection anchor changed")
                text = text.replace(before, '''        WildcardType wildcard=(WildcardType)list.getActualTypeArguments()[0];
        require(wildcard.getLowerBounds().length==0&&java.util.Arrays.equals(wildcard.getUpperBounds(),new Type[]{Object.class}));''', 1)
            observer = work / "PrivateSeededResultHarness.java"
            observer.open("x").write(text)
            observers[1] = observer
        if args.public_helper:
            text = observers[1].read_text()
            before = "Modifier.isPrivate(method.getModifiers())"
            if text.count(before) != 1:
                raise RuntimeError("Original public producer observer anchor changed")
            observer = work / "public-observer/PrivateSeededResultHarness.java"
            observer.parent.mkdir()
            observer.open("x").write(text.replace(before, "Modifier.isPublic(method.getModifiers())", 1))
            observers[1] = observer
        verify.compile_local(observers, harness, classpath=originals, release=8)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != 6 or len(all_methods) != 11:
            raise RuntimeError("Original private seeded result inventory changed")
        declarations = {"Lfixture/Callback;", "Lfixture/Factory;", "Lfixture/Token;"}
        classes = all_classes - (declarations if args.dependency_api else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        dependency = work / "owned-api.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            for descriptor in sorted(declarations):
                path = descriptor[1:-1] + ".class"
                jar.writestr(path, (originals / path).read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_api:
            classpath += ["--classpath", dependency]
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        (work / "frozen-input-hashes.json").write_text(json.dumps(
            {"originals": original_hashes, "harness": harness_hashes, "sdk": sdk_hash,
             "dependency": dependency_hash}, indent=2) + "\n")

        def execute(runtime: tuple, label: str) -> list[str]:
            outputs = []
            for route in range(4):
                for mode in range(20):
                    result = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                        "PrivateSeededResultHarness", str(route), str(mode), str(args.static_helper).lower()],
                        f"{label}-{route}-{mode}")
                    pattern = f"route={route},mode={mode},attempt=[01],trace=[0-9]+"
                    if len(result.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in result.splitlines()):
                        raise RuntimeError("Original private seeded result matrix changed: " + result)
                    outputs.append(result)
            return outputs

        baseline = execute((originals, harness), "baseline")
        mutations = (
            ("changed-result", "        return result;", "        return (R)seed;", 0),
            ("changed-snapshot", "        return result;", "        return (R)Trace.value;", 0),
            ("early-result-cast", "R result=producer.get();", "R result=(R)(Value)producer.get();", 13),
            ("skipped-callback", "Token token=callback.apply(seed);", "Token token=null;", 0),
            ("changed-seed", "Token token=callback.apply(seed);", "Token token=callback.apply(null);", 0),
            ("repeated-producer", "R result=producer.get();", "producer.get(); R result=producer.get();", 0),
        )
        if args.producer_only:
            mutations += (
                ("late-marker", "Trace.seenMarker=marker;", "Trace.seenMarker=Trace.marker;", 0),
                ("late-closed-values", "Trace.seenValues=values;", "Trace.seenValues=Trace.values;", 0),
            )
        core = next(p for p in inputs if p.name == "Core.java").read_text()
        for label, before, after, mode in mutations:
            if args.scoped_consumer:
                before = before.replace("callback.apply(seed)", "callback.apply((R)seed)")
            if core.count(before) != 1:
                raise RuntimeError("Original private seeded counterexample anchor changed: " + label)
            changed = work / label / "Core.java"
            changed.parent.mkdir()
            changed.write_text(core.replace(before, after, 1))
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                "PrivateSeededResultHarness", "1", str(mode), str(args.static_helper).lower()],
                text=True, capture_output=True, timeout=600, env=verify.environment)
            (work / (label + "-counterexample.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or "owned private seeded result" not in result.stderr:
                raise RuntimeError("Independent observer missed incorrect private seeded result: " + label)
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original private seeded result DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes
                or {r["identity"] for r in inventory["methods"]} != methods):
            raise RuntimeError("D8 changed an original private seeded result entry")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.producer_only:
            disassembly = verify.run([verify.dexdump, "-d", source_dex], "original-producer-disassembly")
            calls = re.findall(r"\|[0-9a-fA-F]+:\s+(invoke-\S+)[^\n]*Lfixture/Core;\.read:", disassembly)
            expected = ("invoke-static" if args.static_helper else
                        "invoke-virtual/range" if args.public_helper else "invoke-direct/range")
            if len(calls) != 3 or any(call.split("/")[0] != expected.split("/")[0] or (not args.static_helper and call != expected) for call in calls):
                raise RuntimeError("D8 changed the original private producer dispatch frame")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count", 0) or coverage["status"] != "recovered"
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Private seeded result recovery changed original entries or full generic contracts")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
            classpath=dependency if args.dependency_api else None, release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;"
                            for c in classes if c not in {"Lfixture/Callback;", "Lfixture/Factory;"}}
        if actual_classes != classes or set(actual_methods) != methods | expected_helpers or helpers != expected_helpers:
            raise RuntimeError("Private seeded result invented or omitted a runtime entry")
        after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
        for owner in classes:
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                        "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                if records[owner].get(key) != after[owner].get(key):
                    raise RuntimeError("Original private seeded class metadata changed: " + owner + ":" + key)
            for identity, metadata in records[owner]["methods"].items():
                if after[owner]["methods"].get(identity) != metadata or after_exceptions[identity] != exceptions[identity]:
                    raise RuntimeError("Original private seeded method metadata or Exceptions changed: " + identity)
        runtime = (rebuilt, harness, dependency) if args.dependency_api else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Private seeded result changed identities, callbacks, cast timing or exact failures")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original inputs, SDK or frozen observers changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
            "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "dependency_api": args.dependency_api,
            "static_helper": args.static_helper, "independent_input_result_formals_and_full_generic_reflection": "matched",
            "producer_only": args.producer_only,
            "public_helper": args.public_helper,
            "scoped_consumer": args.scoped_consumer,
            "reifiable_auxiliary": args.reifiable_auxiliary,
            "receiver_seed_result_identity_callback_order_and_snapshots": "matched",
            "checked_raw_discarded_results_nulls_and_exact_failures": "matched",
            "all_original_entries_metadata_and_exceptions": "unchanged", "independent_wrong_programs": len(mutations),
            "original_inputs_and_immutable_callers": "unchanged"}
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
