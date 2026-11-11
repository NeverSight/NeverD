#!/usr/bin/env python3
"""Verify a complete protected abstract naming family preserves virtual dispatch."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
from copy import deepcopy
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_protected_fixed_mangled import patch_name
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from test_mobile_android_protected_fixed_mangled import patch_name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "android-jar"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("java-home", "d8", "work-dir"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--dependency-base", action="store_true")
    parser.add_argument("--baseline-only", action="store_true")
    parser.add_argument("--public-family", action="store_true")
    parser.add_argument("--closed-contracts", action="store_true")
    args = parser.parse_args()
    if args.closed_contracts and not args.public_family:
        parser.error("--closed-contracts requires --public-family")
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-protected-abstract-names-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        fixture = FIXTURES / "protected_abstract_names/java"
        harness_source = FIXTURES / "harness/ProtectedAbstractNamesHarness.java"
        if args.public_family:
            copied = work / "fixture"
            shutil.copytree(fixture, copied)
            fixture = copied
            for name in ("api/Root", "impl/Left", "impl/Right"):
                path = fixture / (name + ".java")
                text = path.read_text()
                if text.count("protected ") != 1:
                    raise RuntimeError("Original abstract family visibility anchor changed")
                path.write_text(text.replace("protected ", "public "))
            (fixture / "api/Other.java").open("x").write(
                "package api;\npublic interface Other { boolean chooseValue(Press scope,long bits); }\n")
            (fixture / "impl/Edge.java").open("x").write(
                "package impl;\nimport api.*;\npublic class Edge implements Other { public boolean chooseValue(Press scope,long bits) { return bits!=0; } }\n")
            text = harness_source.read_text().replace("Modifier.isProtected(", "Modifier.isPublic(")
            old = "for(int attempt=0;attempt<2;attempt++) {"
            new = 'Class<?> contract=Class.forName("api.Other");Object edge=Class.forName("impl.Edge").getConstructor().newInstance();\n        Method shared=contract.getDeclaredMethod(name,Press.class,long.class);\n        require(shared.invoke(edge,null,0L)==Boolean.FALSE&&shared.invoke(edge,null,Long.MIN_VALUE)==Boolean.TRUE);\n        ' + old
            if text.count(old) != 1:
                raise RuntimeError("Independent interface observer anchor changed")
            harness_source = work / "ProtectedAbstractNamesHarness.java"
            harness_source.open("x").write(text.replace(old, new))
        if args.closed_contracts:
            for path in fixture.rglob("*.java"):
                text = path.read_text().replace("Press scope", "Press<? extends CharSequence> scope")
                if path.name == "Press.java":
                    if text.count("interface Press {") != 1:
                        raise RuntimeError("Closed parameter contract anchor changed")
                    text = text.replace("interface Press {", "interface Press<T> {")
                elif path.name == "Base.java":
                    text = text.replace("class Base {", "class Base<T> {")
                elif path.name == "Root.java":
                    text = text.replace("extends Base {", "extends Base<String> {")
                path.write_text(text)
            text = harness_source.read_text()
            marker = "    public static void main(String[] args) throws Throwable {"
            check = """    static void signature(Method method) {
        require(method.getTypeParameters().length==0);
        ParameterizedType parameter=(ParameterizedType)method.getGenericParameterTypes()[0];
        require(parameter.getRawType()==Press.class);
        WildcardType argument=(WildcardType)parameter.getActualTypeArguments()[0];
        require(argument.getLowerBounds().length==0 && argument.getUpperBounds().length==1 && argument.getUpperBounds()[0]==CharSequence.class);
        require(method.getGenericParameterTypes()[1]==long.class && method.getGenericReturnType()==boolean.class);
    }
"""
            if text.count(marker)!=1:
                raise RuntimeError("Closed abstract reflection anchor changed")
            text=text.replace(marker,check+marker)
            marker='        Method root=Root.class.getDeclaredMethod(name,Press.class,long.class);'
            extra="""
        ParameterizedType parent=(ParameterizedType)Root.class.getGenericSuperclass();
        require(parent.getRawType()==Base.class && parent.getActualTypeArguments()[0]==String.class);
        require(Base.class.getTypeParameters().length==1 && Root.class.getTypeParameters().length==0);
        signature(root);"""
            text=text.replace(marker,marker+extra)
            marker='            Method implementation=created.getClass().getDeclaredMethod(name,Press.class,long.class);'
            text=text.replace(marker,marker+'\n            signature(implementation);')
            destination=work/"closed-observer/ProtectedAbstractNamesHarness.java"
            destination.parent.mkdir();destination.open("x").write(text);harness_source=destination
        authored, originals, harness = work / "authored", work / "originals", work / "harness"
        verify.compile_local(sorted(fixture.rglob("*.java")), authored, release=8)
        verify.compile_local([FIXTURES / "harness/ProtectedAbstractNamesCaller.java", harness_source],
                             harness, classpath=authored, release=8)
        authored_hashes = hashes(authored)
        before = class_identity.compiler_classes(authored)
        shutil.copytree(authored, originals)
        witness = {}
        for path in sorted(originals.rglob("*.class")):
            count = patch_name(path)
            if count:
                witness[path.relative_to(originals).as_posix()] = count
        expected_witness = {"api/Root.class": 1, "api/Peer.class": 1,
                            "impl/Left.class": 1, "impl/Right.class": 1, "impl/Middle.class": 1}
        if args.public_family:
            expected_witness.update({"api/Other.class": 1, "impl/Edge.class": 1})
        if witness != expected_witness:
            raise RuntimeError("Original complete protected family encoding sites changed")
        (work / "name-encoding-witness.json").open("x").write(json.dumps(witness, indent=2) + "\n")
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        all_classes, all_methods = compiler_inventory(verify, originals)
        if len(all_classes) != (11 if args.public_family else 9) or len(all_methods) != (21 if args.public_family else 18):
            raise RuntimeError("Original complete protected family inventory changed")
        classes = all_classes - ({"Lapi/Base;"} if args.dependency_base else set())
        methods = {m for m in all_methods if m.split("->", 1)[0] in classes}
        records, exceptions = class_identity.compiler_classes(originals), exception_inventory(originals)
        prototype = "(Lapi/Press;J)Z"
        family = {c + "->choose-Name" + prototype for c in ("Lapi/Root;", "Limpl/Left;", "Limpl/Right;")}
        root = "Lapi/Root;->choose-Name" + prototype
        other_root = "Lapi/Other;->choose-Name" + prototype
        if args.public_family:
            family.update({other_root, "Limpl/Edge;->choose-Name" + prototype})
        wanted = deepcopy(before)
        for identity in family:
            owner = identity.split("->", 1)[0]
            method = wanted[owner]["methods"].pop(identity.replace("->choose-Name", "->chooseValue"))
            method["name"] = "choose-Name"
            wanted[owner]["methods"][identity] = method
        for owner in records:
            for key in wanted[owner]:
                if key != "sha256" and wanted[owner][key] != records[owner].get(key):
                    raise RuntimeError("Protected name encoding changed other metadata: " + owner + ":" + key)
        for identity, access in ((root, 0x404), ("Limpl/Left;->choose-Name" + prototype, 0x14),
                                 ("Limpl/Right;->choose-Name" + prototype, 0x4)):
            if args.public_family:
                access = (access & ~4) | 1
            if records[identity.split("->", 1)[0]]["methods"][identity]["access"] != access:
                raise RuntimeError("Original protected family visibility or finality changed")
        dependency = work / "owned-base.jar"
        with zipfile.ZipFile(dependency, "x") as jar:
            jar.writestr("api/Base.class", (originals / "api/Base.class").read_bytes())
        dependency_hash = hashlib.sha256(dependency.read_bytes()).hexdigest()
        classpath = ["--classpath", args.android_jar.resolve()]
        if args.dependency_base:
            classpath += ["--classpath", dependency]

        def execute(runtime, label, spelling):
            outputs = []
            for route in range(6):
                for kind in range(2):
                    for mode in range(10):
                        output = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)),
                                             "ProtectedAbstractNamesHarness", str(route), str(kind), str(mode), spelling],
                                            f"{label}-{route}-{kind}-{mode}")
                        pattern = f"route={route},kind={kind},mode={mode},attempt=[01],events=[0-9]+"
                        if len(output.splitlines()) != 2 or not all(re.fullmatch(pattern, line) for line in output.splitlines()):
                            raise RuntimeError("Original protected virtual family matrix changed")
                        outputs.append(output)
            return outputs

        baseline = execute((authored, harness), "authored", "chooseValue")
        if execute((originals, harness), "encoded", "choose-Name") != baseline:
            raise RuntimeError("Method name encoding changed original virtual dispatch")
        dex = work / "dex"
        dex.mkdir()
        paths = [p for p in sorted(originals.rglob("*.class"))
                 if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dex, *paths], "d8")
        source_dex = dex / "classes.dex"
        if list(dex.glob("classes*.dex")) != [source_dex]:
            raise RuntimeError("Original protected family changed DEX inventory")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", source_dex], "sdk-inventory")
        inventory = parse_dexdump(dump, source_dex.name, dex_header(source_dex.read_bytes()))
        if {r["descriptor"] for r in inventory["classes"]} != classes or {r["identity"] for r in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original protected family entry")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        mutations = (("wrong-implementation", "impl/Right", "boolean value=!Trace.output;", "boolean value=Trace.output;", 1, 0),
                     ("lost-wide", "api/Root", "chooseValue(scope,bits)", "chooseValue(scope,(int)bits)", 0, 0),
                     ("skipped-touch", "impl/Left", "scope.touch();Trace.hit(3);", "Trace.hit(3);", 0, 0),
                     ("delayed-publication", "api/Root", "Trace.hit(4);return value;",
                      "Trace.published=false;Trace.hit(4);Trace.published=true;return value;", 0, 8))
        for label, name, old, new, kind, mode in mutations:
            text = (fixture / (name + ".java")).read_text()
            if text.count(old) != 1:
                raise RuntimeError("Original protected family counterexample anchor changed")
            changed = work / label / (name + ".java")
            changed.parent.mkdir(parents=True)
            changed.open("x").write(text.replace(old, new))
            wrong = work / (label + "-classes")
            verify.compile_local([changed], wrong, classpath=authored, release=8)
            if patch_name(wrong / (name + ".class")) != 1:
                raise RuntimeError("Counterexample method name encoding changed")
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (wrong, originals, harness))),
                                     "ProtectedAbstractNamesHarness", "0", str(kind), str(mode), "choose-Name"],
                                    text=True, capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned protected abstract names" not in result.stderr:
                raise RuntimeError("Frozen observer missed incorrect protected dispatch: " + label)
        if args.closed_contracts:
            try:
                from scripts.test_mobile_android_static_defaults import method_code
            except ModuleNotFoundError:
                from test_mobile_android_static_defaults import method_code
            data=(originals/"api/Root.class").read_bytes()
            signature=b"(Lapi/Press<+Ljava/lang/CharSequence;>;J)Z"
            changed_signature=signature.replace(b"CharSequence",b"Object")
            old=b"\x01"+len(signature).to_bytes(2,"big")+signature
            new=b"\x01"+len(changed_signature).to_bytes(2,"big")+changed_signature
            if data.count(old)!=1:raise RuntimeError("Closed abstract Signature anchor changed")
            changed=data.replace(old,new)
            if method_code(data)!=method_code(changed):raise RuntimeError("Signature drift changed original Code")
            wrong=work/"signature-drift/api/Root.class";wrong.parent.mkdir(parents=True);wrong.open("xb").write(changed)
            result=subprocess.run([verify.java,"-cp",os.pathsep.join(map(str,(wrong.parent.parent,originals,harness))),
                                   "ProtectedAbstractNamesHarness","0","0","0","choose-Name"],text=True,capture_output=True,timeout=300,env=verify.environment)
            (work/"signature-drift-counterexample.log").open("x").write(result.stdout+result.stderr)
            if result.returncode==0 or "owned protected abstract names" not in result.stderr:raise RuntimeError("Frozen observer missed abstract Signature drift")
        evidence = {"class_count": len(classes), "method_count": len(methods), "fresh_jvm_cases": len(baseline),
                    "attempts_per_case": 2, "dependency_base": args.dependency_base, "public_family": args.public_family,
                    "independent_wrong_programs": len(mutations)+int(args.closed_contracts), "closed_contracts": args.closed_contracts}
        if not args.baseline_only:
            output = work / "recovered"
            verify.run([args.neverd.resolve(), "mobile", source_dex, *classpath, "-o", output], "recover")
            coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
            rows = {r["identity"]: r for r in coverage["methods"]}
            if (set(rows) != methods or coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods)
                    or coverage["unrecovered_method_count"] or coverage.get("projected_method_count") != len(family)
                    or coverage["status"] != "partial" or coverage.get("class_source_bindings")
                    or coverage.get("generic_signature_projections")):
                raise RuntimeError("Protected family lost an entry or changed its full source contracts")
            spelling = rows[root].get("source_name")
            for identity in family:
                row = rows[identity]
                if (row.get("source_name") != spelling or spelling != "__neverdVirtualMethod0"
                        or row.get("source_name_declaration") != (other_root if identity.startswith(("Lapi/Other;", "Limpl/Edge;")) else root)
                        or set(row.get("source_name_family", [])) != family
                        or row.get("projection_kind") != "virtual-method-identifier"
                        or len(row.get("metadata_differences", [])) != 3):
                    raise RuntimeError("Protected family did not retain one complete abstract root receipt")
            rebuilt = work / "rebuilt"
            verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                                 classpath=dependency if args.dependency_base else None,
                                 release=coverage.get("minimum_java_release", 8))
            actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
            renamed = {m: m.replace("->choose-Name", "->" + spelling) for m in family}
            helpers = {r["class"] + "->" + r["name"] + r["prototype"] for r in coverage.get("generated_source_helpers", [])}
            expected_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes - {"Lapi/Press;", "Lapi/Other;"}}
            if actual_classes != classes or set(actual_methods) != {renamed.get(m, m) for m in methods} | expected_helpers or helpers != expected_helpers:
                raise RuntimeError("Protected family invented or omitted a recompiled entry")
            after, after_exceptions = class_identity.compiler_classes(rebuilt), exception_inventory(rebuilt)
            for owner in classes:
                for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method",
                            "deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations"):
                    if records[owner].get(key) != after[owner].get(key):
                        raise RuntimeError("Original protected family class metadata changed: " + owner + ":" + key)
                for identity, metadata in records[owner]["methods"].items():
                    wanted = deepcopy(metadata)
                    if identity in family:
                        wanted["name"] = spelling
                    selected = renamed.get(identity, identity)
                    if after[owner]["methods"].get(selected) != wanted or after_exceptions[selected] != exceptions[identity]:
                        raise RuntimeError("Original protected family method metadata or Exceptions changed: " + identity)
            runtime = (rebuilt, harness, dependency) if args.dependency_base else (rebuilt, harness)
            if execute(runtime, "rebuilt", spelling) != baseline:
                raise RuntimeError("Protected family changed virtual dispatch, receiver, wide values, callbacks or failures")
        if (hashes(authored) != authored_hashes or hashes(originals) != original_hashes or hashes(harness) != harness_hashes
                or hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash
                or hashlib.sha256(dependency.read_bytes()).hexdigest() != dependency_hash):
            raise RuntimeError("Original programs or immutable observers changed")
        evidence.update(status="baseline-only" if args.baseline_only else "success",
                        original_Code_and_metadata="unchanged except disclosed method names",
                        full_protected_virtual_dispatch_and_access="matched")
        (work / "evidence.json").open("x").write(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
