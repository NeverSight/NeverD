"""CI-only Android SDK annotation and recovered Java round-trip regression."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import zipfile

try:
    from scripts import mobile_android_class_identity as identity
    from scripts.test_mobile_android_internal import Verify, choose_d8, choose_jdk, validate_coverage
except ImportError:
    import mobile_android_class_identity as identity
    from test_mobile_android_internal import Verify, choose_d8, choose_jdk, validate_coverage


OWNER = "Lfixture/LintFixture;"
FIXTURES = Path(__file__).parent / "tests/fixtures/mobile/android/suppress-lint"
ANNOTATION_KEYS = ("deprecated_attribute", "runtime_visible_annotations", "runtime_invisible_annotations")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def annotation(values):
    return [{"type": identity.SUPPRESS_LINT_TYPE, "elements": [
        {"name": "value", "tag": "[", "values": [{"tag": "s", "value": value} for value in values]}]}]


def sdk_contract(facts):
    require(facts["name"] == identity.SUPPRESS_LINT_TYPE and facts["access"] == 0x2601
            and facts["superclass"] == "Ljava/lang/Object;"
            and facts["interfaces"] == ["Ljava/lang/annotation/Annotation;"]
            and not facts["fields"], "Unexpected Android SDK SuppressLint declaration")
    method = identity.SUPPRESS_LINT_TYPE + "->value()[Ljava/lang/String;"
    require(set(facts["methods"]) == {method} and facts["methods"][method]["access"] == 0x401
            and not facts["methods"][method]["code"], "Android SDK SuppressLint value type changed")
    annotations = {row["type"]: row["elements"] for row in facts["runtime_visible_annotations"]}
    require(len(annotations) == len(facts["runtime_visible_annotations"]) == 2,
            "Unexpected Android SDK SuppressLint meta annotations")
    require(annotations.get("Ljava/lang/annotation/Retention;") == [
        {"name": "value", "tag": "e", "type": "Ljava/lang/annotation/RetentionPolicy;", "constant": "CLASS"}],
        "Android SDK SuppressLint retention changed")
    targets = annotations.get("Ljava/lang/annotation/Target;", [])
    require(len(targets) == 1 and targets[0].get("name") == "value" and targets[0].get("tag") == "[",
            "Android SDK SuppressLint target shape changed")
    expected = [{"tag": "e", "type": "Ljava/lang/annotation/ElementType;", "constant": value}
                for value in ("TYPE", "FIELD", "METHOD", "PARAMETER", "CONSTRUCTOR", "LOCAL_VARIABLE")]
    require(sorted(targets[0]["values"], key=lambda row: row["constant"]) ==
            sorted(expected, key=lambda row: row["constant"]), "Android SDK SuppressLint targets changed")


def declarations(classes, *, rebuilt=False):
    require(set(classes) == {OWNER}, "SuppressLint fixture class inventory changed")
    cls = classes[OWNER]
    require(cls["major"] == 52 and cls["minor"] == 0,
            "SuppressLint oracle requires Java 8 compiler output")
    require(cls["enclosing_method"] is None and cls["inner_class"] is None,
            "SuppressLint fixture unexpectedly acquired a nested scope")
    require(cls["name"] == OWNER and cls["access"] == 0x21
            and cls["superclass"] == "Ljava/lang/Object;" and not cls["interfaces"]
            and cls["signature"] is None,
            "SuppressLint fixture class declaration changed")
    expected = {"class": (cls, ["ClassIssue", "", "ClassIssue"])}
    require(set(cls["fields"]) == {OWNER + "->value:I"}, "SuppressLint field inventory changed")
    field = cls["fields"][OWNER + "->value:I"]
    require(field["access"] == 1 and field["constant_value"] is None and field["signature"] is None,
            "SuppressLint field changed")
    expected["field"] = field, []
    methods = {"<init>(I)V": (1, ["ConstructorIssue"]),
               "plus(I)I": (1, ["PrivateApi", 'line\n"\\', "", "\0", "\ud800"]),
               "twice(I)I": (9, [])}
    names = {OWNER + "->" + method for method in methods}
    helper = OWNER + "->__neverdThrow" + identity.THROW_HELPER_PROTOTYPE
    require(set(cls["methods"]) == names | ({helper} if rebuilt else set()),
            "SuppressLint method inventory changed")
    for method, (access, values) in methods.items():
        row = cls["methods"][OWNER + "->" + method]
        require(row["access"] == access and row["code"] and row["signature"] is None,
                "SuppressLint method declaration changed: " + method)
        expected[method] = row, values
    if rebuilt:
        row = cls["methods"][helper]
        require(row["access"] == 0xA and row["code"]
                and row["signature"] == identity.THROW_HELPER_SIGNATURE,
                "Unexpected generated helper signature")
        require(not row["deprecated_attribute"] and not row["runtime_visible_annotations"]
                and not row["runtime_invisible_annotations"], "Annotation leaked onto generated helper")
    for label, (row, values) in expected.items():
        require(row["deprecated_attribute"] is False and row["runtime_visible_annotations"] == []
                and row["runtime_invisible_annotations"] == annotation(values),
                "SuppressLint annotation changed at " + label)
    return {label: {key: row[key] for key in ANNOTATION_KEYS} for label, (row, _) in expected.items()}


def behavior(text):
    result = {}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        require(separator and key not in result, "Malformed or duplicate behavior result")
        result[key] = int(value)
    values = (-2147483648, -19, 0, 7, 2147483647)
    signed = lambda value: (value + (1 << 31)) % (1 << 32) - (1 << 31)
    expected = {}
    for initial in values:
        expected[f"field:{initial}"] = initial
        expected[f"twice:{initial}"] = signed(initial * 2)
        for delta in values:
            expected[f"plus:{initial}:{delta}"] = signed(initial + delta)
    require(result == expected, "SuppressLint fixture behavior changed")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True, type=Path)
    parser.add_argument("--d8", required=True, type=Path)
    parser.add_argument("--android-jar", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    require(args.timeout > 0, "Timeout must be positive")
    args.work_dir.mkdir(parents=True, exist_ok=False)
    verify = Verify(args.work_dir, choose_jdk(None), choose_d8(args.d8), args.timeout)
    receipt = {"status": "started", "scope": "SuppressLint declarations and three owned bodies"}
    def save(name, value):
        (args.work_dir / name).write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n")
    try:
        sdk = args.android_jar.resolve(strict=True)
        with zipfile.ZipFile(sdk) as archive:
            name = "android/annotation/SuppressLint.class"
            require(archive.namelist().count(name) == 1, "SDK annotation class is missing or duplicated")
            sdk_bytes = archive.read(name)
        sdk_facts = identity.ClassFile(sdk_bytes, owned_markers=True).facts()
        sdk_contract(sdk_facts)
        save("sdk-contract.json", {"jar_sha256": hashlib.sha256(sdk.read_bytes()).hexdigest(),
                                   "class_sha256": hashlib.sha256(sdk_bytes).hexdigest(), "facts": sdk_facts})
        source = args.work_dir / "source"
        shutil.copytree(FIXTURES / "java", source)
        original_dir = args.work_dir / "original"
        verify.compile_local(sorted(source.rglob("*.java")), original_dir, classpath=sdk)
        original = identity.compiler_classes(original_dir, platform_suppress_lint=True)
        save("original-class-inventory.json", original)
        original_annotations = declarations(original)
        dex = verify.dex(sorted(original_dir.rglob("*.class")), args.work_dir / "dex", sdk)
        save("input.json", {"dex_sha256": hashlib.sha256(dex.read_bytes()).hexdigest(),
                            "expected_classes": [OWNER], "expected_methods": sorted(original[OWNER]["methods"])})
        output = args.work_dir / "recovered"
        verify.run([args.neverd.resolve(strict=True), "mobile", dex, "-o", output,
                    "--timeout", args.timeout, "--json"], "neverd")
        report = json.loads((output / "report.json").read_text())
        helpers = [{"class": OWNER, "name": "__neverdThrow", "prototype": identity.THROW_HELPER_PROTOTYPE,
                    "static": True, "source_unit": "fixture/LintFixture.java", "kind": "throw-helper"}]
        counts = validate_coverage(report, output, {OWNER},
                                   {method: "body" for method in original[OWNER]["methods"]},
                                   {OWNER: "classes.dex"}, expected_generic_helpers=helpers)
        sources = sorted((output / "sources").rglob("*.java"))
        require(report["java_sources"] == ["sources/fixture/LintFixture.java"] and len(sources) == 1,
                "SuppressLint source inventory changed")
        rebuilt_dir = args.work_dir / "rebuilt"
        verify.compile_local(sources, rebuilt_dir, classpath=sdk)
        rebuilt = identity.compiler_classes(rebuilt_dir, platform_suppress_lint=True)
        save("rebuilt-class-inventory.json", rebuilt)
        require(declarations(rebuilt, rebuilt=True) == original_annotations, "Annotation identity changed")
        observations = []
        for label, classes in (("original", original_dir), ("rebuilt", rebuilt_dir)):
            harness = args.work_dir / (label + "-harness")
            verify.compile_local([FIXTURES / "LintHarness.java"], harness, classpath=classes)
            text = verify.run([verify.java, "-cp", os.pathsep.join(map(str, (harness, classes))),
                               "LintHarness"], label + "-behavior")
            observations.append(behavior(text))
        require(observations[0] == observations[1], "Recovered behavior differs")
        save("behavior.json", observations)
        receipt.update(status="success", **counts, annotation_sites=len(original_annotations),
                       behavior_checks=len(observations[0]))
    except Exception as error:
        receipt.update(status="failed", error=str(error))
        raise
    finally:
        save("acceptance.json", receipt)


if __name__ == "__main__":
    main()
