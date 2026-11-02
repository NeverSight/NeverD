#!/usr/bin/env python3
"""Measure the frozen X87Workload fixtures on one native Linux x64 CPU.

Requires a passing native-regression record containing all five workload
tests. Validates every retained file and the runtime before and after running
only its default/O2 programs. No generated program is compiled or altered.
Original and generated measurements alternate order in separate processes.
This synthetic batch replay is not a third-party application benchmark.
"""

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

from check_x87_native_regression import sha256


@dataclass
class BenchmarkOptions:
    repetitions: int = 128
    trials: int = 5
    timeout_seconds: int = 60


def validate_files(root, record):
    for name, expected in record["files"].items():
        path = (root / name).resolve(strict=True)
        if not path.is_relative_to(root):
            raise ValueError("artifact leaves the recorded directory: " + name)
        if path.stat().st_size != expected["bytes"] or sha256(path) != expected["sha256"]:
            raise ValueError("artifact binding changed: " + name)
    for name, digest in record["generated_runtime_sha256"].items():
        if sha256(Path(name)) != digest:
            raise ValueError("runtime binding changed: " + name)


def cpu_identity(description):
    fields = {"Architecture:", "Byte Order:", "Vendor ID:", "Model name:",
              "CPU family:", "Model:", "Stepping:", "Flags:"}
    return {item["field"]: item["data"] for item in description["lscpu"]
            if item["field"] in fields}


def main():
    defaults = BenchmarkOptions()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--regression-record", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--cpu", required=True, type=int,
                        help="one allowed logical CPU; no frequency/governor changes")
    parser.add_argument("--repetitions", type=int, default=defaults.repetitions)
    parser.add_argument("--trials", type=int, default=defaults.trials)
    parser.add_argument("--timeout", type=int, default=defaults.timeout_seconds)
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        parser.error("requires a native Linux x86-64 host")
    if min(args.repetitions, args.trials, args.timeout) <= 0:
        parser.error("repetitions, trials and timeout must be positive")
    if args.cpu not in os.sched_getaffinity(0):
        parser.error("requested CPU is outside the process affinity")
    source = args.regression_record.resolve(strict=True)
    root = source.parent
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    report = {
        "protocol": "neverd-x87-batch-measurement-v2",
        "scope": "bounded synthetic FSIN stream and raw-transport baseline",
        "precision": "same-host raw binary80; no reduced-precision mode",
        "input_profile": {"seed": 0x871677, "control_word": 0x37f,
                          "precision_control": 64, "rounding": "nearest-even"},
        "not_evidence_for": ["arbitrary application compatibility", "other CPUs",
                             "complete typed architectural state", "proof",
                             "ARM64 translation", "speed improvement"],
        "regression_record": str(source), "regression_sha256": sha256(source),
        "script_sha256": sha256(Path(__file__)),
        "helper_sha256": sha256(Path(__file__).with_name("check_x87_native_regression.py")),
        "configuration": vars(args) | {"regression_record": str(source),
                                       "output_dir": str(destination)},
        "timing": "CLOCK_MONOTONIC inside the child; excludes process startup and IO",
        "warmup": "one untimed batch per child",
        "memory": ("Linux /proc/self/status VmHWM for the executable's address space, KiB; "
                   "ru_maxrss is retained separately because it can include the launcher"),
        "fallback": {"applicable": False, "instrumented": False,
                     "reason": "these generated native fixtures have no fallback dispatch"},
        "transport": "separate FNINIT/FLDCW/FNCLEX/FLD/FSTP batch; no FSIN or status branch",
        "environment": {"platform": platform.platform(),
                        "python": platform.python_version(),
                        "affinity_before": sorted(os.sched_getaffinity(0)),
                        "load_average_before": os.getloadavg()},
        "measurements": [], "passed": False,
    }
    started = time.monotonic()
    try:
        record = json.loads(source.read_text())
        if (record.get("protocol") != "neverd-x87-native-generated-regression-v1" or
                not record.get("passed") or not record.get("stable_inputs")):
            raise ValueError("requires a passing, stable native regression record")
        required = {"X87Workload.I386TypedBatch", "X87Workload.X64TypedBatch",
                    "X87Workload.X64OwnedBatch", "X87Workload.MicrosoftX64TypedBatch",
                    "X87Workload.MicrosoftX64OwnedBatch"}
        if {case["name"] for case in record["tests"]} != required:
            raise ValueError("record must contain exactly the five workload tests")
        report["cpu"] = json.loads(subprocess.check_output(["lscpu", "--json"]))
        # lscpu also reports live scaling frequency; that is not CPU identity.
        if cpu_identity(report["cpu"]) != cpu_identity(record["cpu"]):
            raise ValueError("CPU description differs from the regression host")
        validate_files(root, record)
        binaries = sorted(name for name in record["files"]
                          if Path(name).name in {"default-generated-O2.exe",
                                                "default-high-O2.exe",
                                                "default-llvm-O2.exe"})
        if len(binaries) != 30:
            raise ValueError("expected 5 mode/ABI/state paths, 2 workloads and 3 backends")
        os.sched_setaffinity(0, {args.cpu})
        for index, name in enumerate(binaries):
            observations = {"original": [], "generated": []}
            for trial in range(args.trials):
                order = ("original", "generated") if trial % 2 == 0 else (
                    "generated", "original")
                for kind in order:
                    command = [str(root / name), "--benchmark", kind,
                               str(args.repetitions)]
                    result = subprocess.run(command, capture_output=True, text=True,
                                            check=False, timeout=args.timeout)
                    invocation = {"argv": command, "returncode": result.returncode,
                                  "stdout": result.stdout, "stderr": result.stderr}
                    (destination / f"{index:02d}-{trial:02d}-{kind}.json").write_text(
                        json.dumps(invocation, indent=2) + "\n")
                    if result.returncode:
                        raise ValueError("measurement process failed: " + name)
                    value = json.loads(result.stdout)
                    if (value.get("protocol") != 2 or value.get("kind") != kind or
                            value.get("repetitions") != args.repetitions or
                            value.get("samples_per_batch") != 2048 or
                            value.get("control_word") != 0x37f or
                            value.get("seed") != 0x871677 or
                            value.get("address_space_peak_rss_kib", 0) <= 0 or
                            value.get("nanoseconds", 0) <= 0):
                        raise ValueError("invalid measurement response: " + name)
                    observations[kind].append(value)
            digests = {value["final_packet_fnv1a64"] for values in observations.values()
                       for value in values}
            if len(digests) != 1:
                raise ValueError("measured output differs from its original: " + name)
            entry = {"artifact": name, "observations": observations}
            for kind, values in observations.items():
                ns = [value["nanoseconds"] / args.repetitions / 2048 for value in values]
                entry[kind] = {"ns_per_sample_median": statistics.median(ns),
                               "ns_per_sample_min": min(ns), "ns_per_sample_max": max(ns),
                               "samples_per_second_median": 1e9 / statistics.median(ns),
                               "max_rss_kib": max(value["address_space_peak_rss_kib"]
                                                  for value in values)}
            entry["generated_over_original_time"] = (
                entry["generated"]["ns_per_sample_median"] /
                entry["original"]["ns_per_sample_median"])
            report["measurements"].append(entry)
            print(f"{name}: {entry['generated_over_original_time']:.3f}x original time",
                  flush=True)
        validate_files(root, record)
        if sha256(source) != report["regression_sha256"]:
            raise ValueError("regression record changed during measurement")
        if (sha256(Path(__file__)) != report["script_sha256"] or
                sha256(Path(__file__).with_name("check_x87_native_regression.py")) !=
                report["helper_sha256"]):
            raise ValueError("measurement tool changed during execution")
        report["stable_inputs"] = True
        report["passed"] = True
    except (OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        report["error"] = str(error)
    finally:
        os.sched_setaffinity(0, set(report["environment"]["affinity_before"]))
    report["environment"]["load_average_after"] = os.getloadavg()
    report["elapsed_seconds_including_process_io"] = time.monotonic() - started
    report["files"] = {path.name: sha256(path) for path in destination.iterdir()
                       if path.is_file()}
    (destination / "record.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": report["passed"], "error": report.get("error"),
                      "measurements": len(report["measurements"]),
                      "record": str(destination / "record.json")}, indent=2))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
