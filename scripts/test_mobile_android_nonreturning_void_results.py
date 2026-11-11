#!/usr/bin/env python3
"""Verify terminal Void aliases and original void dispatch in a captured scope."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from scripts.test_mobile_android_nested_return import change_signature
    from scripts.test_mobile_android_plain_integer_bridge import bridge_flag
    from scripts.test_mobile_android_private_producer_return_slots import rename_owned_entry
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_plain_integer_bridge import bridge_flag
    from test_mobile_android_private_producer_return_slots import rename_owned_entry
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(
        prefix="neverd-nonreturning-void-results-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "nonreturning_void_results/java"
        verify.compile_local(sorted(fixture.rglob("*.java")), originals)
        verify.compile_local([FIXTURES / "harness/NonReturningVoidResultsHarness.java"], harness,
                             classpath=originals)
        owner = "Lfixture/Host$1;"

        def encode(root: Path) -> list:
            target = root / "fixture/Host$1.class"
            edits = [change_signature(target, b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/Object;>;",
                                      b"Ljava/lang/Object;Lfixture/Api<TT;>;")]
            before = target.read_bytes()
            offset = class_records(before)["pool_end"]
            if int.from_bytes(before[offset:offset + 2], "big") != 0x20:
                raise RuntimeError("Owned anonymous class access flags changed")
            after = before[:offset] + (0x30).to_bytes(2, "big") + before[offset + 2:]
            if method_code(before) != method_code(after) or before[offset + 2:] != after[offset + 2:]:
                raise RuntimeError("Final class flag edit changed original Code or declarations")
            target.write_bytes(after)
            edits.append({"original_class_flags": 0x20, "input_class_flags": 0x30,
                          "code_and_declarations": "unchanged"})
            for old, new, prototype in ((b"typedDrop", b"add", "(Ljava/lang/Object;)V"),
                                        (b"typedReplace", b"set", "(Ljava/lang/Object;)V"),
                                        (b"typedErase", b"remove", "()V")):
                edits += [rename_owned_entry(target, old, new),
                          bridge_flag(target, new.decode(), prototype, 0x1, 0x1041)]
            return edits

        (work / "owned-input-edits.json").write_text(json.dumps(encode(originals), indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = class_identity.compiler_classes(originals)

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "NonReturningVoidResultsHarness"], "baseline")
        if baseline.splitlines() != ["values=38,identities=64,failures=32",
                                     "nonreturning-void-prefix-dispatch-and-exceptions=matched,source-names-and-parameters=reported"]:
            raise RuntimeError("Owned void baseline missed ordered effects or identity cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned void fixture changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original captured void declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        named = {r["identity"]: r for r in coverage["methods"]
                 if r.get("projection_kind") == "nonreturning-void-target-alias"}
        forward = {r["identity"]: r for r in coverage["methods"]
                   if r.get("projection_kind") == "nonreturning-void-forwarding-bridge"}
        parameters = {"add": "(Ljava/lang/Object;)", "set": "(Ljava/lang/Object;)", "remove": "()"}
        typed_ids = {owner + "->" + name + p + "Ljava/lang/Void;" for name, p in parameters.items()}
        bridge_ids = {owner + "->" + name + p + "V" for name, p in parameters.items()}
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or set(named) != typed_ids or
                set(forward) != bridge_ids):
            raise RuntimeError("Void recovery lost an original entry or full lexical contract")
        for name, p in parameters.items():
            typed, bridge = owner + "->" + name + p + "Ljava/lang/Void;", owner + "->" + name + p + "V"
            signature = "(TT;)V" if name != "remove" else None
            if (named[typed].get("original_bridge") != bridge or not named[typed].get("source_name") or
                    not named[typed].get("metadata_differences") or
                    forward[bridge].get("bridge_target") != typed or
                    forward[bridge].get("source_generic_signature") != signature or
                    not forward[bridge].get("metadata_differences")):
                raise RuntimeError("Void aliases or source parameter differences lack an original witness")
        bindings = coverage.get("class_source_bindings", [])
        if (len(bindings) != 1 or bindings[0].get("class") != owner or
                bindings[0].get("binding_kind") != "anonymous-to-named-local" or
                not bindings[0].get("metadata_differences")):
            raise RuntimeError("Captured class rename has no exact lexical source binding")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage["minimum_java_release"])
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        candidates = [c for c in actual_classes if c.endswith(bindings[0]["source_name"] + ";")]
        if len(candidates) != 1:
            raise RuntimeError("Captured source class binding is not unique")
        renamed_owner = candidates[0]

        def rename(identity: str) -> str:
            return identity.replace(owner, renamed_owner)

        source_ids = {r["identity"]: rename(r.get("source_identity", r["identity"]))
                      for r in coverage["methods"]}
        helpers = {rename(r["class"] + "->" + r["name"] + r["prototype"])
                   for r in coverage.get("generated_source_helpers", [])}
        hidden_outer = []
        for identity, source_id in list(source_ids.items()):
            if source_id in actual_methods or "-><init>(" not in source_id:
                continue
            with_outer = source_id.replace("-><init>(", "-><init>(Lfixture/Host;", 1)
            if identity.startswith(owner + "->") and with_outer in actual_methods:
                source_ids[identity] = with_outer
                hidden_outer.append(identity)
        if actual_classes != {rename(c) for c in classes} or set(actual_methods) != set(source_ids.values()) | helpers:
            raise RuntimeError("javac changed original inventory beyond the reported binding and helpers")
        actual_records = class_identity.compiler_classes(rebuilt)
        for original_owner in classes:
            before, after = records[original_owner], actual_records[rename(original_owner)]
            for key in ("signature", "superclass", "interfaces"):
                if before.get(key) != after.get(key):
                    raise RuntimeError("Original captured class contract changed: " + original_owner + ":" + key)
            if original_owner != owner:
                for key in ("access", "fields", "inner_class", "enclosing_method"):
                    if before.get(key) != after.get(key):
                        raise RuntimeError("Ordinary class metadata changed: " + original_owner + ":" + key)
            else:
                if after["enclosing_method"] != before["enclosing_method"]:
                    raise RuntimeError("Captured class escaped its original enclosing method")
                wanted_fields = {rename(key): {**value, "access": value["access"] & ~0x1000}
                                 for key, value in before["fields"].items()}
                actual_fields = dict(after["fields"])
                for key, field in list(actual_fields.items()):
                    if key in wanted_fields:
                        continue
                    if (not hidden_outer or field["descriptor"] != "Lfixture/Host;" or field["access"] != 0x1010):
                        raise RuntimeError("Unreported field appeared in captured source class")
                    del actual_fields[key]
                if actual_fields != wanted_fields:
                    raise RuntimeError("Original captured field type, Signature or modifiers changed")
            for identity, metadata in before["methods"].items():
                wanted = dict(metadata)
                if identity in named:
                    wanted["name"] = named[identity]["source_name"]
                if identity in forward:
                    wanted["access"] = 0x1
                    wanted["signature"] = forward[identity].get("source_generic_signature")
                if identity in hidden_outer:
                    wanted["prototype"] = "(Lfixture/Host;" + wanted["prototype"][1:]
                if after["methods"][source_ids[identity]] != wanted:
                    raise RuntimeError("Method metadata changed beyond reported void alias/parameters: " + identity)
        aliases = [named[owner + "->" + name + p + "Ljava/lang/Void;"]["source_name"]
                   for name, p in parameters.items()]
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "NonReturningVoidResultsHarness", *aliases], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original wide/narrow dispatch, prefix effects or exception identity changed")

        def reject(negative: Path, label: str) -> None:
            negative_dex = make_dex(negative, work / ("negative-" + label + "-dex"))
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / ("negative-" + label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Void alias proof accepted normal return or parameter scope drift")

        negative = work / "negative-scope"
        shutil.copytree(originals, negative)
        receipt = change_signature(negative / "fixture/Host$1.class", b"(TT;)Ljava/lang/Void;",
                                   b"(Ljava/lang/Object;)Ljava/lang/Void;")
        (work / "negative-scope-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        reject(negative, "scope")
        negative_sources = work / "negative-return-java"
        shutil.copytree(fixture, negative_sources)
        target = negative_sources / "fixture/Host.java"
        content = target.read_text()
        if content.count("throw new Failure();") != 3:
            raise RuntimeError("Missing exact independent original terminal throw")
        target.write_text(content.replace("throw new Failure();", "return null;", 1))
        negative_return = work / "negative-return"
        verify.compile_local(sorted(negative_sources.rglob("*.java")), negative_return)
        (work / "negative-return-input-edits.json").write_text(json.dumps(encode(negative_return), indent=2) + "\n")
        reject(negative_return, "return")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original independently authored input or immutable harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 38, "identity_cases": 64, "ordered_failure_cases": 32,
                    "terminal_void_aliases": 3, "original_call_and_discard_bodies": "retained",
                    "captured_class_formal": "retained", "source_name_parameter_and_binding_differences": "reported",
                    "complete_rebuilt_inventory": "reported source identities and helpers only",
                    "normal_return_and_scope_counterexamples": "rejected", "original_and_harness_bytes": "unchanged"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
