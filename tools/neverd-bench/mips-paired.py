#!/usr/bin/env python3
"""Alternating same-base MIPS measurements; raw results stay in an ignored build.

Both bin directories must contain their own CLI/SDK dependency closure and the
same benchmark harness. Repeats/samples are explicit. Timing never substitutes
for output identity, semantic tests, or equal instruction coverage.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import statistics
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("baseline", "candidate", "code", "firmware", "output",
                 "baseline_inputs", "candidate_inputs", "baseline_toolchain",
                 "candidate_toolchain"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    parser.add_argument("--compare-capstone", action="store_true",
                        help="allow the decoder revision to be the varied build input")
    parser.add_argument("--base-commit", required=True)
    parser.add_argument("--samples", type=int, required=True)
    parser.add_argument("--repeats", type=int, required=True)
    profile_text = Path(__file__).with_name("mips-bench.def").read_text()
    parser.add_argument(
        "--profile", action="append",
        choices=re.findall(r"^NEVERD_[A-Z0-9_]+_BENCH_PROFILE\((\w+),", profile_text, re.M),
        help="repeat for selected shared profiles; defaults to the four classic profiles")
    args = parser.parse_args()
    if not hasattr(os, "wait4") or args.samples <= 0 or args.repeats <= 0:
        parser.error("requires POSIX wait4 and positive sample/repetition counts")
    if args.baseline_inputs.read_bytes() != args.candidate_inputs.read_bytes():
        parser.error("dependency snapshots differ; rebuild a matching baseline")
    toolchain_a = json.loads(args.baseline_toolchain.read_text())
    toolchain_b = json.loads(args.candidate_toolchain.read_text())
    if not {"compiler", "llvm", "capstone", "build"} <= toolchain_a.keys():
        parser.error("toolchain manifests must identify compiler, LLVM, Capstone and build flags/allocator")
    def comparable(toolchain):
        return {k: v for k, v in toolchain.items() if k != "capstone" or not args.compare_capstone}
    if "capstone" not in toolchain_b or comparable(toolchain_a) != comparable(toolchain_b):
        parser.error("toolchain/flags/allocator snapshots differ; rebuild a matching baseline")
    inputs = (args.baseline, args.candidate, args.code, args.firmware,
              args.baseline_inputs, args.candidate_inputs,
              args.baseline_toolchain, args.candidate_toolchain)
    with MIPSValidationArtifacts(args.output, "mips-paired", inputs) as artifacts:
        run(args, artifacts, toolchain_a, toolchain_b)


def run(args, artifacts, toolchain_a, toolchain_b):
    artifacts.write("samples.json", [])
    artifacts.write("summary.json", {"status": "running"})
    profile_text = Path(__file__).with_name("mips-bench.def").read_text()
    profiles = args.profile or re.findall(r"^NEVERD_MIPS_BENCH_PROFILE\((\w+),", profile_text, re.M)
    stages = re.findall(r'^NEVERD_MIPS_BENCH_STAGE\(\w+, "(\w+)"\)', profile_text, re.M)
    binaries = {"A": args.baseline.resolve(), "B": args.candidate.resolve()}
    jobs = []
    for profile in profiles:
        code = args.code.resolve() / (profile + "-unchanged.bin")
        for stage in stages:
            jobs.append((f"{profile}-{stage}", "1", "primitive",
                         [profile, stage, str(code), str(args.repeats)]))
    for threads in ("1", "default"):
        for backend in ("highc", "llvmc"):
            for selection, flags in (("call", ["--func", "0x4012d0"]),
                                     ("discovered", ["--max-func=250"])):
                jobs.append((f"ubus-{selection}-{backend}-{threads}", threads,
                             "request", ["decompile", *(["--llvm"] if backend == "llvmc" else []),
                                         *flags, str(args.firmware.resolve())]))
    records = []
    for name, threads, kind, tail in jobs:
        first_output = None
        reference = artifacts.work / "reference.c"
        for sample in range(-1, args.samples): # First pair warms both sides.
            for variant in (("A", "B") if sample % 2 == 0 else ("B", "A")):
                directory = binaries[variant]
                stem = artifacts.work / f"{name}-{sample}-{variant}"
                result_file = stem.with_suffix(".c")
                command = [str(directory / ("neverd-mips-bench" if kind == "primitive" else "neverd")), *tail]
                if kind == "request":
                    command += ["-o", str(result_file)]
                env = dict(os.environ)
                for key in ("NEVERD_THREADS", "NEVERD_NATIVE_PHASES", "DYLD_PRINT_LIBRARIES"):
                    env.pop(key, None)
                if threads != "default":
                    env["NEVERD_THREADS"] = threads
                key = "DYLD_LIBRARY_PATH" if platform.system() == "Darwin" else "LD_LIBRARY_PATH"
                env[key] = str(directory)
                with artifacts.process(command, stem.with_suffix(".stdout"),
                                       stem.with_suffix(".stderr"), env) as (process, start):
                    _, status, usage = os.wait4(process.pid, 0)
                    process.returncode = os.waitstatus_to_exitcode(status)
                    wall = time.monotonic() - start
                if process.returncode:
                    artifacts.keep_source(result_file)
                    raise RuntimeError(f"{name}/{variant} exited {process.returncode}; see commands.json")
                content = (json.loads(stem.with_suffix(".stdout").read_text()) if kind == "primitive"
                           else {"source_sha256": digest(result_file), "source_bytes": result_file.stat().st_size})
                stable = {k: v for k, v in content.items() if k not in ("cpu_seconds", "wall_seconds")}
                record = {"job": name, "variant": variant, "sample": sample, "kind": kind,
                          "command": command, "threads": threads, "output": stable,
                          "wall_seconds": wall, "user_seconds": usage.ru_utime,
                          "system_seconds": usage.ru_stime,
                          "peak_rss_bytes": usage.ru_maxrss * (1 if platform.system() == "Darwin" else 1024)}
                if kind == "primitive":
                    record["inner_cpu_seconds"] = content["cpu_seconds"]
                    record["inner_wall_seconds"] = content["wall_seconds"]
                records.append(record)
                artifacts.write("samples.json", records)
                if first_output is None:
                    first_output = stable
                    if kind == "request":
                        shutil.copyfile(result_file, reference)
                elif stable != first_output:
                    artifacts.keep_source(reference)
                    artifacts.keep_source(result_file)
                    raise RuntimeError(f"{name}: non-timing outputs differ; no comparable timing claim")
                artifacts.discard(result_file, stem.with_suffix(".stdout"), stem.with_suffix(".stderr"))
        selected = [r for r in records if r["job"] == name]
        if any(r["output"] != selected[0]["output"] for r in selected):
            raise RuntimeError(f"{name}: non-timing outputs differ; no comparable timing claim")
        artifacts.discard(reference)
        print("qualified", name, flush=True)
    for profile in profiles:
        reference = next(r["output"] for r in records if r["job"] == profile + "-capstone")
        decoded = next(r["output"] for r in records if r["job"] == profile + "-decode")
        if reference != decoded:
            raise RuntimeError(f"{profile}: physical decode contracts differ from Capstone")
    summaries = []
    for name, _, kind, _ in jobs:
        selected = [r for r in records if r["job"] == name and r["sample"] >= 0]
        cpu = lambda r: r["inner_cpu_seconds"] if kind == "primitive" else r["user_seconds"] + r["system_seconds"]
        a = [r for r in selected if r["variant"] == "A"]
        b = [r for r in selected if r["variant"] == "B"]
        deltas = [(cpu(y) / cpu(x) - 1) * 100 for x, y in zip(a, b)]
        summaries.append({"job": name, "cpu_A_median": statistics.median(map(cpu, a)),
                          "cpu_B_median": statistics.median(map(cpu, b)),
                          "paired_cpu_delta_percent_median": statistics.median(deltas),
                          "paired_cpu_delta_percent_range": [min(deltas), max(deltas)],
                          "wall_A_median": statistics.median(r["wall_seconds"] for r in a),
                          "wall_B_median": statistics.median(r["wall_seconds"] for r in b),
                          "rss_A_median": statistics.median(r["peak_rss_bytes"] for r in a),
                          "rss_B_median": statistics.median(r["peak_rss_bytes"] for r in b)})
    result = {"base_commit": args.base_commit, "host": platform.platform(),
              "samples_per_variant": args.samples, "repeats": args.repeats,
              "dependency_inputs_sha256": digest(args.baseline_inputs),
              "toolchain": toolchain_a,
              "candidate_toolchain": toolchain_b,
              "toolchain_inputs_sha256": {"A": digest(args.baseline_toolchain),
                                          "B": digest(args.candidate_toolchain)},
              "firmware_sha256": digest(args.firmware),
              "binary_hashes": {v: {p.name: digest(p) for p in d.iterdir()
                                     if p.name in ("neverd", "neverd-mips-bench", "libneverd.dylib", "libneverd.so")}
                                for v, d in binaries.items()},
              "code_hashes": {p.name: digest(p) for p in args.code.glob("*-unchanged.bin")},
              "summaries": summaries,
              "limits": "Shared host, no CPU affinity pinning. Direct Capstone is a decode-only comparator; added word guards/new instruction forms are excluded from same-output speedup claims. No world-leading claim."}
    artifacts.write("summary.json", result)
    print(json.dumps(result["summaries"], indent=2))


if __name__ == "__main__":
    main()
