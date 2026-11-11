#!/usr/bin/env python3
"""Verify safe inherited consumer parameter projection with unchanged runtime bodies."""
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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature


def hashes(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*.class"))}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--android-jar", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--d8", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--unbounded", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-contained-consumer-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        inputs = sorted((FIXTURES / "contained_consumer_parameters/java").rglob("*.java"))
        inputs += [FIXTURES / "value_carrier_bridges/java/fixture/Trace.java"]
        harness_source = FIXTURES / "harness/ContainedConsumerParameterHarness.java"
        receiver_source = next(p for p in inputs if p.name == "Receiver.java")
        if args.unbounded:
            source = work / "unbounded-java/fixture"
            source.mkdir(parents=True)
            owned = []
            for path in inputs:
                content = path.read_text()
                name = path.name
                if name == "Service.java":
                    content = ("package fixture; public final class Scope { public interface Service { "
                               "Object request(Value input, Consumer<?> consumer); } }\n")
                    name = "Scope.java"
                elif name == "Receiver.java":
                    if content.count("Consumer<? super Value>") != 2 or content.count("consumer.resume(input)") != 1:
                        raise RuntimeError("Owned consumer source shape changed")
                    content = content.replace("Consumer<? super Value>", "Consumer<?>")
                    content = content.replace("consumer.resume(input)", "((Consumer) consumer).resume(input)")
                    content = content.replace("implements Service", "implements Scope.Service")
                replacement = source / name
                replacement.write_text(content)
                owned.append(replacement)
                if name == "Receiver.java":
                    receiver_source = replacement
            inputs = owned
            content = harness_source.read_text()
            for original, replacement in (("Service api", "Scope.Service api"),
                                           ("Service absentApi", "Scope.Service absentApi"),
                                           ("Service.class", "Scope.Service.class")):
                if content.count(original) != 1:
                    raise RuntimeError("Owned static interface harness shape changed")
                content = content.replace(original, replacement)
            harness_source = work / "ContainedConsumerParameterHarness.java"
            harness_source.write_text(content)
        verify.compile_local(inputs, originals)
        verify.compile_local([harness_source], harness, classpath=originals)
        owner = "Lfixture/Receiver;"
        prototype = "(Lfixture/Value;Lfixture/Consumer;)Ljava/lang/Object;"
        target = owner + "->request" + prototype
        witness = "Lfixture/Service;->request" + prototype
        target_signature = b"(Lfixture/Value;Lfixture/Consumer<-Lfixture/Value;>;)Ljava/lang/Object;"
        source_signature = b"(Lfixture/Value;Lfixture/Consumer<Ljava/lang/Object;>;)Ljava/lang/Object;"
        contract_owner = "Scope$Service" if args.unbounded else "Service"
        input_contract_signature = target_signature
        if args.unbounded:
            source_signature = target_signature
            target_signature = b"(Lfixture/Value;Lfixture/Consumer<*>;)Ljava/lang/Object;"
            input_contract_signature = target_signature
            witness = "Lfixture/Scope$Service;->request" + prototype
        edits = [change_signature(originals / ("fixture/" + contract_owner + ".class"), input_contract_signature, source_signature)]
        rejected_source = subprocess.run(
            [str(verify.javac), "--release", "8", "-cp", str(originals), "-d", str(work / "invalid-java"),
             str(receiver_source)],
            capture_output=True, text=True, timeout=300, env=verify.environment)
        (work / "original-java-override-rejection.log").write_text(rejected_source.stdout + rejected_source.stderr)
        if rejected_source.returncode == 0 or not any(t in rejected_source.stderr for t in ("name clash", "does not override")):
            raise RuntimeError("Independent javac did not reject the original mismatched source override")
        (work / "owned-input-encoding.json").write_text(json.dumps(edits, indent=2) + "\n")
        records = class_identity.compiler_classes(originals)
        classes, methods = compiler_inventory(verify, originals)
        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        sdk_hash = hashlib.sha256(args.android_jar.read_bytes()).hexdigest()
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "ContainedConsumerParameterHarness", "original", "unbounded" if args.unbounded else "object"], "baseline")
        if baseline.splitlines() != ["values=252,identities=284,failures=8", "reflection=original"]:
            raise RuntimeError("Owned reference forwarding baseline missed expected identity, value or failure cases: " + baseline)
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", args.android_jar.resolve())
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed original reference forwarding identities")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        rows = {r["identity"]: r for r in coverage["methods"]}
        projections = coverage.get("generic_signature_projections", [])
        if (set(rows) != set(methods) or coverage["class_count"] != len(classes) or
                coverage["method_count"] != len(methods) or coverage["unrecovered_method_count"] or
                coverage.get("projected_method_count", 0) != 0 or coverage["status"] != "partial" or
                len(projections) != 1 or projections[0].get("declaration") != target or
                projections[0].get("original_signature") != target_signature.decode() or
                not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Consumer parameter projection lost original inventory or conservative coverage")
        change = projections[0]["changes"][0]
        if (change["kind"] != "inherited-contained-consumer-parameter" or change["scope_origin"] != witness or
                change["original_type"] != target_signature.decode() or change["source_type"] != source_signature.decode()):
            raise RuntimeError("Consumer parameter projection lost its complete original containment witness")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("Consumer parameter projection added or lost an undisclosed runtime slot")
        actual_records = class_identity.compiler_classes(rebuilt)
        for identity in classes:
            before, after = records[identity], actual_records[identity]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                wanted = before.get(key)
                if after.get(key) != wanted:
                    raise RuntimeError("Consumer parameter projection changed class or field metadata: " + identity + ":" + key)
            for member, metadata in before["methods"].items():
                wanted = dict(metadata)
                if member == target:
                    wanted["signature"] = source_signature.decode()
                if after["methods"][member] != wanted:
                    raise RuntimeError("Consumer parameter projection changed complete original method metadata: " + member)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "ContainedConsumerParameterHarness", "recovered", "unbounded" if args.unbounded else "object"], "recovered-execution")
        if actual.splitlines() != [baseline.splitlines()[0], "reflection=reported-consumer-parameter"]:
            raise RuntimeError("Consumer parameter projection changed result identity, mutation, effects or exception order")
        if args.unbounded:
            counterexamples = (
                ("upper-variance", "Receiver", target_signature, source_signature.replace(b"<-", b"<+")),
                ("invariant-value", "Receiver", target_signature, source_signature.replace(b"<-", b"<")),
                ("upper-contract", contract_owner, source_signature, source_signature.replace(b"<-", b"<+")))
        else:
            counterexamples = (
                ("upper-variance", "Receiver", target_signature, target_signature.replace(b"<-", b"<+")),
                ("invariant-value", "Receiver", target_signature, target_signature.replace(b"<-", b"<")),
                ("unbounded-contract", contract_owner, source_signature, source_signature.replace(b"<Ljava/lang/Object;>", b"<*>")))
        for label, negative_owner, old, changed in counterexamples:
            negative = work / ("negative-" + label)
            shutil.copytree(originals, negative)
            receipt = change_signature(negative / ("fixture/" + negative_owner + ".class"), old, changed)
            (work / (label + "-signature.json")).write_text(json.dumps(receipt, indent=2) + "\n")
            negative_dex = verify.dex(sorted(negative.rglob("*.class")), work / (label + "-dex"), args.android_jar.resolve())
            rejected = work / ("rejected-" + label)
            result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                     str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                    capture_output=True, timeout=300, env=verify.environment)
            (work / (label + "-recovery.log")).write_text(result.stdout + result.stderr)
            if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
                raise RuntimeError("Consumer parameter projection accepted same-erasure variance drift with unchanged Code")
        if (hashes(originals) != original_hashes or hashes(harness) != harness_hashes or
                hashlib.sha256(args.android_jar.read_bytes()).hexdigest() != sdk_hash):
            raise RuntimeError("Original input, SDK or immutable harness changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 252, "identity_cases": 284, "ordered_failure_cases": 8,
                    "parameter_containment_direction": "inherited values satisfy the original body contract",
                    "callback_dispatch_order_and_nulls": "matched", "original_result_and_fields": "unchanged",
                    "projection_and_complete_metadata": "matched to report", "original_java_override": "independently rejected",
                    "same_erasure_variance_counterexamples": "rejected",
                    "input_sdk_and_immutable_harness": "unchanged", "coverage": "partial"}
        if args.unbounded:
            evidence["unbounded_consumer_and_static_member_interface"] = "original field wildcard retained; parameter projection reported; callback order matched"
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
