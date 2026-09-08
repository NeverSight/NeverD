#!/usr/bin/env python3
"""Compile, recover, rebuild, and execute a self-owned Objective-C fixture.

Requires macOS, Apple Clang with its SDK, and a built NeverD CLI. No dependencies
are downloaded. Both arm64 and x86_64 are attempted by default; an architecture
the host cannot execute is explicitly skipped, never counted as verified.

Example: python3 scripts/test_mobile_ios_backend.py --neverd build/bin/neverd
Use --setup-only to verify compilation and original execution before recovery.
"""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import errno
import json
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "scripts/tests/fixtures/mobile/ObjCBehavior.m"
CLASS_NAME = "NeverDObjCFixture"
METHODS = {
    ("classAnswer", True),
    *((selector, False) for selector in (
        "constant42", "echo:", "unsignedEcho:", "wideEcho:", "add:right:",
        "subtract:right:", "choose:", "sum:count:", "read:", "write:value:",
        "combine:ignored:last:", "selfValue", "commandValue",
    )),
}
SCALARS = (-(2**31), -(2**31) + 1, -32769, -1, 0, 1, 32768, 2**31 - 1)
UNSIGNED = (0, 1, 32768, 2**31 - 1, 2**31, 2**32 - 1)
WIDE = (-(2**63), -(2**63) + 1, -(2**32), -1, 0, 2**32, 2**63 - 1)
PAIRS = ((-(2**31), 0), (2**31 - 1, 0), (-100, 7), (0, 0), (1234, -987), (5, 37))
BRANCHES = (-(2**31), -100, -1, 0, 1, 7, 8, 2**31 - 1)
ARRAY = (-5, 7, 0, 9, -3)


HARNESS = r"""
#include "fixture.h"
#include <objc/runtime.h>
#include <limits.h>
#include <stdio.h>

static void emit(const char *name, unsigned index, long long value) {
    printf("%s:%u=%lld\n", name, index, value);
}

int main(void) {
    Class klass = objc_getClass("NeverDObjCFixture");
    if (!klass) return 80;
    NeverDObjCFixture *object = (NeverDObjCFixture *)class_createInstance(klass, 0);
    if (!object) return 81;
    const int scalars[] = {INT_MIN, INT_MIN + 1, -32769, -1, 0, 1, 32768, INT_MAX};
    const unsigned int unsignedValues[] = {0, 1, 32768, INT_MAX, 2147483648U, UINT_MAX};
    const long long wide[] = {LLONG_MIN, LLONG_MIN + 1, -4294967296LL, -1, 0,
                             4294967296LL, LLONG_MAX};
    const int pairs[][2] = {{INT_MIN, 0}, {INT_MAX, 0}, {-100, 7}, {0, 0},
                           {1234, -987}, {5, 37}};
    const int branches[] = {INT_MIN, -100, -1, 0, 1, 7, 8, INT_MAX};
    const int values[] = {-5, 7, 0, 9, -3};
    emit("classAnswer", 0, [NeverDObjCFixture classAnswer]);
    emit("constant42", 0, [object constant42]);
    for (unsigned index = 0; index < sizeof(scalars) / sizeof(scalars[0]); ++index) {
        int cell = scalars[index];
        int replacement = scalars[7 - index];
        emit("echo", index, [object echo:cell]);
        emit("read", index, [object read:&cell]);
        emit("write-return", index, [object write:&cell value:replacement]);
        emit("write-memory", index, cell);
        emit("combine", index, [object combine:37 ignored:scalars[index] last:-5]);
    }
    for (unsigned index = 0; index < sizeof(unsignedValues) / sizeof(unsignedValues[0]); ++index)
        emit("unsignedEcho", index, [object unsignedEcho:unsignedValues[index]]);
    for (unsigned index = 0; index < sizeof(wide) / sizeof(wide[0]); ++index)
        emit("wideEcho", index, [object wideEcho:wide[index]]);
    for (unsigned index = 0; index < sizeof(pairs) / sizeof(pairs[0]); ++index) {
        emit("add", index, [object add:pairs[index][0] right:pairs[index][1]]);
        emit("subtract", index, [object subtract:pairs[index][0] right:pairs[index][1]]);
    }
    for (unsigned index = 0; index < sizeof(branches) / sizeof(branches[0]); ++index)
        emit("choose", index, [object choose:branches[index]]);
    for (int count = -1; count <= 5; ++count)
        emit("sum", (unsigned)(count + 1), [object sum:values count:count]);
    emit("sum-null", 0, [object sum:0 count:0]);
    emit("selfValue", 0, [object selfValue] == object);
    emit("commandValue", 0, [object commandValue] == @selector(commandValue));
    object_dispose(object);
    return 0;
}
"""


def run(argv: list[str], *, timeout: int = 120) -> str:
    completed = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    if completed.returncode:
        raise RuntimeError(
            f"{Path(argv[0]).name} exited {completed.returncode}:\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    return completed.stdout


def expected_results() -> dict[str, int]:
    result = {"classAnswer:0": 42, "constant42:0": 42,
              "selfValue:0": 1, "commandValue:0": 1, "sum-null:0": 0}
    for index, value in enumerate(SCALARS):
        for name in ("echo", "read", "write-return"):
            result[f"{name}:{index}"] = value
        result[f"write-memory:{index}"] = SCALARS[7 - index]
        result[f"combine:{index}"] = 42
    for name, values in (("unsignedEcho", UNSIGNED), ("wideEcho", WIDE)):
        result.update((f"{name}:{index}", value) for index, value in enumerate(values))
    for index, (left, right) in enumerate(PAIRS):
        result[f"add:{index}"] = left + right
        result[f"subtract:{index}"] = left - right
    for index, value in enumerate(BRANCHES):
        result[f"choose:{index}"] = value + 11 if value < 0 else value - 3 if value > 7 else value + 5
    for count in range(-1, 6):
        result[f"sum:{count + 1}"] = sum(ARRAY[:max(count, 0)])
    return result


def execution_results(binary: Path) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in run([str(binary)], timeout=30).splitlines():
        name, separator, value = line.partition("=")
        if not separator or name in result:
            raise RuntimeError(f"Unexpected or duplicate fixture output: {line!r}")
        result[name] = int(value)
    return result


def assert_results(actual: dict[str, int], expected: dict[str, int], label: str) -> None:
    if actual != expected:
        differences = [
            f"{key}: expected {expected.get(key)!r}, got {actual.get(key)!r}"
            for key in sorted(actual.keys() | expected.keys())
            if actual.get(key) != expected.get(key)
        ]
        raise RuntimeError(f"{label} changed fixture behavior:\n" + "\n".join(differences))


def chained_fixups(binary: Path) -> bool:
    data = binary.read_bytes()
    if len(data) < 32 or struct.unpack_from("<I", data)[0] != 0xFEEDFACF:
        raise RuntimeError("Clang did not produce the expected thin 64-bit Mach-O fixture")
    offset = 32
    found = False
    for _ in range(struct.unpack_from("<I", data, 16)[0]):
        command, size = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > len(data):
            raise RuntimeError("Clang produced a malformed Mach-O load command")
        found |= command == 0x80000034
        offset += size
    return found


def validate_coverage(output: Path, architecture: str) -> dict:
    report = json.loads((output / "report.json").read_text())
    if (report.get("status"), report.get("platform"), report.get("architecture")) != (
            "success", "ios", architecture):
        raise RuntimeError("Mobile report did not identify successful recovery of the selected architecture")
    coverage = report.get("objc_method_recovery", {})
    if coverage != json.loads((output / "metadata/objc-methods.json").read_text()):
        raise RuntimeError("Standalone Objective-C method coverage differs from report.json")
    methods = coverage.get("methods", [])
    expected = {(CLASS_NAME, selector, class_method) for selector, class_method in METHODS}
    actual = {(method.get("class_name"), method.get("selector"), method.get("class_method"))
              for method in methods}
    if actual != expected or len(methods) != len(expected):
        raise RuntimeError(f"Method coverage omitted or duplicated fixture methods: {actual ^ expected!r}")
    if (coverage.get("schema_version"), coverage.get("status"), coverage.get("method_count"),
            coverage.get("recovered_method_count"), coverage.get("unrecovered_method_count")) != (
            1, "recovered", len(METHODS), len(METHODS), 0):
        raise RuntimeError(f"Incomplete Objective-C method coverage: {coverage!r}")
    if any(method.get("status") != "recovered" for method in methods):
        raise RuntimeError("A fixture method was not recovered despite the aggregate coverage")
    return coverage


def verify(arguments: argparse.Namespace, work: Path) -> None:
    clang = shutil.which("clang")
    if sys.platform != "darwin" or not clang:
        raise RuntimeError("This execution test requires macOS, Apple Clang, and its SDK")
    if not arguments.setup_only and not arguments.neverd:
        raise RuntimeError("Pass --neverd PATH, or use --setup-only for original-fixture validation")
    fixture = FIXTURE.read_text()
    declarations, marker, _ = fixture.partition("@implementation")
    if not marker:
        raise RuntimeError("The self-owned fixture has no Objective-C implementation")
    # The harness gets declarations only. Recovered builds never link FIXTURE.
    (work / "fixture.h").write_text(declarations)
    harness = work / "harness.m"
    harness.write_text(HARNESS)
    architectures = ("arm64", "x86_64") if arguments.arch == "all" else (arguments.arch,)
    fixups = ("classic", "default") if arguments.fixups == "both" else (arguments.fixups,)
    host = platform.machine()
    expected = expected_results()
    completed = 0
    skipped: list[str] = []
    for architecture in architectures:
        for fixup in fixups:
            label = f"{architecture}-{fixup}"
            variant = work / label
            variant.mkdir()
            original = variant / "original"
            flags = [clang, "-arch", architecture, "-O1", "-g0", "-fno-objc-arc",
                     "-fno-vectorize", "-fno-slp-vectorize", "-fno-unroll-loops",
                     "-Werror=return-type", "-lobjc"]
            if fixup == "classic":
                flags.append("-Wl,-no_fixup_chains")
            run([*flags, str(FIXTURE), str(harness), "-o", str(original)])
            chained = chained_fixups(original)
            if fixup == "classic" and chained:
                raise RuntimeError("The classic-fixup fixture unexpectedly uses chained fixups")
            try:
                baseline = execution_results(original)
            except OSError as exc:
                if (arguments.arch == "all" and architecture != host and
                        exc.errno in (errno.ENOEXEC, 86)):
                    print(f"SKIP {label}: host cannot execute {architecture} ({exc.strerror})")
                    skipped.append(label)
                    break
                raise
            assert_results(baseline, expected, f"Original {label}")
            print(f"PASS original {label}: {len(baseline)} execution results; chained_fixups={chained}", flush=True)
            if not arguments.setup_only:
                output = variant / "recovered"
                run([str(arguments.neverd.resolve()), "mobile", str(original), "-o", str(output),
                     "--platform=ios", f"--arch={architecture}", "--python", sys.executable,
                     f"--timeout={arguments.timeout}"], timeout=arguments.timeout + 30)
                validate_coverage(output, architecture)
                recovered = output / "sources/objc.m"
                if not recovered.is_file():
                    raise RuntimeError("Mobile recovery produced no sources/objc.m")
                rebuilt = variant / "rebuilt"
                run([*flags, str(recovered), str(harness), "-o", str(rebuilt)])
                assert_results(execution_results(rebuilt), baseline, f"Recovered {label}")
                print(f"PASS recovered {label}: {len(METHODS)} methods, {len(baseline)} matching execution results", flush=True)
            completed += 1
    if not completed:
        raise RuntimeError("No fixture architecture was executed")
    print(f"Verified {completed} {'original' if arguments.setup_only else 'recovered'} variants; "
          f"skipped architectures: {', '.join(skipped) or 'none'}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, help="built NeverD CLI")
    parser.add_argument("--setup-only", action="store_true", help="compile and execute originals only")
    parser.add_argument("--arch", choices=("all", "arm64", "x86_64"), default="all")
    parser.add_argument("--fixups", choices=("both", "classic", "default"), default="both")
    parser.add_argument("--timeout", type=int, default=300, help="seconds per mobile recovery")
    parser.add_argument("--work-dir", type=Path, help="new directory to retain binaries, sources, and reports")
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if arguments.work_dir:
        work = arguments.work_dir.resolve()
        work.mkdir(parents=True, exist_ok=False)
        context = nullcontext(str(work))
    else:
        context = tempfile.TemporaryDirectory(prefix="neverd-ios-execution-")
    with context as temporary:
        verify(arguments, Path(temporary))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
