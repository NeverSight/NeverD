#!/usr/bin/env python3
"""Verify built-in Android recovery against independently compiled Java.

A JDK and D8 prepare the self-owned inputs and execution oracle. They are not
used by the built-in decompiler. No downloads occur. Every declared input
method is checked, including constructors, static initializers, abstract and
native declarations. Rebuilt execution uses only recovered Java and the
independent harness. --neverd selects the required native C++ CLI.
"""
from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

try:
    from scripts import mobile_android_class_identity as class_identity
    from scripts import mobile_android_marker_input as marker_input
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    import mobile_android_class_identity as class_identity
    import mobile_android_marker_input as marker_input

ROOT = Path(__file__).resolve().parents[1]

FIXTURES = ROOT / "scripts/tests/fixtures/mobile/android"
SMALI = ROOT / "scripts/tests/fixtures/mobile"
SMALI_METHODS = {
    "Lfixture/Calculator;->compute(I)I": "body",
    "Lfixture/Calculator;->sumAbs([I)I": "body",
    "Lfixture/Calculator;->safeDivide(II)I": "body",
    "Lfixture/Calculator$Nested;->bump(I)I": "body",
    "Lfixture/Peer;->twice(I)I": "body",
    "Lfixture/Peer;->greeting()Ljava/lang/String;": "body",
}
ROUNDING_METHODS = {
    "Lfixture/FloatRounding;->scalar()F": "body",
    "Lfixture/FloatRounding;->array()[F": "body",
}


ClassFile = class_identity.ClassFile


def compiler_inventory(directory: Path) -> tuple[set[str], dict[str, str]]:
    classes, methods = set(), {}
    for path in sorted(directory.rglob("*.class")):
        owner, declared = ClassFile(path.read_bytes()).inventory()
        if owner in classes or methods.keys() & declared.keys(): raise RuntimeError("Duplicate compiler inventory")
        classes.add(owner); methods.update(declared)
    if not classes or not methods: raise RuntimeError("Original compiler emitted no declaration inventory")
    return classes, methods


def choose_jdk(override: Path | None) -> Path:
    candidates = [override] if override else []
    if not override and os.environ.get("JAVA_HOME"): candidates.append(Path(os.environ["JAVA_HOME"]))
    if not override:
        javac = shutil.which("javac")
        if javac: candidates.append(Path(javac).resolve().parent.parent)
        candidates.append(Path("/Applications/Android Studio.app/Contents/jbr/Contents/Home"))
    suffix = ".exe" if os.name == "nt" else ""
    for candidate in candidates:
        if candidate and all((candidate / "bin" / (tool + suffix)).is_file() for tool in ("java", "javac")):
            # macOS /usr/bin tools are launcher stubs, not an installed JDK.
            if sys.platform == "darwin" and candidate == Path("/usr"): continue
            return candidate.resolve()
    raise RuntimeError("A JDK is required for verification; pass --java-home or set JAVA_HOME")


def choose_d8(override: Path | None) -> Path:
    if override or os.environ.get("NEVERD_D8"):
        path = override or Path(os.environ["NEVERD_D8"])
        if not path.is_file(): raise RuntimeError("The configured D8 path is not a file")
        return path.resolve()
    found = shutil.which("d8")
    if found: return Path(found).resolve()
    roots = [Path(value) for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT") if (value := os.environ.get(name))]
    roots += [Path.home() / "Library/Android/sdk", Path.home() / "Android/Sdk"]
    if os.environ.get("LOCALAPPDATA"): roots.append(Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk")
    for root in roots:
        paths = list((root / "build-tools").glob("*/d8.bat" if os.name == "nt" else "*/d8"))
        paths.sort(key=lambda path: (bool(re.fullmatch(r"\d+(?:\.\d+)*", path.parent.name)),
                                     tuple(int(part) for part in re.findall(r"\d+", path.parent.name))), reverse=True)
        for path in paths:
            if path.is_file(): return path.resolve()
    raise RuntimeError("D8 is required only to prepare DEX inputs; pass --d8 or set NEVERD_D8")


def expected_keys(kind: str) -> set[str]:
    if kind == "marker":
        return set(marker_oracle())
    if kind == "deprecated":
        return set(deprecated_oracle())
    if kind == "constructor":
        return set(constructor_oracle())
    if kind == "generic":
        return {*("reflection:" + key for key in GENERIC_REFLECTION_KEYS),
                *(f"{name}:{i}" for name in ("constructor", "identity", "array-identity", "array-null",
                                              "array-read", "array-old", "array-new") for i in range(4)),
                *(f"{name}:{i}:{j}" for name in ("exchange-old", "exchange-new") for i in range(4) for j in range(4)),
                *(f"scalar:{i}" for i in range(7)), "array-exceptions",
                "wildcard-fields", "wildcard-identity", "wildcard-null", "intersection-integer",
                "intersection-long", "intersection-null", "interface-string", "interface-builder",
                "interface-null", "shadow-integer", "shadow-long", "shadow-null", "ops-constructor"}
    if kind == "local":
        return {*(f"{name}:{i}:{j}" for name in ("first", "first-count", "second", "second-count")
                  for i in range(7) for j in range(7)),
                *(f"{name}:{i}" for name in ("wide", "wide-count") for i in range(7)),
                *("reflection:" + role for role in ("first-int", "second-int", "first-long")),
                "final-first", "final-second", "final-wide", "constant-reflection"}
    if kind == "rounding":
        return {"rounding-field", "rounding-scalar", "rounding-length",
                *(f"rounding-array:{i}" for i in range(10))}
    if kind == "single": return {*(f"twice:{i}" for i in range(7)), "greeting"}
    if kind == "legacy":
        return {*(f"{name}:{i}" for name in ("compute", "nested") for i in range(7)),
                *(f"divide:{i}:{j}" for i in range(7) for j in range(7)),
                *(f"sum-abs:{i}" for i in range(5)), "greeting"}
    result = {f"{name}:{i}" for name in ("instance-get", "instance-add", "instance-exchange", "instance-after", "factory", "cross-class", "namespace-call") for i in range(8)}
    result |= {f"{name}:{i}:{j}" for name in ("scalar", "choose", "divide") for i in range(8) for j in range(8)}
    result |= {f"wide:{i}:{j}" for i in range(7) for j in range(7)}
    result |= {f"{name}:{i}" for name in ("float-bits", "double-bits") for i in range(7)}
    result |= {f"{name}:{i}" for name in ("float-add", "double-add", "mixed") for i in range(-3, 4)}
    result |= {f"sum:{i}" for i in range(-1, 36)}
    result |= {f"{name}:{i}" for name in ("array-swap", "array-after") for i in range(5)}
    result |= {f"array-data:{i}" for i in range(12)}
    result |= {f"{name}:{i}" for name in ("packed", "sparse") for i in range(11)}
    result |= {f"string-unit:{i}" for i in range(10)}  # UTF-16 units of neverd/NUL/lambda/emoji
    result |= {"array-length", "null-zero", "null-read", "array-bounds", "static-seed", "static-once",
               "static-float-zero", "static-double-zero", "static-wide", "string-length",
               "abstract-declaration", "native-declaration", "abstract-subclass", "namespace-identity", "namespace-null"}
    return result


def results(text: str, kind: str) -> dict[str, int]:
    values = {}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in values or not re.fullmatch(r"-?\d+", value):
            raise RuntimeError(f"Unexpected or duplicate behavior output: {line!r}")
        values[key] = int(value)
    expected = expected_keys(kind)
    if set(values) != expected:
        raise RuntimeError(f"Behavior inventory changed: missing={sorted(expected - values.keys())}, extra={sorted(values.keys() - expected)}")
    return values


def validate_coverage(report: dict, output: Path, classes: set[str], expected: dict[str, str], inputs: dict[str, str],
                      *, expected_projection: set[str] | None = None,
                      expected_generic_helpers: list[dict] | None = None) -> dict:
    projected = set() if expected_projection is None else expected_projection
    if expected_generic_helpers is not None and (expected_projection is not None or not expected_generic_helpers):
        raise RuntimeError("Invalid independent generic helper inventory")
    if expected_projection is not None and (not projected or not projected <= expected.keys()
                                           or any(expected[key] != "body" for key in projected)):
        raise RuntimeError("Invalid independent projection inventory")
    if report.get("status") != "success" or report.get("platform") != "android":
        raise RuntimeError("Recovery report does not identify a successful Android export")
    if report.get("backend") != {"name": "neverd", "version": "1", "execution": "builtin"}:
        raise RuntimeError("The default path did not use the built-in engine")
    coverage = json.loads((output / "metadata/android-methods.json").read_text())
    if report.get("android_method_recovery") != coverage or coverage.get("status") != ("partial" if projected else "recovered") or coverage.get("schema_version") != 1:
        raise RuntimeError("Standalone method coverage disagrees with the successful report")
    if not projected:
        if coverage.get("projected_method_count", 0) != 0 or coverage.get("class_source_bindings"):
            raise RuntimeError("Ordinary recovery cannot conceal local source projection")
        if expected_generic_helpers is None:
            if coverage.get("generated_source_helpers"):
                raise RuntimeError("Ordinary recovery cannot conceal local source projection")
        else:
            helpers = coverage.get("generated_source_helpers")
            if (not isinstance(helpers, list) or any(not isinstance(row, dict) or type(row.get("static")) is not bool
                                                   for row in helpers) or helpers != expected_generic_helpers):
                raise RuntimeError("Generic generated helper inventory disagrees with independent expected declarations")
    rows = coverage.get("methods")
    if not isinstance(rows, list): raise RuntimeError("Missing method inventory")
    actual = {}
    for row in rows:
        if not isinstance(row, dict) or any(not isinstance(row.get(key), str) for key in ("identity", "class", "name", "prototype", "input")):
            raise RuntimeError("Malformed method identity in coverage inventory")
        identity = row.get("identity")
        if identity in actual or identity not in expected: raise RuntimeError(f"Duplicate/unexpected method: {identity}")
        if identity != row.get("class", "") + "->" + row.get("name", "") + row.get("prototype", ""):
            raise RuntimeError("Method identity fields disagree")
        if row.get("input") != inputs.get(row.get("class")): raise RuntimeError(f"Incorrect input ownership: {identity}")
        count = row.get("instruction_count")
        if type(count) is not int or count < 0: raise RuntimeError("Invalid instruction count")
        wanted = "source-projected" if identity in projected else ("declaration-only" if expected[identity] == "declaration" else "recovered")
        if row.get("status") != wanted or (count != 0) != (wanted != "declaration-only"):
            raise RuntimeError(f"Method body/declaration classification changed: {identity}: {row}")
        if wanted == "declaration-only" and not row.get("reason"): raise RuntimeError("Declaration-only method lacks its reason")
        if wanted == "source-projected" and (row.get("projection_kind") != "named-method-local"
                                            or not isinstance(row.get("reason"), str) or not row["reason"]):
            raise RuntimeError("Source-projected method lacks its precise scope and reason")
        if wanted != "source-projected" and "projection_kind" in row:
            raise RuntimeError("Ordinary method carries an unexpected projection identity")
        actual[identity] = row
    if set(actual) != set(expected): raise RuntimeError(f"Missing methods: {sorted(expected.keys() - actual.keys())}")
    bodies = sum(kind == "body" for kind in expected.values())
    recovered = bodies - len(projected)
    counts = {"class_count": len(classes), "method_count": len(expected), "recovered_method_count": recovered,
              "declaration_only_method_count": len(expected) - bodies, "unrecovered_method_count": 0}
    if projected:
        counts["projected_method_count"] = len(projected)
    if any(type(coverage.get(key)) is not int or coverage.get(key) != value for key, value in counts.items()):
        raise RuntimeError("Aggregate method counts disagree with original compiler declarations")
    paths = sorted(path.relative_to(output).as_posix() for path in (output / "sources").rglob("*.java"))
    if not paths or sorted(report.get("java_sources", [])) != paths or report.get("java_source_count") != len(paths):
        raise RuntimeError("Java artifact inventory is incomplete")
    return counts


def d8_command(d8: Path, java: str) -> list[str]:
    if d8.suffix.lower() in {".bat", ".cmd", ".jar"}:
        jar = d8 if d8.suffix.lower() == ".jar" else d8.parent / "lib/d8.jar"
        if not jar.is_file(): raise RuntimeError("D8 batch launcher requires its lib/d8.jar distribution file")
        return [java, "-cp", str(jar), "com.android.tools.r8.D8"]
    return [str(d8)]


LOCAL_OWNER = "Lfixture/LocalClassBehavior;"
LOCAL_ROLES = {"first-int": (LOCAL_OWNER, "first", "(II)I", "Worker"),
               "second-int": (LOCAL_OWNER, "second", "(II)I", "Worker"),
               "first-long": (LOCAL_OWNER, "first", "(J)J", "Worker")}
LOCAL_CASES = ("local-dex", "local-multidex-outer-first", "local-multidex-overload-first")

GENERIC_BOX = "Lfixture/GenericBox;"
GENERIC_OPS = "Lfixture/GenericOps;"
GENERIC_CASES = ("generic-dex", "generic-multidex")
GENERIC_FIELDS = {
    "value": ("Ljava/lang/Object;", "TT;"),
    "values": ("[Ljava/lang/Object;", "[TT;"),
    "upper": ("Ljava/util/List;", "Ljava/util/List<+TT;>;"),
    "lower": ("Ljava/util/List;", "Ljava/util/List<-TT;>;"),
    "any": ("Ljava/util/List;", "Ljava/util/List<*>;"),
}
GENERIC_METHODS = {
    GENERIC_BOX: {
        "<init>(Ljava/lang/Object;)V": "(TT;)V",
        "get()Ljava/lang/Object;": "()TT;",
        "exchange(Ljava/lang/Object;)Ljava/lang/Object;": "(TT;)TT;",
        "arrayIdentity([Ljava/lang/Object;)[Ljava/lang/Object;": "([TT;)[TT;",
        "first([Ljava/lang/Object;I)Ljava/lang/Object;": "([TT;I)TT;",
        "arrayExchange([Ljava/lang/Object;ILjava/lang/Object;)Ljava/lang/Object;": "([TT;ITT;)TT;",
        "shadow(Ljava/lang/Number;)Ljava/lang/Number;": "<T:Ljava/lang/Number;>(TT;)TT;",
    },
    GENERIC_OPS: {
        "<init>()V": None,
        "identity(Ljava/lang/Object;)Ljava/lang/Object;": "<U:Ljava/lang/Object;>(TU;)TU;",
        "intersection(Ljava/lang/Number;)Ljava/lang/Number;": "<N:Ljava/lang/Number;:Ljava/lang/Comparable<TN;>;>(TN;)TN;",
        "interfaceOnly(Ljava/lang/CharSequence;)Ljava/lang/CharSequence;": "<I::Ljava/lang/CharSequence;>(TI;)TI;",
        "lowerIdentity(Ljava/util/List;)Ljava/util/List;": "<U:Ljava/lang/Object;>(Ljava/util/List<-TU;>;)Ljava/util/List<-TU;>;",
        "scalar(I)I": None,
    },
}
GENERIC_REFLECTION_KEYS = {
    "platform.List", "platform.Comparable",
    "GenericBox.class", "GenericOps.class", "GenericBox.constructor", "GenericOps.constructor",
    *("GenericBox.field." + name for name in GENERIC_FIELDS),
    *(owner[9:-1] + "." + member.split("(", 1)[0]
      for owner, methods in GENERIC_METHODS.items() for member in methods if not member.startswith("<")),
}


CONSTRUCTOR_CASES = ("constructor-dex",)
CONSTRUCTOR_SIGNATURE = "<T:Ljava/lang/Object;:Ljava/lang/CharSequence;>(TT;I)V"
CONSTRUCTOR_DECLARATIONS = {
    "Lfixture/PlainBase;": {
        "access": 0x21, "superclass": "Ljava/lang/Object;",
        "fields": [("objectCalls", "I", 9), ("sequenceCalls", "I", 9),
                   ("tag", "I", 1), ("received", "Ljava/lang/Object;", 1)],
        "methods": {"<init>(Ljava/lang/Object;)V": None, "<init>(Ljava/lang/CharSequence;)V": None},
    },
    "Lfixture/SuperChild;": {
        "access": 0x31, "superclass": "Lfixture/PlainBase;",
        "fields": [("bodyCalls", "I", 9), ("marker", "I", 1)],
        "methods": {"<init>(Ljava/lang/Object;I)V": CONSTRUCTOR_SIGNATURE},
    },
    "Lfixture/ThisChoice;": {
        "access": 0x31, "superclass": "Ljava/lang/Object;",
        "fields": [("objectCalls", "I", 9), ("sequenceCalls", "I", 9), ("delegatingCalls", "I", 9),
                   ("tag", "I", 1), ("marker", "I", 1), ("received", "Ljava/lang/Object;", 1)],
        "methods": {"<init>(Ljava/lang/Object;)V": None, "<init>(Ljava/lang/CharSequence;)V": None,
                    "<init>(Ljava/lang/Object;I)V": CONSTRUCTOR_SIGNATURE},
    },
}
CONSTRUCTOR_REFLECTION = {
    "KIND.java.lang.Object": "java.lang.Object;interface=false",
    "KIND.java.lang.Number": "java.lang.Number;interface=false",
    "KIND.java.lang.String": "java.lang.String;interface=false",
    "KIND.java.lang.Exception": "java.lang.Exception;interface=false",
    "KIND.java.lang.Throwable": "java.lang.Throwable;interface=false",
    "KIND.java.lang.CharSequence": "java.lang.CharSequence;interface=true",
    "KIND.java.io.Serializable": "java.io.Serializable;interface=true",
    "KIND.java.lang.Comparable": "java.lang.Comparable;interface=true",
    "KIND.java.util.List": "java.util.List;interface=true",
    "PlainBase.class": "constructors=2;super=java.lang.Object;formals=0",
    "SuperChild.class": "constructors=1;super=fixture.PlainBase;formals=0",
    "ThisChoice.class": "constructors=3;super=java.lang.Object;formals=0",
    "SuperChild.generic": "fixture.SuperChild#T;bounds=java.lang.Object,java.lang.CharSequence;erased=java.lang.Object,int",
    "ThisChoice.generic": "fixture.ThisChoice#T;bounds=java.lang.Object,java.lang.CharSequence;erased=java.lang.Object,int",
}


MARKER_CASES = ("marker-dex",)


def marker_meta(*, retention=None, targets=None, documented=False, inherited=False):
    """Literal fixture contracts, independent of the DEX/Java implementation."""
    result = []
    if retention is not None:
        result.append({"type": "Ljava/lang/annotation/Retention;", "elements": [
            {"name": "value", "tag": "e", "type": "Ljava/lang/annotation/RetentionPolicy;", "constant": retention}]})
    if targets is not None:
        result.append({"type": "Ljava/lang/annotation/Target;", "elements": [{"name": "value", "tag": "[", "values": [
            {"tag": "e", "type": "Ljava/lang/annotation/ElementType;", "constant": value} for value in targets]}]})
    for name, present in (("Documented", documented), ("Inherited", inherited)):
        if present:
            result.append({"type": "Ljava/lang/annotation/" + name + ";", "elements": []})
    return result


def marker_uses(*names):
    return [{"type": "Lfixture/" + name + ";", "elements": []} for name in names]


MARKER_DEFINITIONS = {
    "MarkerDefault": (None, None, False, False),
    "MarkerClass": ("CLASS", ["TYPE_USE"], False, False),
    "MarkerSource": ("SOURCE", ["TYPE"], False, False),
    "MarkerRuntime": ("RUNTIME", ["TYPE", "ANNOTATION_TYPE"], True, False),
    "MarkerInherited": ("RUNTIME", ["TYPE"], True, True),
    "MarkerEmptyTarget": (None, [], False, False),
    "MarkerTypeOnly": (None, ["TYPE"], False, False),
    "MarkerAnnotationOnly": ("RUNTIME", ["ANNOTATION_TYPE"], False, False),
    "MarkerTagged": (None, None, False, False),
}
MARKER_VISIBLE = {name: marker_meta(retention=retention, targets=targets, documented=documented, inherited=inherited)
                  for name, (retention, targets, documented, inherited) in MARKER_DEFINITIONS.items()}
MARKER_VISIBLE.update({
    "MarkerTagged": marker_uses("MarkerRuntime", "MarkerAnnotationOnly"),
    "MarkerBase": marker_uses("MarkerRuntime", "MarkerInherited"),
    "MarkerChild": [], "MarkerPlain": [], "MarkerImplementer": [], "MarkerSourceUse": [],
    "MarkerInterface": marker_uses("MarkerRuntime", "MarkerInherited"),
})
MARKER_INVISIBLE = {
    "MarkerTagged": marker_uses("MarkerDefault", "MarkerClass", "MarkerTypeOnly"),
    "MarkerBase": marker_uses("MarkerDefault", "MarkerClass"),
}
MARKER_CONCRETE = {
    "MarkerBase": (0x21, "Ljava/lang/Object;", [], [("seed", "I", 1)], ["<init>(I)V", "adjust(I)I"]),
    "MarkerChild": (0x31, "Lfixture/MarkerBase;", [], [], ["<init>(I)V", "shifted(I)I"]),
    "MarkerPlain": (0x31, "Ljava/lang/Object;", [], [], ["<init>()V", "value(I)I"]),
    "MarkerImplementer": (0x31, "Ljava/lang/Object;", ["Lfixture/MarkerInterface;"], [], ["<init>()V", "value(I)I"]),
    "MarkerSourceUse": (0x31, "Ljava/lang/Object;", [], [], ["<init>()V", "value(I)I"]),
}
MARKER_REFLECTION_NAMES = {
    "MarkerDefault": "", "MarkerClass": "java.lang.annotation.Retention,java.lang.annotation.Target",
    "MarkerSource": "java.lang.annotation.Retention,java.lang.annotation.Target",
    "MarkerRuntime": "java.lang.annotation.Documented,java.lang.annotation.Retention,java.lang.annotation.Target",
    "MarkerInherited": "java.lang.annotation.Documented,java.lang.annotation.Inherited,java.lang.annotation.Retention,java.lang.annotation.Target",
    "MarkerEmptyTarget": "java.lang.annotation.Target", "MarkerTypeOnly": "java.lang.annotation.Target",
    "MarkerAnnotationOnly": "java.lang.annotation.Retention,java.lang.annotation.Target",
    "MarkerTagged": "fixture.MarkerAnnotationOnly,fixture.MarkerRuntime",
    "MarkerBase": "fixture.MarkerInherited,fixture.MarkerRuntime", "MarkerChild": "", "MarkerPlain": "",
    "MarkerInterface": "fixture.MarkerInherited,fixture.MarkerRuntime", "MarkerImplementer": "", "MarkerSourceUse": "",
}
MARKER_REFLECTION = {name + suffix: "types=" + value for name, value in MARKER_REFLECTION_NAMES.items()
                     for suffix in (".declared", ".present")}
MARKER_REFLECTION["MarkerChild.present"] = "types=fixture.MarkerInherited"
for _name, (_retention, _targets, _documented, _inherited) in MARKER_DEFINITIONS.items():
    MARKER_REFLECTION[_name + ".definition"] = (
        "annotation=true;members=0;retention=" + (_retention if _retention is not None else "absent")
        + ";target=" + ("absent" if _targets is None else "[" + ",".join(_targets) + "]")
        + ";documented=" + str(_documented).lower() + ";inherited=" + str(_inherited).lower())


def marker_helpers() -> list[dict]:
    return [{"class": "Lfixture/" + name + ";", "name": "__neverdThrow", "prototype": class_identity.THROW_HELPER_PROTOTYPE,
             "static": True, "source_unit": "fixture/" + name + ".java", "kind": "throw-helper"}
            for name in sorted(MARKER_CONCRETE)]


def validate_marker_compiler(classes: dict, *, rebuilt: bool) -> dict:
    """Both sides satisfy fixed facts; matching two equally wrong inputs fails."""
    if type(rebuilt) is not bool or set(classes) != {"Lfixture/" + name + ";" for name in MARKER_VISIBLE}:
        raise RuntimeError("Marker compiler class inventory changed")
    for owner, facts in classes.items():
        name = owner[len("Lfixture/"):-1]
        access, parent, interfaces, field_specs, members = MARKER_CONCRETE.get(name, (
            0x2601 if name in MARKER_DEFINITIONS else 0x601, "Ljava/lang/Object;",
            ["Ljava/lang/annotation/Annotation;"] if name in MARKER_DEFINITIONS else [], [], []))
        if (facts["name"] != owner or facts["major"] != 52 or facts["minor"] != 0
                or type(facts["access"]) is not int or facts["access"] != access
                or facts["superclass"] != parent or facts["interfaces"] != interfaces
                or facts["enclosing_method"] is not None or facts["inner_class"] is not None
                or facts["signature"] is not None or facts["source_file"] != name + ".java"
                or facts.get("deprecated_attribute") is not False):
            raise RuntimeError("Marker compiler declaration violates its fixed contract: " + owner)
        for key, expected in (("runtime_visible_annotations", MARKER_VISIBLE[name]),
                              ("runtime_invisible_annotations", MARKER_INVISIBLE.get(name, []))):
            values = facts.get(key)
            if (not isinstance(values, list) or any(not isinstance(row, dict) or not isinstance(row.get("type"), str)
                                                    for row in values)
                    or sorted(values, key=lambda row: row["type"]) != sorted(expected, key=lambda row: row["type"])):
                raise RuntimeError("Marker compiler annotation contract changed: " + owner + ":" + key)
        fields = {owner + "->" + field + ":" + descriptor: {
            "name": field, "descriptor": descriptor, "access": flags, "constant_value": None,
            "signature": None, **deprecated_facts()} for field, descriptor, flags in field_specs}
        methods = {}
        for member in members:
            method, tail = member.split("(", 1)
            methods[owner + "->" + member] = {"name": method, "prototype": "(" + tail, "access": 1,
                                               "code": True, "signature": None, **deprecated_facts()}
        if rebuilt and name in MARKER_CONCRETE:
            methods[owner + "->__neverdThrow" + class_identity.THROW_HELPER_PROTOTYPE] = {
                "name": "__neverdThrow", "prototype": class_identity.THROW_HELPER_PROTOTYPE,
                "access": 0xA, "code": True, "signature": class_identity.THROW_HELPER_SIGNATURE, **deprecated_facts()}
        if facts["fields"] != fields or facts["methods"] != methods:
            raise RuntimeError("Marker compiler member/helper contract changed: " + owner)
        for row in [*facts["fields"].values(), *facts["methods"].values()]:
            if type(row["access"]) is not int or row.get("deprecated_attribute") is not False:
                raise RuntimeError("Marker compiler member has malformed facts: " + owner)
        if any(type(row["code"]) is not bool for row in facts["methods"].values()):
            raise RuntimeError("Marker compiler method Code fact is not a boolean: " + owner)
    return {"scope": "owned-java8-marker-declarations-and-body", "class_count": 15,
            "annotation_declaration_count": 9, "original_method_count": 10,
            "generated_helper_count": 5 if rebuilt else 0}


def marker_reflection(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("\t")
        if not separator or not value or key in values:
            raise RuntimeError("Malformed or duplicate marker reflection evidence")
        values[key] = value
    if values != MARKER_REFLECTION:
        raise RuntimeError("Marker reflection evidence changed its independent declaration contract")
    return values


def marker_oracle() -> dict[str, int]:
    values = {"reflection:" + key: 1 for key in MARKER_REFLECTION}
    for index, value in enumerate((-100, -1, 0, 1, 7, 100)):
        values.update({"base-seed:" + str(index): value + 3, "base-adjust:" + str(index): 2 * value + 1,
                       "child-seed:" + str(index): value + 3, "child-shifted:" + str(index): 2 * value + 13,
                       "plain:" + str(index): 3 * value - 4, "implements:" + str(index): value + 11,
                       "source-use:" + str(index): value - 7})
    return values


def compare_marker_behavior(baseline: dict, actual: dict):
    expected = marker_oracle()
    for label, values in (("original", baseline), ("rebuilt", actual)):
        if (set(values) != set(expected) or any(type(value) is not int for value in values.values())
                or values != expected):
            raise RuntimeError("Marker " + label + " independent behavior oracle failed")


DEPRECATED_CASES = ("deprecated-dex",)
DEPRECATED_BASE = "Lfixture/DeprecatedBase;"
DEPRECATED_CHILD = "Lfixture/PlainChild;"
DEPRECATED_DECLARATIONS = {
    DEPRECATED_BASE: {
        "access": 0x21, "superclass": "Ljava/lang/Object;", "deprecated": True,
        "fields": [("legacy", 1, True), ("current", 1, False), ("constructorCalls", 9, False)],
        "methods": {"<init>()V": True, "<init>(I)V": False, "oldAdd(I)I": True, "combine(I)I": False},
    },
    DEPRECATED_CHILD: {
        "access": 0x31, "superclass": DEPRECATED_BASE, "deprecated": False,
        "fields": [("own", 1, False)],
        "methods": {"<init>(I)V": False, "childValue(I)I": False},
    },
}
DEPRECATED_REFLECTION = {
    "DeprecatedBase.class": "declared=java.lang.Deprecated;present=true",
    "PlainChild.class": "declared=;present=false",
    "DeprecatedBase.field.legacy": "declared=java.lang.Deprecated;present=true",
    "DeprecatedBase.field.current": "declared=;present=false",
    "DeprecatedBase.field.constructorCalls": "declared=;present=false",
    "PlainChild.field.own": "declared=;present=false",
    "DeprecatedBase.constructor.empty": "declared=java.lang.Deprecated;present=true",
    "DeprecatedBase.constructor.int": "declared=;present=false",
    "PlainChild.constructor.int": "declared=;present=false",
    "DeprecatedBase.method.oldAdd": "declared=java.lang.Deprecated;present=true",
    "DeprecatedBase.method.combine": "declared=;present=false",
    "PlainChild.method.childValue": "declared=;present=false",
    "PlainChild.inherited": "field=fixture.DeprecatedBase;method=fixture.DeprecatedBase;class-present=false",
}


def deprecated_facts(present: bool = False) -> dict:
    return {"deprecated_attribute": present, "runtime_visible_annotations":
            [{"type": "Ljava/lang/Deprecated;", "elements": []}] if present else []}


def deprecated_helpers() -> list[dict]:
    return [{"class": owner, "name": "__neverdThrow", "prototype": class_identity.THROW_HELPER_PROTOTYPE,
             "static": True, "source_unit": owner[1:-1] + ".java", "kind": "throw-helper"}
            for owner in DEPRECATED_DECLARATIONS]


def validate_deprecated_original(classes: dict):
    if set(classes) != set(DEPRECATED_DECLARATIONS):
        raise RuntimeError("Deprecated original class inventory changed")
    for owner, expected in DEPRECATED_DECLARATIONS.items():
        facts = classes[owner]
        if (facts["name"] != owner or facts["major"] != 52 or facts["minor"] != 0
                or facts["access"] != expected["access"] or facts["superclass"] != expected["superclass"]
                or facts["interfaces"] or facts["enclosing_method"] is not None or facts["inner_class"] is not None
                or facts["signature"] is not None
                or facts.get("deprecated_attribute") is not expected["deprecated"]
                or facts.get("runtime_visible_annotations") != deprecated_facts(expected["deprecated"])["runtime_visible_annotations"]):
            raise RuntimeError("Deprecated original class declaration changed: " + owner)
        fields = {owner + "->" + name + ":I": {"name": name, "descriptor": "I", "access": access,
                  "constant_value": None, "signature": None, **deprecated_facts(marked)}
                  for name, access, marked in expected["fields"]}
        methods = {}
        for member, marked in expected["methods"].items():
            name, tail = member.split("(", 1)
            methods[owner + "->" + member] = {"name": name, "prototype": "(" + tail, "access": 1,
                                              "code": True, "signature": None, **deprecated_facts(marked)}
        if facts["fields"] != fields or facts["methods"] != methods:
            raise RuntimeError("Deprecated original field/method marker inventory changed: " + owner)
        if any(type(row.get("deprecated_attribute")) is not bool
               for rows in (facts["fields"], facts["methods"]) for row in rows.values()):
            raise RuntimeError("Deprecated original marker fact is not a boolean")


def deprecated_reflection(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("\t")
        if not separator or not value or key in values:
            raise RuntimeError("Malformed or duplicate Deprecated reflection evidence")
        values[key] = value
    if values != DEPRECATED_REFLECTION:
        raise RuntimeError("Deprecated reflection evidence changed its independent declaration contract")
    return values


def deprecated_oracle() -> dict[str, int]:
    expected = {"reflection:" + key: 1 for key in DEPRECATED_REFLECTION}
    for index, value in enumerate((-100, -1, 0, 1, 7, 100)):
        for route, old, new, combined in (("deprecated", 7, 7 + value, 33 - value),
                                         ("plain", value, value - 2, 2 * value + 9),
                                         ("child", value, value + 2, 2 * value + 9)):
            prefix = f"{route}:{index}:"
            expected.update({prefix + "constructor-delta": 1, prefix + "old": old,
                             prefix + "new": new, prefix + "combine": combined})
        expected[f"child:{index}:own"] = 5 * value - 2
    expected["final:constructor-count"] = 18
    return expected


def compare_deprecated_behavior(baseline: dict, actual: dict):
    oracle = deprecated_oracle()
    for label, values in (("original", baseline), ("rebuilt", actual)):
        if set(values) != set(oracle):
            raise RuntimeError("Deprecated " + label + " behavior omitted an independent key")
        changed = {key: {"expected": expected, "actual": values[key]}
                   for key, expected in oracle.items() if values[key] != expected}
        if changed:
            raise RuntimeError("Deprecated " + label + " behavior oracle failed: " + json.dumps(changed))


def constructor_helpers() -> list[dict]:
    return [{"class": owner, "name": "__neverdThrow", "prototype": class_identity.THROW_HELPER_PROTOTYPE,
             "static": True, "source_unit": owner[1:-1] + ".java", "kind": "throw-helper"}
            for owner in CONSTRUCTOR_DECLARATIONS]


def validate_constructor_original(classes: dict):
    if set(classes) != set(CONSTRUCTOR_DECLARATIONS):
        raise RuntimeError("Constructor original class inventory changed")
    for owner, expected in CONSTRUCTOR_DECLARATIONS.items():
        facts = classes[owner]
        if (facts["name"] != owner or facts["major"] != 52 or facts["minor"] != 0
                or facts["access"] != expected["access"] or facts["superclass"] != expected["superclass"]
                or facts["interfaces"] or facts["enclosing_method"] is not None or facts["inner_class"] is not None
                or facts["signature"] is not None or facts.get("deprecated_attribute") is not False
                or facts.get("runtime_visible_annotations") != []):
            raise RuntimeError("Constructor original class declaration changed: " + owner)
        fields = {owner + "->" + name + ":" + descriptor:
                  {"name": name, "descriptor": descriptor, "access": access,
                   "constant_value": None, "signature": None, **deprecated_facts()}
                  for name, descriptor, access in expected["fields"]}
        methods = {owner + "->" + member:
                   {"name": "<init>", "prototype": member[len("<init>"):], "access": 1,
                    "code": True, "signature": signature, **deprecated_facts()}
                   for member, signature in expected["methods"].items()}
        if facts["fields"] != fields or facts["methods"] != methods:
            raise RuntimeError("Constructor original field/method Signature inventory changed: " + owner)


def constructor_reflection(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("\t")
        if not separator or not value or key in values:
            raise RuntimeError("Malformed or duplicate constructor reflection evidence")
        values[key] = value
    if values != CONSTRUCTOR_REFLECTION:
        raise RuntimeError("Constructor reflection evidence changed its independent declaration contract")
    return values


def constructor_oracle() -> dict[str, int]:
    expected = {"reflection:" + key: 1 for key in CONSTRUCTOR_REFLECTION}
    for route, tags in (("base-direct", (101, 202)), ("this-direct", (303, 404))):
        for choice in range(2):
            for index in range(3):
                prefix = f"{route}:{choice}:{index}:"
                expected.update({prefix + "tag": tags[choice], prefix + "object-count": 1 - choice,
                                 prefix + "sequence-count": choice, prefix + "received": 1})
    for route, tag in (("super", 101), ("this", 303)):
        for index in range(3):
            for marker_index, marker in enumerate((-7, 19)):
                prefix = f"{route}:{index}:{marker_index}:"
                expected.update({prefix + "tag": tag, prefix + "object-count": 1,
                                 prefix + "sequence-count": 0, prefix + "body-count": 1,
                                 prefix + "marker": marker, prefix + "received": 1})
    expected.update({"final:base-object": 9, "final:base-sequence": 3, "final:super-body": 6,
                     "final:this-object": 9, "final:this-sequence": 3, "final:this-body": 6})
    return expected


def compare_constructor_behavior(baseline: dict, actual: dict):
    oracle = constructor_oracle()
    for label, values in (("original", baseline), ("rebuilt", actual)):
        if set(values) != set(oracle):
            raise RuntimeError("Constructor " + label + " behavior omitted an independent key")
        changed = {key: {"expected": expected, "actual": values[key]}
                   for key, expected in oracle.items() if values[key] != expected}
        if changed:
            raise RuntimeError("Constructor " + label + " behavior oracle failed: " + json.dumps(changed))


def generic_helpers() -> list[dict]:
    return [{"class": owner, "name": "__neverdThrow", "prototype": class_identity.THROW_HELPER_PROTOTYPE,
             "static": True, "source_unit": owner[1:-1] + ".java", "kind": "throw-helper"}
            for owner in (GENERIC_BOX, GENERIC_OPS)]


def validate_generic_original(classes: dict):
    """A handwritten contract prevents an accidentally erased fixture becoming the oracle."""
    if set(classes) != {GENERIC_BOX, GENERIC_OPS}:
        raise RuntimeError("Generic original class inventory changed")
    for owner, facts in classes.items():
        signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;" if owner == GENERIC_BOX else None
        if (facts["name"] != owner or facts["major"] != 52 or facts["minor"] != 0 or facts["access"] != 0x31
                or facts["superclass"] != "Ljava/lang/Object;" or facts["interfaces"]
                or facts["enclosing_method"] is not None or facts["inner_class"] is not None
                or facts["signature"] != signature or facts.get("deprecated_attribute") is not False
                or facts.get("runtime_visible_annotations") != []):
            raise RuntimeError("Generic original class Signature/access/scope changed: " + owner)
        fields = {owner + "->" + name + ":" + descriptor:
                  {"name": name, "descriptor": descriptor, "access": 1, "constant_value": None,
                   "signature": generic, **deprecated_facts()}
                  for name, (descriptor, generic) in (GENERIC_FIELDS.items() if owner == GENERIC_BOX else [])}
        if facts["fields"] != fields:
            raise RuntimeError("Generic original field Signature inventory changed: " + owner)
        methods = {}
        for member, generic in GENERIC_METHODS[owner].items():
            name, tail = member.split("(", 1)
            methods[owner + "->" + member] = {"name": name, "prototype": "(" + tail, "code": True,
                                             "access": 9 if owner == GENERIC_OPS and name != "<init>" else 1,
                                             "signature": generic, **deprecated_facts()}
        if facts["methods"] != methods:
            raise RuntimeError("Generic original method Signature inventory changed: " + owner)


def generic_reflection(path: Path) -> dict[str, str]:
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("\t")
        if not separator or not value or key in values:
            raise RuntimeError("Malformed or duplicate generic reflection evidence")
        values[key] = value
    if set(values) != GENERIC_REFLECTION_KEYS:
        raise RuntimeError("Incomplete independent generic reflection evidence")
    return values


def validate_generic_behavior(values: dict):
    if set(values) != expected_keys("generic"):
        raise RuntimeError("Generic behavior requires the complete independent key inventory")
    scalars = [-2147483648, -100, -1, 0, 1, 100, 2147483647]
    for key, value in values.items():
        if key.startswith("scalar:"):
            raw = (scalars[int(key.split(":")[1])] * 3 + 7) & 0xffffffff
            expected = raw if raw < 0x80000000 else raw - 0x100000000
        else:
            expected = 7 if key == "array-exceptions" else 1
        if type(value) is not int or value != expected:
            raise RuntimeError("Generic independent reflection/behavior oracle failed: " + key)


def compare_generic_behavior(baseline: dict, actual: dict):
    validate_generic_behavior(baseline)
    validate_generic_behavior(actual)
    if actual != baseline:
        raise RuntimeError("Generic recovery changed independent behavior")


def local_roles(classes: dict) -> dict[str, str]:
    locals_ = class_identity.local_classes(classes)
    by_key = {}
    for owner, facts in locals_.items():
        key = class_identity.local_key(facts)
        if key in by_key: raise RuntimeError("Ambiguous fixture lexical identity")
        by_key[key] = owner
    if set(by_key) != set(LOCAL_ROLES.values()):
        raise RuntimeError("Fixture must contain the three independently specified Worker scopes")
    return {role: by_key[key] for role, key in LOCAL_ROLES.items()}


def reflection_manifest(classes: dict, path: Path):
    roles = local_roles(classes)
    path.write_text("".join(role + "\t" + owner[1:-1].replace("/", ".") + "\n"
                            for role, owner in sorted(roles.items())), encoding="utf-8")


def local_partitions(classes: dict) -> dict[str, list[list[str]]]:
    roles = local_roles(classes)
    if set(classes) != {LOCAL_OWNER, *roles.values()}:
        raise RuntimeError("Unexpected original local fixture class inventory")
    wide = roles["first-long"]
    return {LOCAL_CASES[0]: [sorted(classes)],
            LOCAL_CASES[1]: [[LOCAL_OWNER], sorted(roles.values())],
            LOCAL_CASES[2]: [[wide], sorted(set(classes) - {wide}, reverse=True)]}


def source_hashes(directory: Path) -> list[dict]:
    return [{"path": path.relative_to(directory).as_posix(), "size": path.stat().st_size,
             "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for path in sorted(directory.rglob("*.java"))]


def compare_local_behavior(baseline: dict, actual: dict):
    if set(baseline) != expected_keys("local") or set(actual) != expected_keys("local"):
        raise RuntimeError("Local behavior comparison requires the complete independent key inventory")
    if actual != baseline:
        changes = {key: {"original": baseline[key], "recovered": actual[key]}
                   for key in baseline if baseline[key] != actual[key]}
        raise RuntimeError("Local Java projection changed behavior: " + json.dumps(changes))


class Verify:
    def __init__(self, work: Path, jdk: Path, d8: Path, timeout: int):
        self.work, self.timeout = work, timeout
        self.environment = os.environ.copy()
        self.environment["JAVA_HOME"] = str(jdk)
        self.environment["PATH"] = str(jdk / "bin") + os.pathsep + self.environment.get("PATH", "")
        self.environment["NEVERD_JADX"] = str(work / "external-decompiler-must-not-run")
        for key in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS"):
            self.environment.pop(key, None)
        suffix = ".exe" if os.name == "nt" else ""
        self.java, self.javac = str(jdk / "bin" / ("java" + suffix)), str(jdk / "bin" / ("javac" + suffix))
        self.d8 = d8_command(d8, self.java)
        sdk_tools = d8.parent.parent if d8.suffix.lower() == ".jar" else d8.parent
        self.dexdump = sdk_tools / ("dexdump" + suffix)
        self.log_index = 0
        (work / "logs").mkdir()

    def run(self, arguments, label: str) -> str:
        self.log_index += 1
        log = self.work / "logs" / f"{self.log_index:03d}-{label}.log"
        command = {"argv": list(map(str, arguments)), "cwd": os.getcwd(),
                   "java_home": self.environment["JAVA_HOME"], "timeout": self.timeout,
                   "log": log.relative_to(self.work).as_posix(), "status": "started"}
        receipt = log.with_suffix(".command.json")
        receipt.write_text(json.dumps(command, indent=2) + "\n")
        try:
            with log.open("x", encoding="utf-8") as stream:
                result = subprocess.run(command["argv"], stdout=stream,
                                        stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                        timeout=self.timeout, env=self.environment, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            command.update(status="failed", error=str(error))
            receipt.write_text(json.dumps(command, indent=2) + "\n")
            raise
        command.update(status="success" if result.returncode == 0 else "failed", exit_code=result.returncode)
        receipt.write_text(json.dumps(command, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"{label} exited {result.returncode}: {log.read_text(errors='replace')}")
        return log.read_text(encoding="utf-8", errors="replace")

    def compile(self, sources, output: Path, *, classpath: Path | None = None):
        output.mkdir(parents=True)
        arguments = [self.javac, "-encoding", "UTF-8", "-g", "-d", output]
        if classpath: arguments += ["-classpath", classpath]
        self.run([*arguments, *sources], "javac")

    def compile_local(self, sources, output: Path, *, classpath: Path | None = None):
        empty = self.work / "local-empty-classpath"
        empty.mkdir(exist_ok=True)
        if any(empty.iterdir()): raise RuntimeError("Local fixture isolation directory is not empty")
        output.mkdir(parents=True)
        self.run([self.javac, "--release", "8", "-encoding", "UTF-8", "-g", "-proc:none",
                  "-implicit:none", "-sourcepath", empty, "-processorpath", empty,
                  "-classpath", classpath if classpath is not None else empty,
                  "-d", output, *sources], "javac-local")

    def original(self, source: Path, harness: str, name: str, kind: str):
        directory = self.work / name
        self.compile(sorted(source.rglob("*.java")), directory / "classes")
        self.compile([FIXTURES / "harness" / (harness + ".java")], directory / "harness", classpath=directory / "classes")
        cp = os.pathsep.join(map(str, (directory / "harness", directory / "classes")))
        baseline = results(self.run([self.java, "-cp", cp, harness], "original-" + kind), kind)
        (directory / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
        return directory / "classes", baseline

    def dex(self, classes, output: Path, classpath: Path, *, intermediate: bool = False):
        output.mkdir(parents=True)
        self.run([*self.d8, "--debug", *(["--intermediate"] if intermediate else []),
                  "--classpath", classpath, "--output", output, *classes], "d8")
        dex = output / "classes.dex"
        if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Fixture partition did not produce exactly one DEX")
        return dex

    def marker_dex_input(self, source: Path) -> dict:
        """Keep the SDK's post-D8 evidence separate from the javac baseline."""
        receipt = source.parent.parent / "input-marker-annotations.json"
        evidence = {"schema_version": 1, "stage": "post-d8", "status": "started",
                    "scope": "owned-markerbase-class-annotations", "dexdump": str(self.dexdump)}
        try:
            data = source.read_bytes()
            evidence["input_sha256"] = hashlib.sha256(data).hexdigest()
            if not self.dexdump.is_file():
                raise RuntimeError("Marker input requires the SDK dexdump beside D8")
            evidence["dexdump_sha256"] = hashlib.sha256(self.dexdump.read_bytes()).hexdigest()
            dump = self.run([self.dexdump, "-a", "-f", "-h", "-l", "plain", source],
                            "marker-input-dexdump")
            if source.read_bytes() != data:
                raise RuntimeError("Marker input changed during SDK inspection")
            evidence.update(marker_input.inspect_marker_dex(data, dump))
            if set(evidence["class_descriptors"]) != {"Lfixture/" + name + ";" for name in MARKER_VISIBLE}:
                raise RuntimeError("Marker post-D8 class inventory changed")
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            evidence.update(status="failed", error=str(error))
            receipt.write_text(json.dumps(evidence, indent=2) + "\n")
            raise
        evidence["status"] = "success"
        receipt.write_text(json.dumps(evidence, indent=2) + "\n")
        return evidence

    def rebuilt(self, output: Path, harness: str, kind: str, baseline: dict):
        compiled = output.parent / (output.name + "-compiled")
        sources = sorted((output / "sources").rglob("*.java"))
        self.compile([*sources, FIXTURES / "harness" / (harness + ".java")], compiled)
        actual = results(self.run([self.java, "-cp", compiled, harness], "recovered-" + kind), kind)
        (output / "execution.json").write_text(json.dumps(actual, indent=2) + "\n")
        if actual != baseline:
            changes = {key: {"original": baseline[key], "recovered": actual[key]} for key in baseline if baseline[key] != actual[key]}
            raise RuntimeError("Recovered Java changed behavior: " + json.dumps(changes))
        return len(actual)

    def marker_cases(self, neverd: Path) -> tuple[list[dict], list[dict]]:
        passed, failures = [], []
        directory = self.work / "original-marker"
        try:
            self.run([self.javac, "-version"], "marker-javac-version")
            self.run([self.java, "-version"], "marker-java-version")
            source_dir = directory / "source"
            for path in sorted((FIXTURES / "marker/java").rglob("*.java")):
                target = source_dir / path.relative_to(FIXTURES / "marker/java")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
            harness_source = directory / "harness-source/MarkerHarness.java"
            harness_source.parent.mkdir(parents=True)
            shutil.copyfile(FIXTURES / "harness/MarkerHarness.java", harness_source)
            classes_dir = directory / "classes"
            self.compile_local(sorted(source_dir.rglob("*.java")), classes_dir)
            original = class_identity.compiler_classes(classes_dir, owned_markers=True)
            (directory / "class-inventory.json").write_text(json.dumps(original, indent=2) + "\n")
            original_contract = validate_marker_compiler(original, rebuilt=False)
            (directory / "marker-identity.json").write_text(json.dumps(original_contract, indent=2) + "\n")
            methods = {identity: "body" if row["code"] else "declaration"
                       for facts in original.values() for identity, row in facts["methods"].items()}
            self.compile_local([harness_source], directory / "harness", classpath=classes_dir)
            cp = os.pathsep.join(map(str, (directory / "harness", classes_dir)))
            reflection_path = directory / "reflection.tsv"
            baseline = results(self.run([self.java, "-cp", cp, "MarkerHarness", reflection_path],
                                        "original-marker"), "marker")
            (directory / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
            compare_marker_behavior(baseline, baseline)
            reflection = marker_reflection(reflection_path)
            (directory / "reflection.json").write_text(json.dumps(reflection, indent=2) + "\n")
            (directory / "source-hashes.json").write_text(json.dumps({"implementation": source_hashes(source_dir),
                "harness": source_hashes(harness_source.parent)}, indent=2) + "\n")
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            for label in MARKER_CASES:
                failures.append({"case": label, "stage": "original-preparation", "error": str(error)})
            (self.work / "marker-preparation-failure.json").write_text(json.dumps(failures, indent=2) + "\n")
            return passed, failures
        for label in MARKER_CASES:
            case = self.work / label
            case.mkdir()
            try:
                inputs = {owner: "classes.dex" for owner in original}
                paths = [classes_dir / original[owner]["path"] for owner in sorted(original)]
                source = self.dex(paths, case / "dex", classes_dir, intermediate=True)
                dex_annotations = self.marker_dex_input(source)
                input_sha256 = hashlib.sha256(source.read_bytes()).hexdigest()
                if input_sha256 != dex_annotations["input_sha256"]:
                    raise RuntimeError("Marker input changed after SDK inspection")
                (case / "input-inventory.json").write_text(json.dumps({"classes": original, "inputs": inputs,
                    "classes_provider": "original-javac-before-d8", "dex_annotations": dex_annotations,
                    "expected_generated_helpers": marker_helpers(),
                    "input_sha256": input_sha256}, indent=2) + "\n")
                output = case / "recovered"
                self.run([neverd, "mobile", source, "-o", output, "--timeout", self.timeout, "--json"], label)
                report = json.loads((output / "report.json").read_text())
                if report.get("dex_count") != 1 or report.get("smali_count") != 0:
                    raise RuntimeError("Marker DEX input inventory changed")
                counts = validate_coverage(report, output, set(original), methods, inputs,
                                           expected_generic_helpers=marker_helpers())
                if any(row.get("deprecated") is not False for row in report["android_method_recovery"]["methods"]):
                    raise RuntimeError("Marker method inventory invented a Deprecated declaration")
                if sorted(report["java_sources"]) != ["sources/" + owner[1:-1] + ".java" for owner in sorted(original)]:
                    raise RuntimeError("Marker source unit inventory changed")
                sources = sorted((output / "sources").rglob("*.java"))
                (case / "generated-source-hashes.json").write_text(json.dumps(source_hashes(output / "sources"), indent=2) + "\n")
                compiled = case / "compiled"
                self.compile_local(sources, compiled)
                rebuilt = class_identity.compiler_classes(compiled, owned_markers=True)
                (case / "rebuilt-class-inventory.json").write_text(json.dumps(rebuilt, indent=2) + "\n")
                identity = validate_marker_compiler(rebuilt, rebuilt=True)
                (case / "marker-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
                self.compile_local([harness_source], case / "harness", classpath=compiled)
                cp = os.pathsep.join(map(str, (case / "harness", compiled)))
                reflection_path = case / "reflection.tsv"
                actual = results(self.run([self.java, "-cp", cp, "MarkerHarness", reflection_path],
                                          label + "-execution"), "marker")
                (case / "execution.json").write_text(json.dumps(actual, indent=2) + "\n")
                actual_reflection = marker_reflection(reflection_path)
                (case / "reflection.json").write_text(json.dumps(actual_reflection, indent=2) + "\n")
                compare_marker_behavior(baseline, actual)
                passed.append({"case": label, **counts, "matched_results": len(actual),
                               "reflection_declaration_count": len(reflection), "compiler_identity": identity,
                               "acceptance_scope": "owned-java8-marker-declarations-and-body"})
                print(f"PASS {label}: {len(methods)} original bodies, 9 marker definitions, {len(actual)} independent results", flush=True)
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                failure = {"case": label, "error": str(error)}
                failures.append(failure)
                (case / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
                print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
        if {row["case"] for row in passed + failures} != set(MARKER_CASES) or len(passed + failures) != len(MARKER_CASES):
            raise RuntimeError("Marker acceptance omitted a required input case")
        return passed, failures

    def deprecated_cases(self, neverd: Path) -> tuple[list[dict], list[dict]]:
        passed, failures = [], []
        directory = self.work / "original-deprecated"
        try:
            self.run([self.javac, "-version"], "deprecated-javac-version")
            self.run([self.java, "-version"], "deprecated-java-version")
            source_dir = directory / "source"
            for path in sorted((FIXTURES / "deprecated/java").rglob("*.java")):
                target = source_dir / path.relative_to(FIXTURES / "deprecated/java")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
            harness_source = directory / "harness-source/DeprecatedHarness.java"
            harness_source.parent.mkdir(parents=True)
            shutil.copyfile(FIXTURES / "harness/DeprecatedHarness.java", harness_source)
            classes_dir = directory / "classes"
            self.compile_local(sorted(source_dir.rglob("*.java")), classes_dir)
            original = class_identity.compiler_classes(classes_dir)
            (directory / "class-inventory.json").write_text(json.dumps(original, indent=2) + "\n")
            validate_deprecated_original(original)
            methods = {identity: "body" if row["code"] else "declaration"
                       for facts in original.values() for identity, row in facts["methods"].items()}
            self.compile_local([harness_source], directory / "harness", classpath=classes_dir)
            cp = os.pathsep.join(map(str, (directory / "harness", classes_dir)))
            reflection_path = directory / "reflection.tsv"
            baseline = results(self.run([self.java, "-cp", cp, "DeprecatedHarness", reflection_path],
                                        "original-deprecated"), "deprecated")
            (directory / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
            compare_deprecated_behavior(baseline, baseline)
            reflection = deprecated_reflection(reflection_path)
            (directory / "reflection.json").write_text(json.dumps(reflection, indent=2) + "\n")
            (directory / "source-hashes.json").write_text(json.dumps({"implementation": source_hashes(source_dir),
                "harness": source_hashes(harness_source.parent)}, indent=2) + "\n")
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            for label in DEPRECATED_CASES:
                failures.append({"case": label, "stage": "original-preparation", "error": str(error)})
            (self.work / "deprecated-preparation-failure.json").write_text(json.dumps(failures, indent=2) + "\n")
            return passed, failures
        for label in DEPRECATED_CASES:
            case = self.work / label
            case.mkdir()
            try:
                inputs = {owner: "classes.dex" for owner in original}
                paths = [classes_dir / original[owner]["path"] for owner in sorted(original)]
                source = self.dex(paths, case / "dex", classes_dir)
                (case / "input-inventory.json").write_text(json.dumps({"classes": original, "inputs": inputs,
                    "expected_generated_helpers": deprecated_helpers(),
                    "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}, indent=2) + "\n")
                output = case / "recovered"
                self.run([neverd, "mobile", source, "-o", output, "--timeout", self.timeout, "--json"], label)
                report = json.loads((output / "report.json").read_text())
                if report.get("dex_count") != 1 or report.get("smali_count") != 0:
                    raise RuntimeError("Deprecated DEX input inventory changed")
                counts = validate_coverage(report, output, set(original), methods, inputs,
                                           expected_generic_helpers=deprecated_helpers())
                for row in report["android_method_recovery"]["methods"]:
                    expected = original[row["class"]]["methods"][row["identity"]]["deprecated_attribute"]
                    if row.get("deprecated") is not expected:
                        raise RuntimeError("Deprecated method marker disagrees with original declaration: " + row["identity"])
                sources = sorted((output / "sources").rglob("*.java"))
                if sorted(report["java_sources"]) != ["sources/" + owner[1:-1] + ".java" for owner in sorted(original)]:
                    raise RuntimeError("Deprecated source unit inventory changed")
                (case / "generated-source-hashes.json").write_text(json.dumps(source_hashes(output / "sources"), indent=2) + "\n")
                compiled = case / "compiled"
                self.compile_local(sources, compiled)
                rebuilt = class_identity.compiler_classes(compiled)
                (case / "rebuilt-class-inventory.json").write_text(json.dumps(rebuilt, indent=2) + "\n")
                identity = class_identity.match_deprecated_recompiled(original, rebuilt)
                (case / "deprecated-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
                self.compile_local([harness_source], case / "harness", classpath=compiled)
                cp = os.pathsep.join(map(str, (case / "harness", compiled)))
                reflection_path = case / "reflection.tsv"
                actual = results(self.run([self.java, "-cp", cp, "DeprecatedHarness", reflection_path],
                                          label + "-execution"), "deprecated")
                (case / "execution.json").write_text(json.dumps(actual, indent=2) + "\n")
                actual_reflection = deprecated_reflection(reflection_path)
                (case / "reflection.json").write_text(json.dumps(actual_reflection, indent=2) + "\n")
                if actual_reflection != reflection:
                    raise RuntimeError("Deprecated reflection declarations changed")
                compare_deprecated_behavior(baseline, actual)
                passed.append({"case": label, **counts, "matched_results": len(actual),
                               "reflection_declaration_count": len(reflection), "compiler_identity": identity,
                               "acceptance_scope": "owned-deprecated-marker-and-body"})
                print(f"PASS {label}: {len(methods)} original bodies, {len(reflection)} Deprecated reflection declarations, {len(actual)} results", flush=True)
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                failure = {"case": label, "error": str(error)}
                failures.append(failure)
                (case / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
                print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
        if {row["case"] for row in passed + failures} != set(DEPRECATED_CASES) or len(passed + failures) != len(DEPRECATED_CASES):
            raise RuntimeError("Deprecated acceptance omitted a required input case")
        return passed, failures

    def constructor_cases(self, neverd: Path) -> tuple[list[dict], list[dict]]:
        passed, failures = [], []
        directory = self.work / "original-constructor"
        try:
            self.run([self.javac, "-version"], "constructor-javac-version")
            self.run([self.java, "-version"], "constructor-java-version")
            source_dir = directory / "source"
            for path in sorted((FIXTURES / "constructor/java").rglob("*.java")):
                target = source_dir / path.relative_to(FIXTURES / "constructor/java")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
            harness_source = directory / "harness-source/ConstructorHarness.java"
            harness_source.parent.mkdir(parents=True)
            shutil.copyfile(FIXTURES / "harness/ConstructorHarness.java", harness_source)
            classes_dir = directory / "classes"
            self.compile_local(sorted(source_dir.rglob("*.java")), classes_dir)
            original = class_identity.compiler_classes(classes_dir)
            (directory / "class-inventory.json").write_text(json.dumps(original, indent=2) + "\n")
            validate_constructor_original(original)
            methods = {identity: "body" if row["code"] else "declaration"
                       for facts in original.values() for identity, row in facts["methods"].items()}
            self.compile_local([harness_source], directory / "harness", classpath=classes_dir)
            cp = os.pathsep.join(map(str, (directory / "harness", classes_dir)))
            reflection_path = directory / "reflection.tsv"
            baseline = results(self.run([self.java, "-cp", cp, "ConstructorHarness", reflection_path],
                                        "original-constructor"), "constructor")
            (directory / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
            compare_constructor_behavior(baseline, baseline)
            reflection = constructor_reflection(reflection_path)
            (directory / "reflection.json").write_text(json.dumps(reflection, indent=2) + "\n")
            (directory / "source-hashes.json").write_text(json.dumps({"implementation": source_hashes(source_dir),
                "harness": source_hashes(harness_source.parent)}, indent=2) + "\n")
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            for label in CONSTRUCTOR_CASES:
                failures.append({"case": label, "stage": "original-preparation", "error": str(error)})
            (self.work / "constructor-preparation-failure.json").write_text(json.dumps(failures, indent=2) + "\n")
            return passed, failures
        for label in CONSTRUCTOR_CASES:
            case = self.work / label
            case.mkdir()
            try:
                inputs = {owner: "classes.dex" for owner in original}
                paths = [classes_dir / original[owner]["path"] for owner in sorted(original)]
                source = self.dex(paths, case / "dex", classes_dir)
                (case / "input-inventory.json").write_text(json.dumps({"classes": original, "inputs": inputs,
                    "expected_generated_helpers": constructor_helpers(),
                    "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}, indent=2) + "\n")
                output = case / "recovered"
                self.run([neverd, "mobile", source, "-o", output, "--timeout", self.timeout, "--json"], label)
                report = json.loads((output / "report.json").read_text())
                if report.get("dex_count") != 1 or report.get("smali_count") != 0:
                    raise RuntimeError("Constructor DEX input inventory changed")
                counts = validate_coverage(report, output, set(original), methods, inputs,
                                           expected_generic_helpers=constructor_helpers())
                sources = sorted((output / "sources").rglob("*.java"))
                if sorted(report["java_sources"]) != ["sources/" + owner[1:-1] + ".java" for owner in sorted(original)]:
                    raise RuntimeError("Constructor source unit inventory changed")
                (case / "generated-source-hashes.json").write_text(json.dumps(source_hashes(output / "sources"), indent=2) + "\n")
                compiled = case / "compiled"
                self.compile_local(sources, compiled)
                rebuilt = class_identity.compiler_classes(compiled)
                (case / "rebuilt-class-inventory.json").write_text(json.dumps(rebuilt, indent=2) + "\n")
                identity = class_identity.match_generic_recompiled(original, rebuilt)
                (case / "constructor-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
                self.compile_local([harness_source], case / "harness", classpath=compiled)
                cp = os.pathsep.join(map(str, (case / "harness", compiled)))
                reflection_path = case / "reflection.tsv"
                actual = results(self.run([self.java, "-cp", cp, "ConstructorHarness", reflection_path],
                                          label + "-execution"), "constructor")
                (case / "execution.json").write_text(json.dumps(actual, indent=2) + "\n")
                actual_reflection = constructor_reflection(reflection_path)
                (case / "reflection.json").write_text(json.dumps(actual_reflection, indent=2) + "\n")
                if actual_reflection != reflection:
                    raise RuntimeError("Constructor reflection declarations changed")
                compare_constructor_behavior(baseline, actual)
                passed.append({"case": label, **counts, "matched_results": len(actual),
                               "reflection_declaration_count": len(reflection), "compiler_identity": identity,
                               "acceptance_scope": "owned-constructor-overload-binding"})
                print(f"PASS {label}: {len(methods)} original constructors, {len(actual)} reflection/behavior results", flush=True)
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                failure = {"case": label, "error": str(error)}
                failures.append(failure)
                (case / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
                print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
        if {row["case"] for row in passed + failures} != set(CONSTRUCTOR_CASES) or len(passed + failures) != len(CONSTRUCTOR_CASES):
            raise RuntimeError("Constructor acceptance omitted a required input case")
        return passed, failures

    def generic_cases(self, neverd: Path) -> tuple[list[dict], list[dict]]:
        passed, failures = [], []
        directory = self.work / "original-generic"
        try:
            self.run([self.javac, "-version"], "generic-javac-version")
            self.run([self.java, "-version"], "generic-java-version")
            source_dir = directory / "source"
            for path in sorted((FIXTURES / "generic/java").rglob("*.java")):
                target = source_dir / path.relative_to(FIXTURES / "generic/java")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
            harness_source = directory / "harness-source/GenericHarness.java"
            harness_source.parent.mkdir(parents=True)
            shutil.copyfile(FIXTURES / "harness/GenericHarness.java", harness_source)
            classes_dir = directory / "classes"
            self.compile_local(sorted(source_dir.rglob("*.java")), classes_dir)
            original = class_identity.compiler_classes(classes_dir)
            (directory / "class-inventory.json").write_text(json.dumps(original, indent=2) + "\n")
            validate_generic_original(original)
            methods = {identity: "body" if row["code"] else "declaration"
                       for facts in original.values() for identity, row in facts["methods"].items()}
            self.compile_local([harness_source], directory / "harness", classpath=classes_dir)
            cp = os.pathsep.join(map(str, (directory / "harness", classes_dir)))
            reflection_path = directory / "reflection.tsv"
            baseline = results(self.run([self.java, "-cp", cp, "GenericHarness", reflection_path], "original-generic"), "generic")
            validate_generic_behavior(baseline)
            reflection = generic_reflection(reflection_path)
            (directory / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
            (directory / "reflection.json").write_text(json.dumps(reflection, indent=2) + "\n")
            (directory / "source-hashes.json").write_text(json.dumps({"implementation": source_hashes(source_dir),
                "harness": source_hashes(harness_source.parent)}, indent=2) + "\n")
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            for label in GENERIC_CASES:
                failures.append({"case": label, "stage": "original-preparation", "error": str(error)})
            (self.work / "generic-preparation-failure.json").write_text(json.dumps(failures, indent=2) + "\n")
            return passed, failures
        partitions = {"generic-dex": [[GENERIC_BOX, GENERIC_OPS]],
                      "generic-multidex": [[GENERIC_OPS], [GENERIC_BOX]]}
        for label in GENERIC_CASES:
            case = self.work / label
            case.mkdir()
            try:
                inputs, dexes = {}, []
                for number, owners in enumerate(partitions[label], 1):
                    dex_name = "classes.dex" if number == 1 else f"classes{number}.dex"
                    paths = [classes_dir / original[owner]["path"] for owner in owners]
                    dexes.append(self.dex(paths, case / ("partition-" + str(number)), classes_dir))
                    inputs.update({owner: dex_name for owner in owners})
                if len(dexes) == 1:
                    source = dexes[0]
                else:
                    source = case / "input.apk"
                    with zipfile.ZipFile(source, "w") as archive:
                        for number, dex in enumerate(dexes, 1):
                            archive.write(dex, "classes.dex" if number == 1 else f"classes{number}.dex")
                (case / "input-inventory.json").write_text(json.dumps({"classes": original, "inputs": inputs,
                    "partitions": partitions[label], "expected_generated_helpers": generic_helpers(),
                    "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}, indent=2) + "\n")
                output = case / "recovered"
                self.run([neverd, "mobile", source, "-o", output, "--timeout", self.timeout, "--json"], label)
                report = json.loads((output / "report.json").read_text())
                if report.get("dex_count") != len(dexes) or report.get("smali_count") != 0:
                    raise RuntimeError("Generic DEX partition inventory changed")
                counts = validate_coverage(report, output, set(original), methods, inputs,
                                           expected_generic_helpers=generic_helpers())
                if sorted(report["java_sources"]) != ["sources/fixture/GenericBox.java", "sources/fixture/GenericOps.java"]:
                    raise RuntimeError("Generic source unit inventory changed")
                (case / "generated-source-hashes.json").write_text(json.dumps(source_hashes(output / "sources"), indent=2) + "\n")
                compiled = case / "compiled"
                self.compile_local(sorted((output / "sources").rglob("*.java")), compiled)
                rebuilt = class_identity.compiler_classes(compiled)
                (case / "rebuilt-class-inventory.json").write_text(json.dumps(rebuilt, indent=2) + "\n")
                identity = class_identity.match_generic_recompiled(original, rebuilt)
                (case / "generic-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
                self.compile_local([harness_source], case / "harness", classpath=compiled)
                cp = os.pathsep.join(map(str, (case / "harness", compiled)))
                reflection_path = case / "reflection.tsv"
                actual = results(self.run([self.java, "-cp", cp, "GenericHarness", reflection_path], label + "-execution"), "generic")
                (case / "execution.json").write_text(json.dumps(actual, indent=2) + "\n")
                actual_reflection = generic_reflection(reflection_path)
                (case / "reflection.json").write_text(json.dumps(actual_reflection, indent=2) + "\n")
                if actual_reflection != reflection:
                    raise RuntimeError("Generic reflection declarations changed")
                compare_generic_behavior(baseline, actual)
                passed.append({"case": label, **counts, "matched_results": len(actual),
                               "reflection_declaration_count": len(reflection), "compiler_identity": identity,
                               "acceptance_scope": "owned-generic-signature-and-body"})
                print(f"PASS {label}: {len(methods)} original bodies, {len(reflection)} generic reflection declarations, {len(actual)} results", flush=True)
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                failure = {"case": label, "error": str(error)}
                failures.append(failure)
                (case / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
                print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
        if {row["case"] for row in passed + failures} != set(GENERIC_CASES) or len(passed + failures) != len(GENERIC_CASES):
            raise RuntimeError("Generic acceptance omitted a required input case")
        return passed, failures

    def local_cases(self, neverd: Path) -> tuple[list[dict], list[dict]]:
        passed, failures = [], []
        directory = self.work / "original-local"
        try:
            self.run([self.javac, "-version"], "local-javac-version")
            self.run([self.java, "-version"], "local-java-version")
            source_dir = directory / "source"
            for path in sorted((FIXTURES / "local/java").rglob("*.java")):
                target = source_dir / path.relative_to(FIXTURES / "local/java")
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
            harness_source = directory / "harness-source/LocalClassHarness.java"
            harness_source.parent.mkdir(parents=True)
            shutil.copyfile(FIXTURES / "harness/LocalClassHarness.java", harness_source)
            classes_dir = directory / "classes"
            self.compile_local(sorted(source_dir.rglob("*.java")), classes_dir)
            original = class_identity.compiler_classes(classes_dir)
            (directory / "class-inventory.json").write_text(json.dumps(original, indent=2) + "\n")
            projected = class_identity.projected_methods(original)
            methods = {identity: "body" if row["code"] else "declaration"
                       for facts in original.values() for identity, row in facts["methods"].items()}
            if len(methods) != 10 or len(projected) != 9:
                raise RuntimeError("Owned local fixture must retain ten original bodies and nine projection roles")
            partitions = local_partitions(original)
            manifest = directory / "reflection.tsv"
            reflection_manifest(original, manifest)
            self.compile_local([harness_source], directory / "harness", classpath=classes_dir)
            cp = os.pathsep.join(map(str, (directory / "harness", classes_dir)))
            baseline = results(self.run([self.java, "-cp", cp, "LocalClassHarness", manifest], "original-local"), "local")
            if any(baseline["reflection:" + role] != 1 for role in LOCAL_ROLES):
                raise RuntimeError("Original reflection checks did not succeed")
            if [baseline["final-first"], baseline["final-second"], baseline["final-wide"]] != [49, 49, 7]:
                raise RuntimeError("Original fixture constructor side effects changed")
            if baseline["constant-reflection"] != 7:
                raise RuntimeError("Original fixture reflective constant value changed")
            (directory / "baseline.json").write_text(json.dumps(baseline, indent=2) + "\n")
            (directory / "source-hashes.json").write_text(json.dumps({"implementation": source_hashes(source_dir),
                "harness": source_hashes(harness_source.parent)}, indent=2) + "\n")
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            for label in LOCAL_CASES:
                failures.append({"case": label, "stage": "original-preparation", "error": str(error)})
            (self.work / "local-preparation-failure.json").write_text(json.dumps(failures, indent=2) + "\n")
            return passed, failures
        for label in LOCAL_CASES:
            case = self.work / label
            case.mkdir()
            try:
                inputs, dexes = {}, []
                for number, owners in enumerate(partitions[label], 1):
                    dex_name = "classes.dex" if number == 1 else f"classes{number}.dex"
                    paths = [classes_dir / original[owner]["path"] for owner in owners]
                    dexes.append(self.dex(paths, case / ("partition-" + str(number)), classes_dir))
                    inputs.update({owner: dex_name for owner in owners})
                if len(dexes) == 1:
                    source = dexes[0]
                else:
                    source = case / "input.apk"
                    with zipfile.ZipFile(source, "w") as archive:
                        for number, dex in enumerate(dexes, 1):
                            archive.write(dex, "classes.dex" if number == 1 else f"classes{number}.dex")
                (case / "input-inventory.json").write_text(json.dumps({"classes": original, "inputs": inputs,
                    "projected_methods": sorted(projected), "partitions": partitions[label],
                    "input_sha256": hashlib.sha256(source.read_bytes()).hexdigest()}, indent=2) + "\n")
                output = case / "recovered"
                self.run([neverd, "mobile", source, "-o", output, "--timeout", self.timeout, "--json"], label)
                report = json.loads((output / "report.json").read_text())
                if report.get("dex_count") != len(dexes) or report.get("smali_count") != 0:
                    raise RuntimeError("Local DEX partition inventory changed")
                counts = validate_coverage(report, output, set(original), methods, inputs, expected_projection=projected)
                coverage = report["android_method_recovery"]
                class_identity.validate_bindings(coverage, original, inputs)
                if report["java_sources"] != ["sources/fixture/LocalClassBehavior.java"]:
                    raise RuntimeError("Local source was flattened or emitted outside its enclosing unit")
                (case / "generated-source-hashes.json").write_text(json.dumps(source_hashes(output / "sources"), indent=2) + "\n")
                compiled = case / "compiled"
                self.compile_local(sorted((output / "sources").rglob("*.java")), compiled)
                rebuilt = class_identity.compiler_classes(compiled)
                (case / "rebuilt-class-inventory.json").write_text(json.dumps(rebuilt, indent=2) + "\n")
                mapping = class_identity.match_recompiled(original, rebuilt, coverage.get("generated_source_helpers"))
                (case / "class-mapping.json").write_text(json.dumps(mapping, indent=2) + "\n")
                rebuilt_manifest = case / "reflection.tsv"
                reflection_manifest(rebuilt, rebuilt_manifest)
                self.compile_local([harness_source], case / "harness", classpath=compiled)
                cp = os.pathsep.join(map(str, (case / "harness", compiled)))
                actual = results(self.run([self.java, "-cp", cp, "LocalClassHarness", rebuilt_manifest], label + "-execution"), "local")
                (case / "execution.json").write_text(json.dumps(actual, indent=2) + "\n")
                compare_local_behavior(baseline, actual)
                passed.append({"case": label, **counts, "matched_results": len(actual),
                               "acceptance_scope": "owned-local-source-projection", "native_coverage_status": "partial",
                               "binary_identity_equivalence": False,
                               "all_measured_binary_names_equal": mapping["all_measured_binary_names_equal"]})
                print(f"PASS {label}: {len(projected)} projected original methods, {len(actual)} reflection/behavior results; native coverage remains partial", flush=True)
            except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                failure = {"case": label, "error": str(error)}
                failures.append(failure)
                (case / "failure.json").write_text(json.dumps(failure, indent=2) + "\n")
                print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
        if {row["case"] for row in passed + failures} != set(LOCAL_CASES) or len(passed + failures) != len(LOCAL_CASES):
            raise RuntimeError("Local projection acceptance omitted a required partition")
        return passed, failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--d8", type=Path, help="D8 launcher/JAR, then NEVERD_D8, PATH, or installed Android SDK")
    parser.add_argument("--java-home", type=Path, help="JDK used for fixture compilation and execution")
    parser.add_argument("--neverd", type=Path, required=True, help="native C++ NeverD CLI under test")
    parser.add_argument("--work-dir", type=Path, help="new directory retaining all input, output, logs and failure evidence")
    parser.add_argument("--timeout", type=int, default=300, help="positive seconds per tool process/recovery budget")
    args = parser.parse_args()
    if args.timeout <= 0: raise RuntimeError("--timeout must be positive")
    jdk, d8 = choose_jdk(args.java_home), choose_d8(args.d8)
    neverd = args.neverd.resolve()
    if not neverd.is_file(): raise RuntimeError("--neverd must point to the built CLI")
    if args.work_dir:
        args.work_dir = args.work_dir.resolve()
        args.work_dir.mkdir(parents=True, exist_ok=False)
    context = nullcontext(str(args.work_dir)) if args.work_dir else tempfile.TemporaryDirectory(prefix="neverd-android-internal-")
    with context as directory:
        work = Path(directory)
        verify = Verify(work, jdk, d8, args.timeout)
        verify.run([*verify.d8, "--version"], "d8-version")
        original, baseline = verify.original(FIXTURES / "java", "AndroidHarness", "original", "full")
        classes, methods = compiler_inventory(original)
        (work / "original-inventory.json").write_text(json.dumps({"classes": sorted(classes), "methods": methods}, indent=2) + "\n")
        original_paths = sorted(original.rglob("*.class"))
        single_dex = verify.dex(original_paths, work / "dex", original)
        peer = original / "fixture/AndroidPeer.class"
        first = verify.dex([path for path in original_paths if path != peer], work / "multidex-first", original)
        second = verify.dex([peer], work / "multidex-second", original)
        apk = work / "multidex.apk"
        with zipfile.ZipFile(apk, "w") as archive:
            archive.write(first, "classes.dex"); archive.write(second, "classes2.dex")
            archive.writestr("assets/not-bytecode.txt", "not part of source recovery")
        _, legacy_baseline = verify.original(FIXTURES / "legacy", "LegacyHarness", "original-legacy", "legacy")
        _, single_baseline = verify.original(FIXTURES / "legacy", "SingleHarness", "original-single", "single")
        _, rounding_baseline = verify.original(FIXTURES / "rounding", "RoundingHarness", "original-rounding", "rounding")
        smali_dir = work / "smali"
        smali_dir.mkdir()
        smali_inputs = {}
        for name in ("Calculator.smali", "Calculator$Nested.smali", "Peer.smali"):
            shutil.copyfile(SMALI / name, smali_dir / name)
            smali_inputs["Lfixture/" + name[:-6] + ";"] = name
        cases = [
            ("dex", single_dex, classes, methods, {name: "classes.dex" for name in classes}, "AndroidHarness", "full", baseline, 1, 0),
            ("multidex", apk, classes, methods, {name: "classes2.dex" if name == "Lfixture/AndroidPeer;" else "classes.dex" for name in classes}, "AndroidHarness", "full", baseline, 2, 0),
            ("smali-directory", smali_dir, set(smali_inputs), SMALI_METHODS, smali_inputs, "LegacyHarness", "legacy", legacy_baseline, 0, 3),
            ("single-smali", smali_dir / "Peer.smali", {"Lfixture/Peer;"}, {key: value for key, value in SMALI_METHODS.items() if key.startswith("Lfixture/Peer;")}, {"Lfixture/Peer;": "Peer.smali"}, "SingleHarness", "single", single_baseline, 0, 1),
            ("rounding-smali", FIXTURES / "FloatRounding.smali", {"Lfixture/FloatRounding;"}, ROUNDING_METHODS, {"Lfixture/FloatRounding;": "FloatRounding.smali"}, "RoundingHarness", "rounding", rounding_baseline, 0, 1),
        ]
        failures, passed = [], []
        for name, source, expected_classes, expected_methods, inputs, harness, kind, oracle, dex_count, smali_count in cases:
            for route in ("native-cli",):
                label = name + "-" + route
                output = work / label
                try:
                    verify.run([neverd, "mobile", source, "-o", output, "--timeout", str(args.timeout), "--json"], label)
                    report = json.loads((output / "report.json").read_text())
                    if report.get("dex_count") != dex_count or report.get("smali_count") != smali_count:
                        raise RuntimeError("Input code inventory changed")
                    counts = validate_coverage(report, output, expected_classes, expected_methods, inputs)
                    matched = verify.rebuilt(output, harness, kind, oracle)
                    passed.append({"case": label, **counts, "matched_results": matched})
                    print(f"PASS {label}: {counts['recovered_method_count']} bodies, {counts['declaration_only_method_count']} declarations, {matched} behavior results", flush=True)
                except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
                    failure = {"case": label, "error": str(error)}
                    failures.append(failure)
                    (work / (label + "-failure.json")).write_text(json.dumps(failure, indent=2) + "\n")
                    print(f"FAIL {label}: {error}", file=sys.stderr, flush=True)
        local_passed, local_failures = verify.local_cases(neverd)
        passed.extend(local_passed)
        failures.extend(local_failures)
        generic_passed, generic_failures = verify.generic_cases(neverd)
        passed.extend(generic_passed)
        failures.extend(generic_failures)
        constructor_passed, constructor_failures = verify.constructor_cases(neverd)
        passed.extend(constructor_passed)
        failures.extend(constructor_failures)
        deprecated_passed, deprecated_failures = verify.deprecated_cases(neverd)
        passed.extend(deprecated_passed)
        failures.extend(deprecated_failures)
        marker_passed, marker_failures = verify.marker_cases(neverd)
        passed.extend(marker_passed)
        failures.extend(marker_failures)
        broken = work / "broken-smali"
        broken.mkdir()
        shutil.copyfile(SMALI / "Peer.smali", broken / "Peer.smali")
        (broken / "Broken.smali").write_text(".class public Lfixture/Broken;\n.super Ljava/lang/Object;\n.method public static broken()I\n.registers 1\nnot-an-opcode\n.end method\n")
        duplicate = work / "duplicate-smali"
        duplicate.mkdir()
        for name in ("Peer.smali", "PeerCopy.smali"): shutil.copyfile(SMALI / "Peer.smali", duplicate / name)
        for label, source in (("malformed", broken), ("duplicate", duplicate)):
            output = work / (label + "-output")
            result = subprocess.run([str(neverd), "mobile", str(source), "-o", str(output), "--json"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                    timeout=args.timeout, env=verify.environment)
            if result.returncode == 1 and json.loads(result.stdout).get("status") == "error":
                if output.exists(): failures.append({"case": label, "error": "Failed recovery published an output directory"})
                else: print(f"PASS {label}: rejects partial recovery", flush=True)
            else: failures.append({"case": label, "error": "Invalid input did not produce a structured recovery error"})
        summary = {"schema_version": 1, "passed": passed, "failures": failures}
        (work / "acceptance.json").write_text(json.dumps(summary, indent=2) + "\n")
        if failures: raise RuntimeError(f"{len(failures)} built-in Android acceptance cases failed; evidence: {work}")
        print(f"PASS all {len(passed)} source recovery/projection execution cases and 2 rejection cases", flush=True)
    return 0


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
