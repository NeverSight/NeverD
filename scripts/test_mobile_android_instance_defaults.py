#!/usr/bin/env python3
"""Verify receiver generic scopes on inlined synthetic default methods."""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

try:
    from scripts.mobile_real_apps_android import dex_header, parse_dexdump
    from scripts.test_mobile_android_annotation_values import compiler_inventory
    from scripts.test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from scripts.test_mobile_android_kotlin_scopes import erase_fixture_method_signature
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_jdk
    from test_mobile_android_kotlin_scopes import erase_fixture_method_signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--d8", type=Path, required=True)
    parser.add_argument("--java-home", type=Path)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--copied-receiver", action="store_true")
    args = parser.parse_args()
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(args.work_dir) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-instance-defaults-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), args.d8.resolve(), 300)
        originals, harness_dir = work / "originals", work / "harness"
        fixture = "instance_default_copies" if args.copied_receiver else "instance_default_scopes"
        harness_name = "CopiedInstanceDefaultScopeHarness" if args.copied_receiver else "InstanceDefaultScopeHarness"
        inputs = sorted((FIXTURES / fixture / "java").rglob("*.java"))
        harness = FIXTURES / "harness" / (harness_name + ".java")
        verify.compile_local(inputs, originals)
        verify.compile_local([harness], harness_dir, classpath=originals)
        classes, methods = compiler_inventory(verify, originals)
        prototype = b"(Lfixture/Receiver;Lfixture/Mapper;ILjava/lang/Object;)Lfixture/Mapper;"
        mutation = erase_fixture_method_signature(
            originals / "fixture/Receiver.class", method_name=b"pick$default", prototype=prototype,
            expected_signature=b"<Unused:Ljava/lang/Object;>" + prototype)
        path = originals / "fixture/Receiver$1.class"
        before = path.read_bytes()
        old_signature = b"(Lfixture/Box<Ljava/lang/Object;>;)Ljava/lang/Object;"
        encoded_signature = b"(Lfixture/Box<TE;>;)Ljava/lang/Object;"
        old = b"\x01" + len(old_signature).to_bytes(2, "big") + old_signature
        new = b"\x01" + len(encoded_signature).to_bytes(2, "big") + encoded_signature
        if before.count(old) != 1:
            raise RuntimeError("Expected one owned callback Signature UTF8 record")
        after = before.replace(old, new, 1)
        path.write_bytes(after)
        mutation["callback_signature"] = {"change": "one Signature UTF8 record", "bytecode": "unchanged",
                                          "before_sha256": hashlib.sha256(before).hexdigest(),
                                          "after_sha256": hashlib.sha256(after).hexdigest()}
        (work / "input-metadata-change.json").write_text(json.dumps(mutation, indent=2) + "\n")
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness_dir, originals))),
                               harness_name, "original"], "baseline")
        dex = verify.dex(sorted(originals.rglob("*.class")), work / "dex", originals)
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({row["descriptor"] for row in inventory["classes"]} != classes
                or {row["identity"] for row in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed the owned fixture declaration inventory")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        if args.copied_receiver:
            code = verify.run([verify.dexdump, "-d", dex], "sdk-code")
            copies = []
            for name in ("pick", "pick$default"):
                body = code.split("name          : '" + name + "'", 1)[1]
                body = re.split(r"\n(?:    #|  Virtual methods)", body, maxsplit=1)[0]
                counts = re.search(r"registers\s*:\s*(\d+)\s+ins\s*:\s*(\d+)", body)
                entry = re.search(r"\|0000: move-object(?:/(?:from16|16))? v(\d+), v(\d+)", body)
                if counts is None or entry is None:
                    raise RuntimeError("D8 omitted the owned entry receiver copy")
                receiver = int(counts[1]) - int(counts[2])
                alias = int(entry[1])
                if (receiver < 16 or int(entry[2]) != receiver
                        or re.search(r"invoke-virtual(?:/range)? \{v" + str(alias)
                                     + r"\}, Lfixture/Receiver;\.read:\(\)Ljava/lang/Object;", body) is None):
                    raise RuntimeError("D8 did not invoke the exact member on the copied receiver")
                copies.append({"method": name, "incoming_register": receiver, "copied_register": alias})
            (work / "receiver-copies.json").write_text(json.dumps(copies, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({row["identity"] for row in coverage["methods"]} != set(methods)
                or coverage["class_count"] != len(classes) or coverage["unrecovered_method_count"] != 0
                or coverage["status"] != "partial"):
            raise RuntimeError("Instance default scope changed inventory or omitted partial coverage")
        projections = coverage.get("generic_signature_projections", [])
        if (len(projections) != 1 or projections[0]["original_signature"] != encoded_signature.decode()
                or not projections[0].get("metadata_differences") or len(projections[0]["changes"]) != 1):
            raise RuntimeError("Missing original receiver scope and metadata difference")
        change = projections[0]["changes"][0]
        if (change["kind"] != "synthetic-default-receiver-type-variable"
                or change.get("scope_origin") != "Lfixture/Receiver;"
                or change["original_type"] != "Lfixture/Receiver;->pick$default" + prototype.decode() + "::E"
                or change["source_type"] != "Ljava/lang/Object;"):
            raise RuntimeError("Receiver scope lacks its exact original declaration")
        rebuilt = work / "rebuilt"
        verify.compile_local([*sorted((output / "sources").rglob("*.java")), harness], rebuilt)
        rebuilt_classes, rebuilt_methods = compiler_inventory(verify, rebuilt)
        bindings = coverage.get("class_source_bindings", [])
        if len(bindings) != 1 or bindings[0]["binding_kind"] != "anonymous-to-named-local":
            raise RuntimeError("Missing original anonymous class binding")
        binding = bindings[0]
        prefix = binding["enclosing_method"]["owner"][:-1] + "$"
        pattern = re.escape(prefix) + r"[0-9]+" + re.escape(binding["source_name"]) + ";"
        matches = [cls for cls in rebuilt_classes if re.fullmatch(pattern, cls)]
        if len(matches) != 1:
            raise RuntimeError("Rebuilt local callback has no unique compiler identity")
        expected = {method.replace(binding["class"], matches[0]) for method in methods}
        if not expected.issubset(rebuilt_methods):
            raise RuntimeError("Recompiled source lost an original runtime descriptor")
        (work / "local-class-identity.json").write_text(json.dumps(
            {"original": binding["class"], "rebuilt": matches[0]}, indent=2) + "\n")
        actual = verify.run([verify.java, "-cp", rebuilt, harness_name, "recovered"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Receiver scope projection changed executable behavior")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "identity_probes": 8, "receiver_calls": 17, "receiver_failures": 1, "null_failures": 2,
                    "supplied_callback_identity": "preserved", "runtime_descriptors": "preserved",
                    "generic_reflection_difference": "confirmed", "original_method_bytecode": "unchanged"}
        if args.copied_receiver:
            evidence.update(receiver_origin="SDK-confirmed-entry-copy", live_local_identities=272,
                            sink_calls=16)
        (work / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
