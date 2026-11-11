#!/usr/bin/env python3
"""Verify target annotations and disclosed copying onto erased bridges."""
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
    from scripts.test_mobile_android_shared_upper_bounds import class_records
    from scripts.test_mobile_android_static_defaults import method_code
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    from mobile_real_apps_android import dex_header, parse_dexdump
    from test_mobile_android_annotation_values import compiler_inventory
    from test_mobile_android_internal import FIXTURES, Verify, choose_d8, choose_jdk, class_identity
    from test_mobile_android_nested_return import change_signature
    from test_mobile_android_shared_upper_bounds import class_records
    from test_mobile_android_static_defaults import method_code


class OwnedAnnotationClassFile(class_identity.ClassFile):
    """Add only this fixture's typed method annotation and two exact defaults."""
    def __init__(self, data):
        super().__init__(data, owned_markers=True)

    def element(self, reader):
        tag = reader.take(1)
        if tag == b"I":
            return {"tag": "I", "value": int.from_bytes(self.constant(reader.number(2), 3), "big", signed=True)}
        if tag == b"s":
            return {"tag": "s", "value": self.text(reader.number(2))}
        raise RuntimeError("Owned annotation has an unsupported element tag")

    def deprecation(self, attributes, context, **kwargs):
        ordinary, method_annotations, default = [], None, None
        for kind, data in attributes:
            if kind == "AnnotationDefault":
                if (kwargs.get("allowed", True) is not True or default is not None or
                        context not in ("Lfixture/Stamp;->value()I", "Lfixture/Stamp;->label()Ljava/lang/String;")):
                    raise RuntimeError("Unexpected owned annotation default attachment")
                reader = class_identity.Bytes(data)
                default = self.element(reader)
                reader.finish()
                expected = ({"tag": "I", "value": -1} if "->value()" in context else
                            {"tag": "s", "value": "none"})
                if default != expected:
                    raise RuntimeError("Owned annotation default changed")
                continue
            if kind == "RuntimeVisibleAnnotations" and context.startswith("Lfixture/Core;->get("):
                if kwargs.get("allowed", True) is not True or method_annotations is not None:
                    raise RuntimeError("Duplicate or misplaced owned method annotation")
                reader = class_identity.Bytes(data)
                if reader.number(2) != 1 or self.text(reader.number(2)) != "Lfixture/Stamp;":
                    raise RuntimeError("Owned method annotation type or inventory changed")
                if reader.number(2) != 2:
                    raise RuntimeError("Owned method annotation must preserve both original elements")
                elements = []
                for _ in range(2):
                    name = self.text(reader.number(2))
                    elements.append({"name": name, **self.element(reader)})
                reader.finish()
                elements.sort(key=lambda row: row["name"])
                if elements != [{"name": "label", "tag": "s", "value": "owned"},
                                {"name": "value", "tag": "I", "value": 11}]:
                    raise RuntimeError("Owned method annotation values or element roles changed")
                method_annotations = [{"type": "Lfixture/Stamp;", "elements": elements}]
                continue
            ordinary.append((kind, data))
        result = super().deprecation(ordinary, context, **kwargs)
        if method_annotations is not None:
            if result["runtime_visible_annotations"]:
                raise RuntimeError("Owned method gained another visible annotation")
            result["runtime_visible_annotations"] = method_annotations
        if default is not None:
            result["annotation_default"] = default
        return result


def owned_classes(directory: Path) -> dict:
    classes = {}
    for path in sorted(directory.rglob("*.class")):
        if path.is_symlink():
            raise RuntimeError("Owned compiler inventory contains a symlink")
        facts = OwnedAnnotationClassFile(path.read_bytes()).facts()
        owner = facts["name"]
        if owner in classes or path.relative_to(directory).as_posix() != owner[1:-1] + ".class":
            raise RuntimeError("Owned compiler class identity or inventory changed")
        classes[owner] = facts
    return classes


def omit_owned_bridge_annotation(path: Path) -> dict:
    original = path.read_bytes()
    records = class_records(original)
    offset = records["pool_end"] + 6
    offset += 2 + 2 * int.from_bytes(original[offset:offset + 2], "big")

    def attributes(position: int, count: int) -> tuple:
        spans = []
        for _ in range(count):
            start = position
            name = int.from_bytes(original[position:position + 2], "big")
            position += 6 + int.from_bytes(original[position + 2:position + 6], "big")
            spans.append((records["strings"][name], start, position))
        return position, spans

    fields = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    for _ in range(fields):
        count = int.from_bytes(original[offset + 6:offset + 8], "big")
        offset, _ = attributes(offset + 8, count)
    methods = int.from_bytes(original[offset:offset + 2], "big")
    offset += 2
    selected = []
    for _ in range(methods):
        start = offset
        flags = int.from_bytes(original[offset:offset + 2], "big")
        name = records["strings"][int.from_bytes(original[offset + 2:offset + 4], "big")]
        prototype = records["strings"][int.from_bytes(original[offset + 4:offset + 6], "big")]
        count = int.from_bytes(original[offset + 6:offset + 8], "big")
        offset, spans = attributes(offset + 8, count)
        if name + prototype != b"get(Ljava/lang/Object;)Ljava/lang/Object;":
            continue
        if flags != 0x1041:
            raise RuntimeError("Owned erased method is not the exact compiler bridge")
        for attribute, first, last in spans:
            if attribute == b"RuntimeVisibleAnnotations":
                selected.append((start + 6, count, first, last))
    if len(selected) != 1:
        raise RuntimeError("Owned compiler bridge lacks exactly one copied method annotation attribute")
    count_at, count, first, last = selected[0]
    changed = (original[:count_at] + (count - 1).to_bytes(2, "big") +
               original[count_at + 2:first] + original[last:])
    if method_code(original) != method_code(changed) or class_records(changed)["strings"] != records["strings"]:
        raise RuntimeError("Owned bridge annotation omission changed Code, descriptors or constants")
    path.write_bytes(changed)
    return {"changed_attribute": "one erased bridge RuntimeVisibleAnnotations",
            "code_descriptors_and_constant_pool": "unchanged", "typed_target_annotations": "unchanged"}


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
        prefix="neverd-annotated-bridge-targets-")
    with context as directory:
        work = Path(directory).resolve()
        verify = Verify(work, choose_jdk(args.java_home), choose_d8(args.d8), 300)
        originals, harness = work / "originals", work / "harness"
        fixture = FIXTURES / "annotated_bridge_targets/java"
        inputs = sorted(fixture.rglob("*.java"))
        verify.compile_local(inputs, originals)
        verify.compile_local([FIXTURES / "harness/AnnotatedBridgeTargetsHarness.java"], harness,
                             classpath=originals)
        before_edit = owned_classes(originals)
        bridge = "Lfixture/Core;->get(Ljava/lang/Object;)Ljava/lang/Object;"
        target = "Lfixture/Core;->get(Ljava/lang/Object;)Ljava/lang/String;"
        receipt = omit_owned_bridge_annotation(originals / "fixture/Core.class")
        (work / "owned-bridge-annotation-edit.json").write_text(json.dumps(receipt, indent=2) + "\n")
        classes, methods = compiler_inventory(verify, originals)
        records = owned_classes(originals)
        expected_edit = before_edit["Lfixture/Core;"]["methods"][bridge].copy()
        if expected_edit["runtime_visible_annotations"] != before_edit["Lfixture/Core;"]["methods"][target]["runtime_visible_annotations"]:
            raise RuntimeError("Owned compiler did not copy the original typed annotation")
        expected_edit["runtime_visible_annotations"] = []
        for owner in classes:
            expected = before_edit[owner].copy()
            if owner == "Lfixture/Core;":
                expected["methods"] = dict(expected["methods"], **{bridge: expected_edit})
            if records[owner] != expected:
                raise RuntimeError("Owned annotation omission changed another original declaration")

        def hashes(root: Path) -> dict:
            return {p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(root.rglob("*.class"))}

        original_hashes, harness_hashes = hashes(originals), hashes(harness)
        baseline = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (originals, harness))),
                               "AnnotatedBridgeTargetsHarness"], "baseline")
        expected = ["values=49,identities=118,failures=4",
                    "target-annotations-and-ordered-effects=retained,bridge-annotation-copy=reported"]
        if baseline.splitlines() != expected:
            raise RuntimeError("Owned annotated bridge missed independent cases: " + baseline)

        def make_dex(source: Path, output: Path) -> Path:
            output.mkdir()
            verify.run([*verify.d8, "--debug", "--min-api", "24", "--classpath", args.android_jar.resolve(),
                        "--output", output, *sorted(source.rglob("*.class"))], "d8-" + output.name)
            dex = output / "classes.dex"
            if list(output.glob("classes*.dex")) != [dex]:
                raise RuntimeError("Owned annotated bridge changed DEX inventory")
            return dex

        dex = make_dex(originals, work / "dex")
        dump = verify.run([verify.dexdump, "-f", "-h", "-l", "plain", dex], "sdk-inventory")
        inventory = parse_dexdump(dump, dex.name, dex_header(dex.read_bytes()))
        if ({r["descriptor"] for r in inventory["classes"]} != classes or
                {r["identity"] for r in inventory["methods"]} != set(methods)):
            raise RuntimeError("D8 changed an original annotated bridge declaration")
        (work / "sdk-inventory.json").write_text(json.dumps(inventory, indent=2) + "\n")
        output = work / "recovered"
        verify.run([args.neverd.resolve(), "mobile", dex, "--classpath", args.android_jar.resolve(),
                    "-o", output], "recover")
        coverage = json.loads((output / "report.json").read_text())["android_method_recovery"]
        if ({r["identity"] for r in coverage["methods"]} != set(methods) or
                coverage["class_count"] != len(classes) or coverage["method_count"] != len(methods) or
                coverage["unrecovered_method_count"] or coverage["status"] != "partial" or
                coverage.get("generic_signature_projections") or coverage.get("class_source_bindings")):
            raise RuntimeError("Annotated bridge recovery lost an original entry or scope")
        row = next(r for r in coverage["methods"] if r["identity"] == bridge)
        if (row.get("bridge_target") != target or row.get("projection_kind") != "generic-interface-bridge" or
                row.get("bridge_dispatch") != "Lfixture/Api;->get(Ljava/lang/Object;)Ljava/lang/Object;" or
                not row.get("reason") or row.get("bridge_annotation_source") != target or
                not row.get("metadata_differences")):
            raise RuntimeError("Original annotated bridge lost its target or reported differences")
        rebuilt = work / "rebuilt"
        verify.compile_local(sorted((output / "sources").rglob("*.java")), rebuilt,
                             release=coverage.get("minimum_java_release", 8))
        actual_classes, actual_methods = compiler_inventory(verify, rebuilt)
        helpers = {r["class"] + "->" + r["name"] + r["prototype"]
                   for r in coverage.get("generated_source_helpers", [])}
        if actual_classes != classes or set(actual_methods) != set(methods) | helpers:
            raise RuntimeError("javac changed inventory beyond the reported helpers")
        actual_records = owned_classes(rebuilt)
        for owner in classes:
            before, after = records[owner], actual_records[owner]
            for key in ("access", "signature", "superclass", "interfaces", "fields", "inner_class", "enclosing_method"):
                if after.get(key) != before.get(key):
                    raise RuntimeError("Original annotated bridge class/field metadata changed: " + owner + ":" + key)
            for identity, metadata in before["methods"].items():
                expected_metadata = metadata.copy()
                if identity == bridge:
                    expected_metadata["runtime_visible_annotations"] = records["Lfixture/Core;"]["methods"][target]["runtime_visible_annotations"]
                if after["methods"][identity] != expected_metadata:
                    raise RuntimeError("Original method metadata changed beyond the reported annotation copy: " + identity)
        actual = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (rebuilt, harness))),
                             "AnnotatedBridgeTargetsHarness", "reported-copy"], "recovered-execution")
        if actual != baseline:
            raise RuntimeError("Original default/override dispatch, scopes, identity or effect order changed")
        negative = work / "negative-input"
        shutil.copytree(originals, negative)
        signature = b"Ljava/lang/Object;Lfixture/Api<Ljava/lang/String;>;"
        receipt = change_signature(negative / "fixture/Core.class", signature,
                                   signature.replace(b"Ljava/lang/String;", b"Ljava/lang/Integer;"))
        (work / "negative-parameter-signature.json").write_text(json.dumps(receipt, indent=2) + "\n")
        negative_dex = make_dex(negative, work / "negative-input-dex")
        rejected = work / "rejected-input"
        result = subprocess.run([str(args.neverd.resolve()), "mobile", str(negative_dex), "--classpath",
                                 str(args.android_jar.resolve()), "-o", str(rejected)], text=True,
                                capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-input-recovery.log").write_text(result.stdout + result.stderr)
        if result.returncode == 0 or rejected.exists() or "generic" not in result.stderr:
            raise RuntimeError("Equal erased parameter hid a different complete result contract")
        negative_sources = work / "negative-java"
        compiler_inputs = []
        for source in inputs:
            content = source.read_text()
            if source.name == "Core.java":
                content = content.replace("implements Api<String>", "implements Api<Integer>")
            destination = negative_sources / "fixture" / source.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(content)
            compiler_inputs.append(str(destination))
        compiler = subprocess.run([verify.javac, "--release", "8", "-proc:none", "-d",
                                   str(work / "negative-compiler"), *compiler_inputs], text=True,
                                  capture_output=True, timeout=300, env=verify.environment)
        (work / "negative-compiler.log").write_text(compiler.stdout + compiler.stderr)
        if compiler.returncode == 0:
            raise RuntimeError("Independent Java compiler accepted the changed complete interface result")
        if hashes(originals) != original_hashes or hashes(harness) != harness_hashes:
            raise RuntimeError("Original authored input or immutable JVM harness bytes changed")
        evidence = {"status": "success", "class_count": len(classes), "method_count": len(methods),
                    "value_cases": 49, "identity_cases": 118, "ordered_failure_cases": 4,
                    "complete_method_contract_and_target_annotations": "retained",
                    "original_typed_and_erased_dispatch": "matched", "getter_effects_and_failure_identity": "matched",
                    "bridge_annotation_copy": "exact typed annotation copied and reported",
                    "original_metadata_and_complete_inventory": "only reported bridge annotation copy and helpers",
                    "result_counterexample": "unchanged Code, rejected by NeverD and javac",
                    "original_and_harness_bytes": "unchanged", "coverage": "partial"}
        (work / "oracle-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(json.dumps(evidence))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
