#!/usr/bin/env python3
"""Measure fresh production GUI processes with warm OS/application caches."""

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time


def identity(path):
    path = path.resolve()
    result = {"path": str(path)}
    try:
        digest = hashlib.sha256()
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        result.update(sha256=digest.hexdigest(), bytes=path.stat().st_size)
    except OSError as error:
        result["error"] = str(error)
    return result


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return None

    def percentile(fraction):
        return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]

    return {"samples": len(values), "p50_ms": percentile(.50),
            "p95_ms": percentile(.95), "p99_ms": percentile(.99),
            "min_ms": ordered[0], "max_ms": ordered[-1],
            "mean_ms": statistics.mean(values)}


def validate_report(report, returncode):
    if not isinstance(report, dict):
        return "GUI report must be a JSON object"
    if returncode != 0 or not report.get("success"):
        return report.get("error") or report.get("failure_reason") or "GUI failed"
    if report.get("schema_version") != 2:
        return "Startup report schema 2 is required for synchronized frame measurement"
    milestones = report.get("milestones", {})
    if not isinstance(milestones, dict):
        return "GUI milestones must be a JSON object"
    required = ("worker_ready_ms", "metadata_ms", "instructions_ms",
                "representation_ms", "qml_created_ms", "first_frame_ms",
                "useful_frame_sync_ms", "useful_frame_ms")
    for name in required:
        value = milestones.get(name)
        if not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            return f"Missing or invalid milestone: {name}"
    if not (max(milestones["instructions_ms"], milestones["representation_ms"])
            <= milestones["useful_frame_sync_ms"] <= milestones["useful_frame_ms"]):
        return "Useful frame precedes synchronization of required analysis data"
    if milestones["first_frame_ms"] > milestones["useful_frame_ms"]:
        return "Useful frame precedes the first frame"
    return None


def fixture_bytes():
    path = Path(__file__).resolve().parents[1] / "tests/analysis_probe_test.py"
    spec = importlib.util.spec_from_file_location("gui_startup_fixture", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.analysis_fixture()


def write_report(path, report):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", required=True, type=Path)
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--engine", required=True, type=Path,
                        help="Actual libneverd dependency used by this worker; recorded for identity")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--binary", type=Path,
                        help="Override the default benign, named-function ELF fixture")
    parser.add_argument("--samples", type=int, default=8)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=30,
                        help="Per-launch main-entry deadline in seconds; hard process limit adds 5 seconds")
    args = parser.parse_args()
    if not 1 <= args.samples <= 1000 or not 0 <= args.warmup <= 20:
        parser.error("samples must be 1–1000 and warmup must be 0–20")
    if not math.isfinite(args.timeout) or not .1 <= args.timeout <= 300:
        parser.error("timeout must be 0.1–300 seconds")

    artifacts = {"gui": identity(args.gui), "worker": identity(args.worker),
                 "engine": identity(args.engine)}
    environment_keys = ("QT_QPA_PLATFORM", "QT_QUICK_BACKEND", "QSG_RHI_BACKEND",
                        "QSG_RENDER_LOOP", "QT_SCALE_FACTOR", "QT_FONT_DPI",
                        "QSG_RENDERER_DEBUG", "QT_DEBUG_PLUGINS", "DISPLAY",
                        "WAYLAND_DISPLAY", "XDG_SESSION_TYPE", "LANG", "LC_ALL")
    report = {"schema_version": 1, "kind": "production-gui-startup",
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "platform": platform.platform(), "machine": platform.machine(),
              "cpu_count": os.cpu_count(), "python": platform.python_version(),
              "environment": {key: os.environ[key] for key in environment_keys if key in os.environ},
              "artifacts": artifacts, "engine_identity": "caller-supplied actual dependency path",
              "requested_samples": args.samples, "warmup_launches": args.warmup,
              "percentile_method": "nearest rank; with 8 samples p95/p99 equal the maximum",
              "samples": [], "warmup": [], "success": False,
              "caveats": [
                  "Fresh GUI and worker processes; OS and application caches are warm and are not flushed.",
                  "Qt frameSwapped emission is not independently measured hardware presentation.",
                  "Milestones begin at main entry after dynamic loading; process wall time includes shutdown.",
                  "Useful frame synchronizes instructions and representation in the fresh production layout.",
                  "The default tiny ELF is analyzed but never executed; large-image performance is not implied.",
                  "Other system activity is not controlled; this run establishes no world ranking or speedup."]}
    if any("error" in value for value in artifacts.values()):
        report["error"] = "A required build artifact is unavailable"
        write_report(args.output, report)
        return 1

    with tempfile.TemporaryDirectory(prefix="neverd-gui-startup-") as directory:
        temporary = Path(directory)
        binary = args.binary.resolve() if args.binary else temporary / "named-native.elf"
        if args.binary is None:
            binary.write_bytes(fixture_bytes())
        report["fixture"] = identity(binary)
        report["fixture"]["generator"] = (
            "caller-supplied" if args.binary else "tools/neverd-gui/tests/analysis_probe_test.py:analysis_fixture")
        for index in range(args.warmup + args.samples):
            destination = temporary / f"startup-{index}.json"
            command = [str(args.gui.resolve()), "--worker", str(args.worker.resolve()),
                       "--startup-benchmark", str(destination),
                       "--startup-benchmark-timeout", str(round(args.timeout * 1000)),
                       "--fresh-layout", str(binary)]
            sample = {"index": index, "command": command}
            started = time.perf_counter_ns()
            try:
                process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, errors="replace", timeout=args.timeout + 5)
                sample.update(returncode=process.returncode, diagnostics=process.stdout[-8192:])
            except (OSError, subprocess.TimeoutExpired) as error:
                sample.update(returncode=None, error=str(error))
            sample["process_wall_ms"] = (time.perf_counter_ns() - started) / 1e6
            try:
                sample["report"] = json.loads(destination.read_text(encoding="utf-8"))
                failure = validate_report(sample["report"], sample["returncode"])
                if failure:
                    sample["error"] = failure
            except (OSError, ValueError, TypeError) as error:
                sample.setdefault("error", f"Missing or invalid GUI report: {error}")
            sample["success"] = "error" not in sample
            report["warmup" if index < args.warmup else "samples"].append(sample)
            write_report(args.output, report)
            print(f"Launch {index + 1}/{args.warmup + args.samples}: "
                  f"{'ok' if sample['success'] else sample['error']}", flush=True)
            if not sample["success"]:
                break

        valid = [sample for sample in report["samples"] if sample["success"]]
        names = sorted({name for sample in valid for name in sample["report"]["milestones"]})
        report["milestone_distributions"] = {
            name: distribution([sample["report"]["milestones"][name]
                                for sample in valid if name in sample["report"]["milestones"]])
            for name in names}
        report["process_wall_distribution"] = distribution([sample["process_wall_ms"] for sample in valid])
        after = {"gui": identity(args.gui), "worker": identity(args.worker), "engine": identity(args.engine)}
        report["artifacts_unchanged"] = artifacts == after
        report["fixture_unchanged"] = report["fixture"].get("sha256") == identity(binary).get("sha256")
        report["success"] = (len(valid) == args.samples and report["artifacts_unchanged"]
                             and report["fixture_unchanged"])
    write_report(args.output, report)
    return 0 if report["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
