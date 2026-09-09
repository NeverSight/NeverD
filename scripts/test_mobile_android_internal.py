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
except ModuleNotFoundError as error:
    if error.name != "scripts":
        raise
    import mobile_android_class_identity as class_identity

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
                      *, expected_projection: set[str] | None = None) -> dict:
    projected = set() if expected_projection is None else expected_projection
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
    if not projected and (coverage.get("projected_method_count", 0) != 0
                          or coverage.get("class_source_bindings") or coverage.get("generated_source_helpers")):
        raise RuntimeError("Ordinary recovery cannot conceal local source projection")
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

    def dex(self, classes, output: Path, classpath: Path):
        output.mkdir(parents=True)
        self.run([*self.d8, "--debug", "--classpath", classpath, "--output", output, *classes], "d8")
        dex = output / "classes.dex"
        if not dex.is_file() or list(output.glob("classes*.dex")) != [dex]:
            raise RuntimeError("Fixture partition did not produce exactly one DEX")
        return dex

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
