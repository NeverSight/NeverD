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
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

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


class ClassFile:
    """Read the independent compiler's declaration/Code attribute inventory."""
    def __init__(self, data: bytes):
        self.data, self.offset = data, 0

    def take(self, count: int) -> bytes:
        if count < 0 or self.offset + count > len(self.data):
            raise RuntimeError("Truncated compiler class file")
        result = self.data[self.offset:self.offset + count]
        self.offset += count
        return result

    def number(self, count: int) -> int: return int.from_bytes(self.take(count), "big")

    def inventory(self) -> tuple[str, dict[str, str]]:
        if self.take(4) != b"\xca\xfe\xba\xbe": raise RuntimeError("Invalid compiler class file")
        self.take(4)  # minor and major versions
        count = self.number(2)
        constants, index = {}, 1
        while index < count:
            tag = self.number(1)
            if tag == 1:
                raw = self.take(self.number(2))
                value = raw.replace(b"\xc0\x80", b"\0").decode("utf-8", errors="surrogatepass")
            elif tag in (7, 8, 16, 19, 20): value = self.number(2)
            elif tag in (3, 4, 9, 10, 11, 12, 17, 18): value = self.take(4)
            elif tag in (5, 6): value = self.take(8)
            elif tag == 15: value = self.take(3)
            else: raise RuntimeError(f"Unsupported compiler constant-pool tag {tag}")
            constants[index] = tag, value
            index += 2 if tag in (5, 6) else 1

        def constant(index: int, tag: int):
            entry = constants.get(index)
            if entry is None or entry[0] != tag: raise RuntimeError("Invalid compiler constant reference")
            return entry[1]

        self.number(2)
        owner = "L" + constant(constant(self.number(2), 7), 1) + ";"
        self.number(2)
        self.take(self.number(2) * 2)

        def attributes():
            names = []
            for _ in range(self.number(2)):
                names.append(constant(self.number(2), 1))
                self.take(self.number(4))
            return names

        for _ in range(self.number(2)):
            self.take(6); attributes()
        methods = {}
        for _ in range(self.number(2)):
            flags, name, prototype = self.number(2), constant(self.number(2), 1), constant(self.number(2), 1)
            attrs = attributes()
            code = attrs.count("Code")
            declaration_only = bool(flags & (0x100 | 0x400))
            if code != (0 if declaration_only else 1):
                raise RuntimeError("Compiler method has inconsistent Code/access metadata")
            identity = owner + "->" + name + prototype
            if identity in methods: raise RuntimeError("Compiler emitted a duplicate method")
            methods[identity] = "declaration" if declaration_only else "body"
        attributes()
        if self.offset != len(self.data): raise RuntimeError("Compiler class file has unparsed trailing data")
        return owner, methods


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


def validate_coverage(report: dict, output: Path, classes: set[str], expected: dict[str, str], inputs: dict[str, str]) -> dict:
    if report.get("status") != "success" or report.get("platform") != "android":
        raise RuntimeError("Recovery report does not identify a successful Android export")
    if report.get("backend") != {"name": "neverd", "version": "1", "execution": "builtin"}:
        raise RuntimeError("The default path did not use the built-in engine")
    coverage = json.loads((output / "metadata/android-methods.json").read_text())
    if report.get("android_method_recovery") != coverage or coverage.get("status") != "recovered" or coverage.get("schema_version") != 1:
        raise RuntimeError("Standalone method coverage disagrees with the successful report")
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
        wanted = "declaration-only" if expected[identity] == "declaration" else "recovered"
        if row.get("status") != wanted or (count != 0) != (wanted == "recovered"):
            raise RuntimeError(f"Method body/declaration classification changed: {identity}: {row}")
        if wanted == "declaration-only" and not row.get("reason"): raise RuntimeError("Declaration-only method lacks its reason")
        actual[identity] = row
    if set(actual) != set(expected): raise RuntimeError(f"Missing methods: {sorted(expected.keys() - actual.keys())}")
    recovered = sum(kind == "body" for kind in expected.values())
    counts = {"class_count": len(classes), "method_count": len(expected), "recovered_method_count": recovered,
              "declaration_only_method_count": len(expected) - recovered, "unrecovered_method_count": 0}
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


class Verify:
    def __init__(self, work: Path, jdk: Path, d8: Path, timeout: int):
        self.work, self.timeout = work, timeout
        self.environment = os.environ.copy()
        self.environment["JAVA_HOME"] = str(jdk)
        self.environment["PATH"] = str(jdk / "bin") + os.pathsep + self.environment.get("PATH", "")
        self.environment["NEVERD_JADX"] = str(work / "external-decompiler-must-not-run")
        suffix = ".exe" if os.name == "nt" else ""
        self.java, self.javac = str(jdk / "bin" / ("java" + suffix)), str(jdk / "bin" / ("javac" + suffix))
        self.d8 = d8_command(d8, self.java)
        self.log_index = 0
        (work / "logs").mkdir()

    def run(self, arguments, label: str) -> str:
        self.log_index += 1
        log = self.work / "logs" / f"{self.log_index:03d}-{label}.log"
        with log.open("x", encoding="utf-8") as stream:
            result = subprocess.run(list(map(str, arguments)), stdout=stream,
                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                    timeout=self.timeout, env=self.environment, check=False)
        if result.returncode:
            raise RuntimeError(f"{label} exited {result.returncode}: {log.read_text(errors='replace')}")
        return log.read_text(encoding="utf-8", errors="replace")

    def compile(self, sources, output: Path, *, classpath: Path | None = None):
        output.mkdir(parents=True)
        arguments = [self.javac, "-encoding", "UTF-8", "-g", "-d", output]
        if classpath: arguments += ["-classpath", classpath]
        self.run([*arguments, *sources], "javac")

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
        print(f"PASS all {len(passed)} source recovery/execution cases and 2 rejection cases", flush=True)
    return 0


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
