#!/usr/bin/env python3
"""Verify exact visibility bridges from closed recursive superclass contracts."""
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
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from scripts.test_mobile_android_visibility_generic_bridge import compiler_facts
    from scripts.test_mobile_android_empty_original_bridge_throws import hashes, exception_inventory
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk, choose_d8
    from test_mobile_android_visibility_generic_bridge import compiler_facts
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
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-closed-visibility-parameter-bridge-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 1200)
        compiler_work = work / "compiler"
        compiler_work.mkdir()
        compiler = Verify(compiler_work, choose_jdk(args.compiler_java_home), choose_d8(args.d8), 1200)
        fixture = FIXTURES / "closed_visibility_parameter_bridge/java"
        originals, harness = work / "originals", work / "harness"
        name = "fixture.ClosedVisibilityParameterBridgeHarness"
        sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
        sdk = args.android_jar.resolve()
        sdk_hash, cli_hash = sha(sdk), sha(args.neverd)
        compiler.compile_local(sorted(fixture.rglob("*.java")), originals, release=8)
        compiler.compile_local([FIXTURES / "harness" / ("ClosedVisibilityParameterBridge" + n + ".java") for n in ("Caller", "Future", "Harness")], harness, classpath=originals, release=11)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        (work / "frozen-input-hashes.json").open("x").write(json.dumps({"originals": original_hashes, "harness": harness_hashes}, indent=2) + "\n")
        all_classes, all_methods = compiler_inventory(verify, originals)
        if (len(all_classes), len(all_methods)) != (9, 9):
            raise RuntimeError("Owned closed visibility parameter bridge inventory changed")
        facts, exceptions = compiler_facts(originals), exception_inventory(originals)
        identity = "Lfixture/Base;->apply(Lfixture/First;)V"
        signature = "(TA;)V"
        if facts["Lfixture/Base;"]["methods"][identity]["signature"] != signature:
            raise RuntimeError("Original inherited method scope changed")
        providers = {"Lfixture/Base;", "Lfixture/Root;", "Lfixture/First;", "Lfixture/Second;"}
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

        def execute(runtime, label):
            outputs = []
            for route in range(6):
                for future in ("false", "true"):
                    for mode in range(12):
                        text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, runtime)), name, str(route), future, str(mode)], f"{label}-{route}-{future}-{mode}")
                        if text != "attempts=2,closed-visibility-parameter-bridge=matched\n":
                            raise RuntimeError("Frozen closed visibility parameter bridge observer changed")
                        outputs.append(text)
            return outputs

        baseline = execute((originals, harness), "baseline")
        (work / "baseline.json").open("x").write(json.dumps(baseline) + "\n")
        wrong = [
            ("wrong-receiver", "Base.java", "Trace.receiver = this;", "Trace.receiver = null;", 0, 0, False),
            ("wrong-argument", "Base.java", "Trace.argument = value;", "Trace.argument = null;", 0, 0, False),
            ("lost-action", "Base.java", "Trace.action.run();", ";", 0, 0, False),
            ("duplicate-action", "Base.java", "Trace.action.run();", "Trace.action.run(); Trace.action.run();", 0, 0, False),
            ("wrapped-checked", "Base.java", "Trace.action.run();", "try { Trace.action.run(); } catch (Throwable failure) { throw new IllegalStateException(failure); }", 0, 5, False),
            ("lost-completion", "Base.java", "Trace.hit(4);", ";", 0, 0, False),
            ("late-argument", "Base.java", "Trace.argument = value;\n        Trace.action.run();", "Trace.action.run(); Trace.argument = value;", 0, 0, False),
            ("invented-cast", "Base.java", "Trace.hit(3);", "Object checked = (Child.Left) (Object) value; Trace.hit(3);", 3, 2, False),
            ("lost-call", "Core.java", "child.apply(value);", ";", 0, 0, False),
            ("duplicate-call", "Core.java", "child.apply(value);", "child.apply(value); child.apply(value);", 0, 0, False),
            ("lost-tail", "Core.java", "Trace.hit(8);", ";", 0, 0, True),
            ("wrong-core-argument", "Core.java", "child.apply(value);", "child.apply(null);", 0, 0, False),
            ("fixed-receiver", "Core.java", "child.apply(value);", "new Child().apply(value);", 0, 0, True),
            ("explicit-specialization", "Child.java", "public interface Left", "@Override public void apply(Child.Left value) { super.apply(value); }\n    public interface Left", 3, 2, False),
            ("extra-runtime-entry", "Child.java", "public interface Left", "public void extra() { }\n    public interface Left", 0, 0, False),
            ("invalid-helper-contract", "Child.java", "public interface Left", "private static RuntimeException __neverdThrow(Throwable failure) { return new RuntimeException(failure); }\n    public interface Left", 0, 0, False),
        ]
        for label, filename, anchor, replacement, route, mode, future in wrong:
            text = (fixture / "fixture" / filename).read_text()
            if text.count(anchor) != 1:
                raise RuntimeError("Visibility bridge counterexample anchor changed: " + label)
            source = work / label / filename
            source.parent.mkdir()
            source.open("x").write(text.replace(anchor, replacement))
            compiled = work / (label + "-classes")
            compiler.compile_local([source], compiled, classpath=originals, release=8)
            result = subprocess.run([verify.java, "-cp", os.pathsep.join(map(str, (compiled, originals, harness))), name, str(route), str(future).lower(), str(mode)], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-counterexample.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or "owned closed visibility parameter bridge mismatch" not in result.stderr:
                raise RuntimeError("Observer missed independent wrong program: " + label)

        classpath = ["--classpath", sdk] + (["--classpath", dependency] if excluded else [])
        paths = [p for p in sorted(originals.rglob("*.class")) if "L" + p.relative_to(originals).as_posix()[:-6] + ";" in classes]
        dexdir = work / "dex"
        dexdir.mkdir()
        verify.run([*verify.d8, "--debug", "--min-api", "24", *classpath, "--output", dexdir, *paths], "d8")
        dex = dexdir / "classes.dex"
        dex_hash = sha(dex)
        if list(dexdir.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Owned method DEX inventory changed")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if {row["descriptor"] for row in inventory["classes"]} != classes or {row["identity"] for row in inventory["methods"]} != methods:
            raise RuntimeError("D8 changed an original method declaration")
        (work / "sdk-inventory.json").open("x").write(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, *classpath, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        bridges = {"Lfixture/Child;->apply(Lfixture/First;)V"}
        projected = {row["identity"] for row in coverage["methods"] if row["status"] == "source-projected"}
        if ({row["identity"] for row in coverage["methods"]} != methods or coverage["class_count"] != len(classes)
                or coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"]
                or coverage.get("projected_method_count") != 1 or projected != bridges
                or coverage["status"] != "partial" or coverage["recovered_method_count"] != len(methods) - 1
                or coverage.get("declaration_only_method_count", 0)
                or any(row["status"] != "recovered" for row in coverage["methods"] if row["identity"] not in bridges)
                or any(row.get("projection_kind") != "generated-public-superclass-access-bridge" for row in coverage["methods"] if row["identity"] in bridges)
                or coverage.get("class_source_bindings") or coverage.get("generic_signature_projections")):
            raise RuntimeError("Visibility bridge recovery changed original coverage or declaration contracts")
        rebuilt = work / "rebuilt"
        release = coverage.get("minimum_java_release", 8)
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt, classpath=dependency if excluded else None, release=release)
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {row["class"] + "->" + row["name"] + row["prototype"] for row in coverage.get("generated_source_helpers", [])}
        allowed_helpers = {c + "->__neverdThrow(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;" for c in classes}
        if actual_classes != classes or set(actual_methods) != methods | helpers or not helpers <= allowed_helpers:
            raise RuntimeError("Method recovery added or omitted an original runtime entry")
        after, after_exceptions = compiler_facts(rebuilt), exception_inventory(rebuilt)
        for helper in helpers:
            wanted = {"name": "__neverdThrow", "prototype": "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;",
                      "access": 0xA, "code": True,
                      "signature": "<E:Ljava/lang/Throwable;>(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;^TE;",
                      "deprecated_attribute": False, "runtime_visible_annotations": [],
                      "runtime_invisible_annotations": [], "runtime_invisible_parameter_annotations": [], "method_parameters": None}
            if after[helper.split("->", 1)[0]]["methods"][helper] != wanted or after_exceptions[helper] != ["Ljava/lang/Throwable;"]:
                raise RuntimeError("Generated throw helper changed its exact admitted contract")
        for c in classes:
            wanted, actual = copy.deepcopy(facts[c]), copy.deepcopy(after[c])
            actual["methods"] = {m: row for m, row in actual["methods"].items() if m not in helpers}
            if wanted.pop("major") != 52 or actual.pop("major") != 44 + release:
                raise RuntimeError("Method fixture compiler release changed")
            if wanted != actual or any(exceptions[m] != after_exceptions[m] for m in facts[c]["methods"]):
                raise RuntimeError("Original method Code, roles, Signature, Exceptions or parameter metadata changed: " + c)
        def bridge_code(root, label):
            javap = Path(verify.java).with_name("javap.exe" if os.name == "nt" else "javap")
            dump = verify.run([javap, "-classpath", root, "-c", "-s", "-p", "fixture.Child"], label)
            descriptor = "(Lfixture/First;)V"
            header = re.search(r"(?m)^  public void apply\(fixture.First\);\n    descriptor: " + re.escape(descriptor) + r"\n", dump)
            if not header:
                raise RuntimeError("Original visibility bridge header changed")
            instructions = []
            for line in dump[header.end():].splitlines():
                match = re.fullmatch(r"\s*([0-9]+):\s+(.*)", line)
                if match:
                    instructions.append(" ".join(re.sub(r"#\d+", "#", match.group(2)).split()))
                elif instructions:
                    break
            expected = ["aload_0", "aload_1", "invokespecial # // Method fixture/Base.apply:" + descriptor, "return"]
            if instructions != expected:
                raise RuntimeError("Original visibility bridge forwarding bytecodes changed: " + repr(instructions))
            return {"descriptor": descriptor, "instructions": instructions, "javap_sha256": hashlib.sha256(dump.encode()).hexdigest()}

        bridge_witness = {"status": "verified-original-and-rebuilt-bridge", "identities": sorted(bridges),
                          "reported_status": "source-projected", "original": bridge_code(originals, "original-bridge-code"),
                          "rebuilt": bridge_code(rebuilt, "rebuilt-bridge-code")}
        (work / "disclosed-metadata.json").open("x").write(json.dumps(bridge_witness, indent=2) + "\n")
        runtime = (rebuilt, harness, dependency) if excluded else (rebuilt, harness)
        if execute(runtime, "rebuilt") != baseline:
            raise RuntimeError("Method recovery changed initialization, checks, dispatch or exception identity")

        child_signature = facts["Lfixture/Child;"]["signature"]
        second_signature = facts["Lfixture/Second;"]["signature"]
        controls = [
            ("missing-method-slot", "Lfixture/Base;", identity, signature, "(TMissing;)V"),
            ("shadowed-method-slot", "Lfixture/Base;", identity, signature, "<A::Lfixture/First<TS;>;>(TA;)V"),
            ("changed-result-erasure", "Lfixture/Base;", identity, signature, "(TA;)I"),
            ("extra-method-formal", "Lfixture/Base;", identity, signature, "<U:Ljava/lang/Object;>(TA;)V"),
            ("raw-superclass", "Lfixture/Child;", None, child_signature, "Lfixture/Base;"),
            ("incompatible-input", "Lfixture/Child;", None, child_signature, child_signature.replace("Child$Left", "Child$Right")),
            ("variant-superclass", "Lfixture/Child;", None, child_signature, child_signature.replace("Lfixture/Child$Left;", "+Lfixture/Child$Left;")),
            ("unused-bound-arity", "Lfixture/Second;", None, second_signature, second_signature.replace(">", "U:Ljava/lang/Object;>", 1)),
        ]
        for label, owner, member, original_signature, changed_signature in controls:
            changed_originals = work / (label + "-originals")
            shutil.copytree(originals, changed_originals)
            target = changed_originals / (owner[1:-1] + ".class")
            code_before = method_code(target.read_bytes())
            if original_signature == changed_signature:
                raise RuntimeError("Signature control did not change its owned declaration")
            receipt = change_signature(target, original_signature.encode(), changed_signature.encode())
            changed_facts = compiler_facts(changed_originals)
            wanted = copy.deepcopy(facts)
            selected = wanted[owner]["methods"][member] if member else wanted[owner]
            selected["signature"] = changed_signature
            if changed_facts != wanted or method_code(target.read_bytes()) != code_before:
                raise RuntimeError("Method control changed unrelated metadata or Code")
            (work / (label + "-encoding.json")).open("x").write(json.dumps(receipt, indent=2) + "\n")
            counter_classpath = ["--classpath", sdk]
            if excluded:
                counter_provider = work / (label + "-provider.jar")
                write_provider(counter_provider, changed_originals)
                counter_classpath += ["--classpath", counter_provider]
            counter_dex = work / (label + "-dex")
            counter_dex.mkdir()
            encoding = verify.run([*verify.d8, "--debug", "--min-api", "24", *counter_classpath, "--output", counter_dex, *[changed_originals / p.relative_to(originals) for p in paths]], label + "-d8")
            if "Signature is ignored" in encoding:
                raise RuntimeError("D8 discarded a method Signature control: " + label)
            if owner in excluded:
                if sha(counter_dex / "classes.dex") != dex_hash:
                    raise RuntimeError("SDK method Signature control changed input DEX")
            else:
                annotations = verify.run([verify.dexdump, "-a", "-f", "-h", "-l", "plain", counter_dex / "classes.dex"], label + "-annotations")
                owners = re.findall(r"Class #(\d+)\s+-\n\s*Class descriptor\s*:\s*'" + re.escape(owner) + r"'", annotations)
                if len(owners) != 1:
                    raise RuntimeError("Missing control annotation owner")
                blocks = re.findall(r"Class #" + owners[0] + r" annotations:\n(.*?)\nClass #" + owners[0] + r"\s+-", annotations, re.S)
                if len(blocks) != 1:
                    raise RuntimeError("Missing control annotation block")
                if member:
                    members = re.findall(r"Annotations on method #[0-9]+ 'apply'\n((?:  [^\n]*\n)+)", blocks[0] + "\n")
                else:
                    if blocks[0].count("Annotations on class") != 1:
                        raise RuntimeError("Missing class Signature owner")
                    members = [blocks[0].split("Annotations on class", 1)[1].split("Annotations on ", 1)[0]]
                if len(members) != 1:
                    raise RuntimeError("Control changed its original Signature owner")
                encoded = []
                pattern = r'"(?:[^"\\]|\\.)*"'
                for member in members:
                    values = re.findall(r"VISIBILITY_SYSTEM Ldalvik/annotation/Signature; value=\{ (.*?) \}", member)
                    if len(values) != 1 or re.sub(pattern, "", values[0]).strip():
                        raise RuntimeError("Unexpected method Signature annotation encoding")
                    encoded.append("".join(json.loads(piece) for piece in re.findall(pattern, values[0])))
                if encoded != [changed_signature]:
                    raise RuntimeError("D8 normalized the visibility bridge Signature control: " + label)
                (work / (label + "-dex-signature.json")).open("x").write(json.dumps({"expected": changed_signature, "encoded": encoded, "status": "exact"}) + "\n")
            rejected = work / (label + "-rejected")
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(counter_dex / "classes.dex"), *map(str, counter_classpath), "-o", str(rejected)], text=True, capture_output=True, timeout=1200, env=verify.environment)
            (work / (label + "-rejection.log")).open("x").write(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists():
                raise RuntimeError("Unproved closed visibility parameter bridge was accepted: " + label)
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or sha(sdk) != sdk_hash
                or sha(dependency) != dependency_hash or sha(dex) != dex_hash or sha(args.neverd) != cli_hash):
            raise RuntimeError("Frozen original method program, observer, SDK or CLI changed")
        summary = {"status": "verified", "class_count": len(classes), "method_count": len(methods),
                   "fresh_jvm_cases": len(baseline), "attempts_per_case": 2, "wrong_programs": len(wrong),
                   "same_code_signature_controls": len(controls),
                   "metadata_roles_exceptions": "unchanged", "regenerated_bridges": len(bridges), "bridge_bytecodes": "unchanged", "dependency_provider": args.dependency_provider, "minimum_java_release": release}
        (work / "evidence.json").open("x").write(json.dumps(summary, indent=2) + "\n")
        print(json.dumps(summary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
