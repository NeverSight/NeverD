#!/usr/bin/env python3
"""Measure authored portable x87 programs only after their frozen execution passes.

Use one allowed CPU, retain every child result, and check final raw state against
the frozen observations. Timings are scoped measurements, never a speedup claim.
"""

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess

from benchmark_x87_workload import cpu_identity
from check_x87_cross_host import read_verified, require_clean_source_record
from check_x87_native_regression import git, linked_libraries, sha256, source_snapshot
from check_x87_portable_generated import audit_observations
from check_x87_translation import audit_observations as audit_translation_observations


@dataclass
class BenchmarkOptions:
    iterations: int = 16384
    session_iterations: int = 4
    trials: int = 5
    timeout_seconds: int = 180


def validate_measurement(row, mode, shape, mixed, iterations, exact=None, refusal=None, session=None):
    integer_fields = ("protocol", "shape", "iterations", "completed", "refused", "fallback_calls",
                      "elapsed_ns", "peak_rss_kib", "generated_object_bytes")
    if any(type(row[field]) is not int for field in integer_fields):
        raise ValueError("measurement numeric fields must be integral protocol values")
    refused = iterations // 32 if mixed else 0
    if (row["protocol"] != 1 or row["mode"] != mode or row["shape"] != shape or
            row["mixed"] is not mixed or row["iterations"] != iterations or
            row["completed"] != iterations - refused or row["refused"] != refused or
            row["fallback_available"] is not False or row["fallback_calls"] != 0 or
            row["elapsed_ns"] <= 0 or row["peak_rss_kib"] <= 0):
        raise ValueError("measurement has incoherent mode, status counts or timing")
    if mode == "generated":
        expected_return = refusal["return"] if mixed and iterations % 32 == 0 else exact["return"]
        if (row["state"] != exact["after"] or row["memory"] != exact["memory"][2]["bytes"] or
                row["return"] != expected_return):
            raise ValueError("measured generated state differs from its passing frozen observation")
        if mixed and (row["refusal_state"] != refusal["after"] or
                      row["refusal_memory"] != refusal["memory"][2]["bytes"]):
            raise ValueError("measured refusal changed its committed prefix or memory")
    elif mode == "session":
        if row["logical_wire"] != session["after"] or row["generated_object_bytes"] <= 0:
            raise ValueError("measured native session differs from frozen execution")
    elif mode == "snapshot":
        if not bytes.fromhex(row["logical_wire"]):
            raise ValueError("state transport has no observable logical state")
    elif mode == "transport":
        if row["memory"] != "A50100000000000080FF3FA5":
            raise ValueError("raw 80-bit transport changed the payload or canaries")


def main():
    defaults = BenchmarkOptions()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated-record", type=Path, required=True)
    parser.add_argument("--translation-record", type=Path)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--cpu", type=int)
    parser.add_argument("--iterations", type=int, default=defaults.iterations)
    parser.add_argument("--session-iterations", type=int, default=defaults.session_iterations)
    parser.add_argument("--trials", type=int, default=defaults.trials)
    parser.add_argument("--timeout", type=int, default=defaults.timeout_seconds)
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "aarch64"):
        parser.error("native Linux x64 or AArch64 is required")
    if min(args.iterations, args.session_iterations, args.trials, args.timeout) <= 0:
        parser.error("iteration counts, trials and timeout must be positive")
    affinity = os.sched_getaffinity(0)
    cpu = min(affinity) if args.cpu is None else args.cpu
    if cpu not in affinity:
        parser.error("selected CPU is outside the process affinity")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    record = {"protocol": "neverd-x87-portable-measurement-v1", "passed": False,
              "scope": "authored leaf, branch and two-backedge bodies; raw transport and explicit state snapshots",
              "precision": "exact portable binary80, CW=0x037f, TOP=5; no approximate mode",
              "mixed_refusals": "separate qNaN input context reset every 32nd call; no fallback is present",
              "timing": "steady_clock inside each child; one warmup per path; includes every status check",
              "transport": "raw FLD/FSTP C bridge pairs and separately timed logical state import/export",
              "session": "native AArch64 only: create, compile, link, execute, snapshot and unload per request; no block cache",
              "memory": "Linux VmHWM for the whole child address space, including loaded libraries and test fixtures",
              "exclusions": ["speed improvement", "all control words in performance measurements",
                             "third-party application compatibility", "formal equivalence", "isolated allocator accounting"],
              "configuration": {"cpu": cpu, "iterations": args.iterations,
                                "session_iterations": args.session_iterations, "trials": args.trials},
              "environment": {"platform": platform.platform(), "machine": platform.machine(),
                              "affinity_before": sorted(affinity), "load_before": os.getloadavg()},
              "measurements": [], "measurement_attempts": [], "script_sha256": sha256(Path(__file__))}
    try:
        generated_root = args.generated_record.resolve(strict=True).parent
        generated = read_verified(generated_root)
        require_clean_source_record(generated_root, generated)
        if (generated["protocol"] != "neverd-x87-portable-generated-leaf-v1" or
                not generated["passed"] or not generated["stable_inputs"]):
            raise ValueError("passing stable generated evidence is required")
        record["cpu"] = json.loads(subprocess.check_output(["lscpu", "--json"]))
        if cpu_identity(record["cpu"]) != cpu_identity(generated["cpu"]):
            raise ValueError("benchmark host differs from the frozen execution host")
        head = git("rev-parse", "HEAD").decode().strip()
        snapshot = source_snapshot()
        if head != generated["git_head"] or snapshot != (b"", {}):
            raise ValueError("benchmark requires the same clean committed source as its frozen evidence")
        record["git_head"] = head
        driver = args.driver.resolve(strict=True)
        bindings = {str(driver): sha256(driver), **linked_libraries(driver)}
        for name, digest in generated["input_sha256"].items():
            if sha256(Path(name)) != digest:
                raise ValueError("generated input changed before measurement: " + name)
            bindings[name] = digest
        record["input_sha256"] = bindings
        record["generated_record_sha256"] = sha256(args.generated_record)
        directories = list(generated_root.glob("artifacts/x87-generated*"))
        if len(directories) != 1:
            raise ValueError("expected one authored generated artifact directory")
        directory = directories[0]
        commands = [json.loads(line) for line in (directory / "commands.jsonl").read_text().splitlines()]
        observations = [json.loads(line) for line in (directory / "observations.jsonl").read_text().splitlines()]
        audit_observations(commands, observations, platform.machine() == "x86_64")
        by_case = {(row["artifact"], row["case"]): row for row in observations}
        commands = {row["artifact"]: row for row in commands}
        session = None
        translation_root = None
        if platform.machine() == "aarch64":
            if not args.translation_record:
                raise ValueError("native ARM timing requires its passing translation evidence")
            translation_root = args.translation_record.resolve(strict=True).parent
            translation = read_verified(translation_root)
            require_clean_source_record(translation_root, translation)
            if not translation["passed"] or not translation["stable_inputs"] or translation["git_head"] != head:
                raise ValueError("translation evidence does not match the benchmark")
            rows = [json.loads(line) for line in (translation_root / "artifacts/observations.jsonl").read_text().splitlines()]
            audit_translation_observations(rows)
            session = next(row for row in rows if row["case"] == "exact-5-3-0" and row["level"] == 2)
            record["translation_record_sha256"] = sha256(args.translation_record)
        workloads = []
        for shape in range(3):
            for route in range(3):
                name = f"default-{shape}-{route}-O2"
                command = commands[name]["command"]
                library = Path(command[command.index("-o") + 1]).resolve(strict=True)
                if not library.is_relative_to(directory):
                    raise ValueError("selected library is outside the authored evidence")
                for mixed in (False, True):
                    workloads.append((name + ("-mixed" if mixed else "-exact"), "generated", shape, mixed,
                                      args.iterations, str(library), name))
        workloads.extend((mode, mode, 0, False, args.iterations, "-", None)
                         for mode in ("transport", "snapshot"))
        if session:
            workloads.append(("session", "session", 0, False, args.session_iterations, "-", None))
        os.sched_setaffinity(0, {cpu})
        for trial in range(args.trials):
            for label, mode, shape, mixed, count, library, artifact in (workloads if trial % 2 == 0 else reversed(workloads)):
                command = [str(driver), mode, str(count), str(shape), str(int(mixed)), library]
                prefix = output / f"{trial}-{label}"
                attempt = {"label": label, "trial": trial, "command": command}
                record["measurement_attempts"].append(attempt)
                with prefix.with_suffix(".stdout").open("wb") as stdout, prefix.with_suffix(".stderr").open("wb") as stderr:
                    completed = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=args.timeout, check=False)
                attempt["returncode"] = completed.returncode
                if completed.returncode:
                    raise ValueError(f"measurement failed ({completed.returncode}): {label}, trial {trial}")
                row = json.loads(prefix.with_suffix(".stdout").read_text())
                exact = by_case[(artifact, f'exact-5-3-0-{1 if shape == 0 else 2}')] if artifact else None
                refusal = by_case[(artifact, "refusal-4")] if artifact else None
                validate_measurement(row, mode, shape, mixed, count, exact, refusal, session)
                record["measurements"].append({"label": label, "trial": trial, "command": command, **row})
        record["summary"] = []
        for label, mode, *_ in workloads:
            rows = [row for row in record["measurements"] if row["label"] == label]
            rates = [row["iterations"] * 1e9 / row["elapsed_ns"] for row in rows]
            record["summary"].append({"label": label, "mode": mode, "trials": len(rows),
                "units_per_second_min": min(rates), "units_per_second_median": statistics.median(rates),
                "units_per_second_max": max(rates), "peak_rss_kib_max": max(row["peak_rss_kib"] for row in rows),
                "refusals": sum(row["refused"] for row in rows), "attempts": sum(row["iterations"] for row in rows),
                "fallback_calls": sum(row["fallback_calls"] for row in rows)})
        read_verified(generated_root)
        if translation_root:
            read_verified(translation_root)
        record["stable_inputs"] = (source_snapshot() == snapshot and git("rev-parse", "HEAD").decode().strip() == head and
            all(sha256(Path(name)) == digest for name, digest in bindings.items()) and
            sha256(args.generated_record) == record["generated_record_sha256"] and
            (not translation_root or sha256(args.translation_record) == record["translation_record_sha256"]))
        if not record["stable_inputs"]:
            raise ValueError("inputs changed while measuring")
        record["passed"] = True
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError, StopIteration) as error:
        record["error"] = str(error)
    finally:
        os.sched_setaffinity(0, affinity)
    record["environment"]["load_after"] = os.getloadavg()
    record["files"] = {str(path.relative_to(output)): {"bytes": path.stat().st_size, "sha256": sha256(path)}
                       for path in sorted(output.rglob("*")) if path.is_file()}
    (output / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"passed": record["passed"], "measurements": len(record["measurements"]),
                      "error": record.get("error"), "record": str(output / "record.json")}, indent=2))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
