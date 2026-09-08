#!/usr/bin/env python3
"""Execute a self-owned Objective-C call corpus and inspect or verify recovery.

Requires macOS, Apple Clang, libobjc, and a built NeverD CLI. --setup-only
validates originals. --inspect-only retains actual export failures and partial
coverage as diagnostic evidence; it never counts these as verified recovery.
The default mode requires every method and rebuilt execution to match.
"""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from test_mobile_ios_backend import assert_results, chained_fixups, execution_results, run


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "scripts/tests/fixtures/mobile/ObjCCalls.m"
INPUTS = (-31, -1, 0, 1, 7, 23, 1000)
WIDE = (-(2**63), -(2**32) - 1, -1, 0, 2**32 + 1, 2**63 - 1)
METHODS = {
    ("NDCallBase", "classStep:", True),
    *(("NDCallBase", selector, False) for selector in (
        "leaf:", "setBias:", "bias", "setWide:", "wide", "exchangePeer:",
    )),
    ("NDCallChild", "classStep:", True),
    ("NDCallChild", "selfClassSend:", True),
    *(("NDCallChild", selector, False) for selector in (
        "leaf:", "selfSend:", "objectSend:value:", "classSend:", "superSend:",
        "cHelper:", "adjustExtra:", "extra", "blockApply:value:",
        "capturedBlock:", "globalBlock:", "categoryStep:",
    )),
}
LAYOUT_KEYS = {
    "layout-base-size:0", "layout-child-size:0", "layout-bias:0",
    "layout-wide:0", "layout-peer:0", "layout-extra:0",
}

HARNESS = r"""
#include "fixture.h"
#include <objc/runtime.h>
#include <limits.h>
#include <stdio.h>
static void emit(const char *name, unsigned index, long long value) {
    printf("%s:%u=%lld\n", name, index, value);
}
int main(void) {
    Class baseClass = objc_getClass("NDCallBase");
    Class childClass = objc_getClass("NDCallChild");
    if (!baseClass || !childClass) return 80;
    NDCallBase *base = (NDCallBase *)class_createInstance(baseClass, 0);
    NDCallChild *child = (NDCallChild *)class_createInstance(childClass, 0);
    if (!base || !child) return 81;
    Ivar bias = class_getInstanceVariable(baseClass, "_bias");
    Ivar wide = class_getInstanceVariable(baseClass, "_wide");
    Ivar peer = class_getInstanceVariable(baseClass, "_peer");
    Ivar extra = class_getInstanceVariable(childClass, "_extra");
    if (!bias || !wide || !peer || !extra) return 82;
    emit("layout-base-size", 0, class_getInstanceSize(baseClass));
    emit("layout-child-size", 0, class_getInstanceSize(childClass));
    emit("layout-bias", 0, ivar_getOffset(bias));
    emit("layout-wide", 0, ivar_getOffset(wide));
    emit("layout-peer", 0, ivar_getOffset(peer));
    emit("layout-extra", 0, ivar_getOffset(extra));
    [base setBias:5];
    [child setBias:-3];
    emit("base-bias", 0, [base bias]);
    emit("child-bias", 0, [child bias]);
    emit("extra-old", 0, [child adjustExtra:11]);
    emit("extra-new", 0, [child extra]);
    const int inputs[] = {-31, -1, 0, 1, 7, 23, 1000};
    NDUnaryBlock external = ^(int value) { return value * 2 + 1; };
    for (unsigned index = 0; index < sizeof(inputs) / sizeof(inputs[0]); ++index) {
        int value = inputs[index];
        emit("base-leaf", index, [base leaf:value]);
        emit("child-leaf", index, [child leaf:value]);
        emit("self-send", index, [child selfSend:value]);
        emit("object-base", index, [child objectSend:base value:value]);
        emit("object-child", index, [child objectSend:child value:value]);
        emit("object-nil", index, [child objectSend:nil value:value]);
        emit("class-base", index, [NDCallBase classStep:value]);
        emit("class-child", index, [NDCallChild classStep:value]);
        emit("class-self", index, [NDCallChild selfClassSend:value]);
        emit("class-send", index, [child classSend:value]);
        emit("super-send", index, [child superSend:value]);
        emit("c-helper", index, [child cHelper:value]);
        emit("category", index, [child categoryStep:value]);
        emit("block-external", index, [child blockApply:external value:value]);
        emit("block-capture", index, [child capturedBlock:value]);
        emit("block-global", index, [child globalBlock:value]);
    }
    const long long values[] = {LLONG_MIN, -4294967297LL, -1, 0, 4294967297LL, LLONG_MAX};
    for (unsigned index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        [child setWide:values[index]];
        emit("wide", index, [child wide]);
    }
    emit("peer-empty", 0, [child exchangePeer:base] == nil);
    emit("peer-old", 0, [child exchangePeer:child] == base);
    emit("peer-self", 0, [child exchangePeer:nil] == child);
    emit("extra-change", 0, [child adjustExtra:-7]);
    emit("extra-last", 0, [child extra]);
    emit("leaf-after-change", 0, [child leaf:4]);
    object_dispose(child);
    object_dispose(base);
    return 0;
}
"""


def expected_results() -> dict[str, int]:
    expected = {
        "base-bias:0": 5, "child-bias:0": -3, "extra-old:0": 0,
        "extra-new:0": 11, "peer-empty:0": 1, "peer-old:0": 1,
        "peer-self:0": 1, "extra-change:0": 11, "extra-last:0": -7,
        "leaf-after-change:0": -6,
    }
    for index, value in enumerate(INPUTS):
        for name, result in {
            "base-leaf": value + 5, "child-leaf": value + 8,
            "self-send": value + 9, "object-base": value + 3,
            "object-child": value + 6, "object-nil": -2,
            "class-base": value + 13, "class-child": value + 23,
            "class-self": value + 32, "class-send": value + 20,
            "super-send": value + 8, "c-helper": value * 3 - 11,
            "category": value + 37, "block-external": value * 2 + 5,
            "block-capture": value + 13, "block-global": value - 3,
        }.items():
            expected[f"{name}:{index}"] = result
    expected.update((f"wide:{index}", value) for index, value in enumerate(WIDE))
    return expected


def inspect_command(argv: list[str], output: Path, timeout: int) -> dict:
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        output.with_suffix(".stdout.txt").write_text(result.stdout)
        output.with_suffix(".stderr.txt").write_text(result.stderr)
        return {"argv": argv, "returncode": result.returncode, "timed_out": False}
    except subprocess.TimeoutExpired as error:
        output.with_suffix(".stdout.txt").write_bytes(error.stdout or b"")
        output.with_suffix(".stderr.txt").write_bytes(error.stderr or b"")
        return {"argv": argv, "returncode": None, "timed_out": True}


def inspect_binary(neverd: Path, original: Path, variant: Path, timeout: int) -> dict:
    evidence = variant / "inspection"
    evidence.mkdir()
    commands = []
    for name in ("funcs", "imports", "strings", "objc-methods"):
        target = evidence / f"{name}.json"
        commands.append(inspect_command(
            [str(neverd), "export", str(original), f"--format={name}", "-o", str(target)],
            evidence / name, timeout,
        ))
    # The CLI selects one dump level per invocation; combining these flags
    # would silently capture LowIR alone.
    for level in ("low", "med", "high"):
        commands.append(inspect_command(
            [str(neverd), "lift", str(original), f"--dump-{level}",
             "-o", str(evidence / f"native-{level}.ll")], evidence / level, timeout,
        ))
    summary = {"commands": commands, "expected_method_count": len(METHODS)}
    batch = evidence / "objc-methods.json"
    if batch.is_file() and batch.stat().st_size:
        data = json.loads(batch.read_text())
        coverage = {key: data.get(key) for key in (
            "schema_version", "status", "method_count", "recovered_method_count", "limitations",
        )}
        coverage["methods"] = [{key: value for key, value in item.items() if key != "source"}
                               for item in data.get("methods", [])]
        summary["coverage"] = coverage
        (evidence / "native.c").write_text(data.get("native_source", ""))
        (evidence / "objc-metadata.json").write_text(
            json.dumps(data.get("objc_metadata", {}), indent=2) + "\n")
        actual = {(item.get("class_name"), item.get("selector"), item.get("class_method"))
                  for item in coverage.get("methods", [])}
        summary["missing_methods"] = sorted(METHODS - actual)
        summary["extra_methods"] = sorted(actual - METHODS)
    (evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    return summary


def verify(arguments: argparse.Namespace, work: Path) -> None:
    clang = shutil.which("clang")
    if sys.platform != "darwin" or not clang:
        raise RuntimeError("This execution corpus requires macOS, Apple Clang, and its SDK")
    if not arguments.setup_only and not arguments.neverd:
        raise RuntimeError("Pass --neverd PATH or use --setup-only")
    declarations, marker, _ = FIXTURE.read_text().partition("// NEVERD_CALLS_IMPLEMENTATION")
    if not marker:
        raise RuntimeError("The self-owned fixture declaration marker is missing")
    (work / "fixture.h").write_text(declarations)
    harness = work / "harness.m"
    harness.write_text(HARNESS)
    architectures = ("arm64", "x86_64") if arguments.arch == "all" else (arguments.arch,)
    fixups = ("classic", "default") if arguments.fixups == "both" else (arguments.fixups,)
    completed = 0
    for architecture in architectures:
        for fixup in fixups:
            label = f"{architecture}-{fixup}"
            variant = work / label
            variant.mkdir()
            original = variant / "original"
            flags = [clang, "-arch", architecture, "-O1", "-g0", "-fno-objc-arc",
                     "-fblocks", "-fno-vectorize", "-fno-slp-vectorize", "-fno-unroll-loops",
                     "-Werror=return-type", "-Wl,-no_objc_category_merging", "-lobjc"]
            if fixup == "classic":
                flags.append("-Wl,-no_fixup_chains")
            run([*flags, str(FIXTURE), str(harness), "-o", str(original)])
            baseline = execution_results(original)
            layout = {key: value for key, value in baseline.items() if key.startswith("layout-")}
            if layout.keys() != LAYOUT_KEYS or any(value <= 0 for value in layout.values()):
                raise RuntimeError(f"Incomplete runtime layout evidence: {layout!r}")
            assert_results({key: value for key, value in baseline.items() if key not in LAYOUT_KEYS},
                           expected_results(), f"Original {label}")
            chained = chained_fixups(original)
            if fixup == "classic" and chained:
                raise RuntimeError("Classic fixture unexpectedly has chained fixups")
            (variant / "original-results.json").write_text(json.dumps(baseline, indent=2) + "\n")
            print(f"PASS original {label}: {len(baseline)} execution results; chained_fixups={chained}",
                  flush=True)
            if arguments.inspect_only:
                summary = inspect_binary(arguments.neverd.resolve(), original, variant, arguments.timeout)
                coverage = summary.get("coverage", {})
                print(f"INSPECT {label}: {coverage.get('recovered_method_count', 'unknown')} recovered; "
                      f"{len(summary.get('missing_methods', []))} missing method identities; "
                      f"{sum(item['returncode'] != 0 for item in summary['commands'])} failed exports",
                      flush=True)
            elif not arguments.setup_only:
                output = variant / "recovered"
                run([str(arguments.neverd.resolve()), "mobile", str(original), "-o", str(output),
                     "--platform=ios", f"--arch={architecture}", "--python", sys.executable,
                     f"--timeout={arguments.timeout}"], timeout=arguments.timeout + 30)
                report = json.loads((output / "report.json").read_text())
                if (report.get("status"), report.get("platform"), report.get("architecture")) != (
                        "success", "ios", architecture):
                    raise RuntimeError("Mobile report did not identify this recovered iOS architecture")
                coverage = report.get("objc_method_recovery", {})
                recorded = json.loads((output / "metadata/objc-methods.json").read_text())
                methods = coverage.get("methods", [])
                actual = {(item.get("class_name"), item.get("selector"), item.get("class_method"))
                          for item in methods}
                if (coverage != recorded or actual != METHODS or len(methods) != len(METHODS) or
                        coverage.get("schema_version") != 1 or
                        coverage.get("method_count") != len(METHODS) or
                        coverage.get("recovered_method_count") != len(METHODS) or
                        coverage.get("unrecovered_method_count") != 0 or
                        coverage.get("status") != "recovered" or
                        any(item.get("status") != "recovered" for item in methods)):
                    raise RuntimeError(f"Incomplete call-corpus method recovery: {coverage!r}")
                source = output / "sources/objc.m"
                if not source.is_file():
                    raise RuntimeError("No recovered sources/objc.m")
                rebuilt = variant / "rebuilt"
                run([*flags, str(source), str(harness), "-o", str(rebuilt)])
                assert_results(execution_results(rebuilt), baseline, f"Recovered {label}")
                print(f"PASS recovered {label}: {len(METHODS)} methods, "
                      f"{len(baseline)} matching execution results", flush=True)
            completed += 1
    description = "original" if arguments.setup_only or arguments.inspect_only else "recovered"
    print(f"Verified {completed} {description} variants; skipped architectures: none")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, help="built NeverD CLI")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--setup-only", action="store_true")
    modes.add_argument("--inspect-only", action="store_true")
    parser.add_argument("--arch", choices=("all", "arm64", "x86_64"), default="all")
    parser.add_argument("--fixups", choices=("both", "classic", "default"), default="both")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--work-dir", type=Path, help="new directory to retain all evidence")
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if arguments.work_dir:
        work = arguments.work_dir.resolve()
        work.mkdir(parents=True, exist_ok=False)
        context = nullcontext(str(work))
    else:
        context = tempfile.TemporaryDirectory(prefix="neverd-ios-calls-")
    with context as temporary:
        verify(arguments, Path(temporary))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
