#!/usr/bin/env python3
"""Alternate two checked-CPU benchmark executables and retain every sample.

Use preserved, statically linked NeverD CPU executables for before/after tests.
Labels describe the caller's source provenance; SHA-256 identifies actual binaries.
This measures one host and these workloads, not a general performance ranking.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

try:
    import resource
except ImportError:
    resource = None


WORKLOADS = {
    "initialization": 0,
    "integer": 3002,
    "branch": 5503,
    "memory": 5002,
    "tls_call": 7003,
    "two_cpu_switch": 512,
}


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def command_output(command):
    try:
        result = subprocess.run(command, text=True, capture_output=True, timeout=30)
        return {"command": command, "returncode": result.returncode,
                "stdout": result.stdout, "stderr": result.stderr}
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"command": command, "error": str(error)}


def validate(text, backend, contract="checked", wide_loop=False):
    rows = [json.loads(line) for line in text.splitlines()]
    workloads = dict(WORKLOADS)
    if wide_loop:
        workloads["wide_loop"] = 65570
    expected = {(name, warmup) for name in workloads for warmup in (False, True)}
    observed = set()
    for row in rows:
        key = (row["workload"], row["warmup"])
        if (key not in expected or key in observed
                or row["schema_version"] != 1
                or row["architecture"] != "aarch64" or row["contract"] != contract
                or row["backend"] != backend
                or row["guest_instructions"] != workloads[key[0]]
                or type(row["elapsed_ns"]) is not int or row["elapsed_ns"] <= 0
                or type(row["warmup"]) is not bool
                or row["sample"] != (0 if row["warmup"] else 1)):
            raise ValueError(f"unexpected benchmark row: {row}")
        observed.add(key)
    if observed != expected:
        raise ValueError("benchmark did not report the complete workload set")
    return rows


def summarize(runs, pairs, wide_loop=False):
    summary = {}
    workloads = list(WORKLOADS)
    if wide_loop:
        workloads.append("wide_loop")
    for workload in workloads:
        samples = {side: [next(row["elapsed_ns"] for row in run["rows"]
                              if row["workload"] == workload and not row["warmup"])
                          for run in runs if run["side"] == side]
                   for side in ("baseline", "candidate")}
        if any(len(values) != pairs for values in samples.values()):
            raise ValueError("incomplete comparison")
        stats = {side: {"median_ns": statistics.median(values),
                        "min_ns": min(values), "max_ns": max(values),
                        "samples_ns": values}
                 for side, values in samples.items()}
        ratios = [before / after for before, after in
                  zip(samples["baseline"], samples["candidate"], strict=True)]
        stats["paired_speedup"] = ratios
        stats["median_paired_speedup"] = statistics.median(ratios)
        summary[workload] = stats
    return summary


def result_hash(rows):
    # Backend identity is separately retained and validated; comparing two
    # transports is supported. Every semantic row field must still agree.
    values = [{key: value for key, value in row.items()
               if key not in ("elapsed_ns", "backend")} for row in rows]
    values.sort(key=lambda row: (row["workload"], row["warmup"]))
    return hashlib.sha256(json.dumps(values, sort_keys=True).encode()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for side in ("baseline", "candidate"):
        parser.add_argument(f"--{side}", type=Path, required=True)
        parser.add_argument(f"--{side}-label", required=True)
        parser.add_argument(f"--{side}-backend", choices=("hvf", "unicorn"), default="hvf")
    parser.add_argument("--pairs", type=int, default=9)
    parser.add_argument("--contract", choices=("checked", "checked-user"), default="checked")
    parser.add_argument("--wide-loop", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if os.environ.get("DYLD_INSERT_LIBRARIES") or os.environ.get("LD_PRELOAD"):
        parser.error("remove library injection before measuring uninstrumented timings")
    if not 1 <= args.pairs <= 1000:
        parser.error("--pairs must be between 1 and 1000")
    if args.output.exists():
        parser.error("--output already exists; preserve it and choose a new path")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = {"schema_version": 1, "complete": False,
              "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "host": {"platform": platform.platform(), "machine": platform.machine(),
                       "cpu_count": os.cpu_count()},
              "driver_sha256": sha256(Path(__file__)),
              "pairs": args.pairs, "contract": args.contract,
              "wide_loop": args.wide_loop, "binaries": {}, "runs": []}
    for side in ("baseline", "candidate"):
        path = getattr(args, side).resolve(strict=True)
        report["binaries"][side] = {
            "path": str(path), "sha256": sha256(path),
            "caller_source_label": getattr(args, f"{side}_label"),
            "backend": getattr(args, f"{side}_backend"),
        }
        if sys.platform == "darwin":
            report["binaries"][side]["dynamic_dependencies"] = command_output(["otool", "-L", str(path)])
    if sys.platform == "darwin":
        report["host"]["macos"] = command_output(["sw_vers"])
        report["host"]["cpu"] = command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
    try:
        for pair in range(args.pairs):
            order = ("baseline", "candidate") if pair % 2 == 0 else ("candidate", "baseline")
            for side in order:
                binary = report["binaries"][side]
                if sha256(Path(binary["path"])) != binary["sha256"]:
                    raise ValueError(f"{side} executable changed during measurement")
                command = [binary["path"], "--backend", binary["backend"],
                           "--samples", "1", "--warmup", "1"]
                # Preserve compatibility with older benchmark executables for
                # the unchanged default contract and workload inventory.
                if args.contract != "checked":
                    command += ["--contract", args.contract]
                if args.wide_loop:
                    command.append("--wide-loop")
                run = {"pair": pair, "side": side, "command": command,
                       "load_before": os.getloadavg() if hasattr(os, "getloadavg") else None}
                report["runs"].append(run)
                before_cpu = resource.getrusage(resource.RUSAGE_CHILDREN) if resource else None
                start = time.monotonic_ns()
                result = subprocess.run(command, capture_output=True, text=True, timeout=180)
                run.update(returncode=result.returncode, stdout=result.stdout, stderr=result.stderr,
                           process_elapsed_ns=time.monotonic_ns() - start,
                           load_after=os.getloadavg() if hasattr(os, "getloadavg") else None)
                if resource:
                    after_cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
                    run["process_user_seconds"] = after_cpu.ru_utime - before_cpu.ru_utime
                    run["process_system_seconds"] = after_cpu.ru_stime - before_cpu.ru_stime
                if result.returncode:
                    raise RuntimeError(f"{side} benchmark failed: {result.stderr}")
                run["rows"] = validate(result.stdout, binary["backend"], args.contract, args.wide_loop)
                run["non_timing_sha256"] = result_hash(run["rows"])
                if run["non_timing_sha256"] != report["runs"][0]["non_timing_sha256"]:
                    raise ValueError("non-timing benchmark output differs between runs")
                args.output.write_text(json.dumps(report, indent=2) + "\n")
                print(f"pair {pair + 1}/{args.pairs}: {side} passed", flush=True)
        report["summary"] = summarize(report["runs"], args.pairs, args.wide_loop)
        report["complete"] = True
    except (OSError, ValueError, KeyError, TypeError, RuntimeError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    finally:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    if not report["complete"]:
        print(report["error"], file=sys.stderr)
        return 1
    for name, stats in report["summary"].items():
        print(f"{name}: median paired speedup {stats['median_paired_speedup']:.3f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
