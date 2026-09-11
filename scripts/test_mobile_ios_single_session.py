#!/usr/bin/env python3
"""Verify the single-session CLI against the existing twelve macOS variants.

Run only after the scalar, calls and Swift recovery/behavior runners. This is
CI orchestration, not a decoder. Metadata-only uses the unchanged direct-loader
path as the whole Swift metadata oracle. All original artifacts are retained.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

VARIANTS = tuple((corpus, architecture, fixup)
                 for corpus in ("scalar", "calls", "swift")
                 for architecture in ("arm64", "x86_64")
                 for fixup in ("classic", "default"))
PHASE = re.compile(
    r"\[neverd-child-phase\] phase=(session_load|objc_export|pipeline) "
    r"event=(begin|completed|failed|aborted) iteration=([0-9]+) elapsed_ms=([0-9]+)"
)


def read_bounded(path: Path, maximum: int) -> bytes:
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"Missing regular evidence file: {path}")
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    if len(data) > maximum:
        raise RuntimeError(f"Evidence file exceeds its limit: {path}")
    return data


def document(path: Path) -> dict:
    value = json.loads(read_bounded(path, 32 * 1024 * 1024))
    if not isinstance(value, dict):
        raise RuntimeError(f"Evidence must be a JSON object: {path}")
    return value


def digest(path: Path) -> str:
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"Missing regular binary: {path}")
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def validate_trace(output: Path, report: dict) -> dict:
    outputs = report.get("outputs")
    if not isinstance(outputs, dict) or outputs.get("native_log") != "logs/native.log":
        raise RuntimeError("Missing shared native worker log")
    swift = report.get("swift_method_recovery")
    if (not isinstance(swift, dict) or type(swift.get("method_count")) is not int or
            swift["method_count"] < 0):
        raise RuntimeError("Missing Swift callable inventory")
    needs_swift = swift["method_count"] > 0
    if (needs_swift or "swift_native_log" in outputs) and outputs.get("swift_native_log") != "logs/native.log":
        raise RuntimeError("Swift export must reference the same native worker log")
    log_directory = output / "logs"
    if log_directory.is_symlink() or not log_directory.is_dir():
        raise RuntimeError("Missing regular worker log directory")
    if {path.name for path in log_directory.iterdir()} != {"native.log"}:
        raise RuntimeError("Recovery contains additional child logs")
    raw = read_bounded(log_directory / "native.log", 16 * 1024 * 1024)
    events = []
    stack = []
    completed = []
    for line in raw.decode("utf-8").splitlines():
        if not line.startswith("[neverd-child-phase]"):
            continue
        match = PHASE.fullmatch(line)
        if not match:
            raise RuntimeError("Malformed native phase record")
        phase, event, iteration, elapsed = match.groups()
        key = (phase, int(iteration))
        if event == "begin":
            if stack != ([("objc_export", 0)] if phase == "pipeline" else []):
                raise RuntimeError("Native phase began outside its owning scope")
            if elapsed != "0" or key in stack or key in completed:
                raise RuntimeError("Repeated native phase or invalid begin time")
            stack.append(key)
        else:
            if event != "completed" or not stack or stack.pop() != key:
                raise RuntimeError("Native phase did not complete in order")
            completed.append(key)
        events.append((phase, event, int(iteration)))
    if stack:
        raise RuntimeError("Native phase has no terminal record")
    for phase in ("session_load", "objc_export"):
        if ([event for event in events if event[0] == phase] !=
                [(phase, "begin", 0), (phase, "completed", 0)]):
            raise RuntimeError(f"Expected exactly one completed {phase}")
    if events.index(("session_load", "completed", 0)) > events.index(("objc_export", "begin", 0)):
        raise RuntimeError("Native export preceded the completed session load")
    if ("pipeline", 0) not in completed:
        raise RuntimeError("No completed native pipeline")
    return {"log_sha256": hashlib.sha256(raw).hexdigest(),
            "session_loads": 1, "objc_exports": 1, "swift_export_used": needs_swift}


def run_metadata(neverd: Path, binary: Path, output: Path,
                 architecture: str, timeout: int) -> None:
    completed = subprocess.run(
        [str(neverd), "mobile", str(binary), "-o", str(output),
         "--platform=ios", f"--arch={architecture}", "--metadata-only",
         f"--timeout={timeout}"],
        capture_output=True, text=True, timeout=timeout + 30,
    )
    output.parent.joinpath("metadata-only.stdout").write_text(completed.stdout)
    output.parent.joinpath("metadata-only.stderr").write_text(completed.stderr)
    if completed.returncode:
        raise RuntimeError(f"Metadata-only CLI exited {completed.returncode}: {completed.stderr}")


def verify_variant(neverd: Path, variant: Path, corpus: str,
                   architecture: str, timeout: int) -> dict:
    binary = variant / ("libSwiftBehavior.dylib" if corpus == "swift" else "original")
    before = digest(binary)
    output = variant / "recovered"
    report = document(output / "report.json")
    if (type(report.get("schema_version")) is not int or report["schema_version"] != 1 or
            report.get("status") != "success" or
            report.get("platform") != "ios" or report.get("architecture") != architecture or
            report.get("metadata_only") is not False):
        raise RuntimeError("Missing successful recovery for the requested variant")
    trace = validate_trace(output, report)
    baseline = variant / "metadata-only"
    if baseline.exists() or baseline.is_symlink():
        raise RuntimeError("Metadata-only evidence output already exists")
    run_metadata(neverd, binary, baseline, architecture, timeout)
    metadata_report = document(baseline / "report.json")
    if (type(metadata_report.get("schema_version")) is not int or metadata_report["schema_version"] != 1 or
            metadata_report.get("status") != "success" or
            metadata_report.get("platform") != "ios" or
            metadata_report.get("architecture") != architecture or
            metadata_report.get("metadata_only") is not True or
            any(metadata_report.get(key, "missing") is not None for key in
                ("native_function_count", "objc_method_recovery", "swift_method_recovery"))):
        raise RuntimeError("Metadata-only control did not preserve its contract")
    selected = digest(output / "artifacts/selected.macho")
    if selected != digest(baseline / "artifacts/selected.macho") or before != digest(binary):
        raise RuntimeError("Selected or original binary identity changed")
    direct = document(baseline / "metadata/swift.json")
    worker = document(output / "metadata/swift.json")
    if json.dumps(direct, sort_keys=True) != json.dumps(worker, sort_keys=True):
        raise RuntimeError("Whole Swift metadata changed between direct loader and worker")
    return {"corpus": corpus, "architecture": architecture,
            "original_sha256": before, "selected_sha256": selected,
            "swift_metadata_sha256": digest(output / "metadata/swift.json"),
            "metadata_only_swift_sha256": digest(baseline / "metadata/swift.json"),
            "whole_swift_metadata_equal": True, **trace}


def verify(neverd: Path, work: Path, timeout: int) -> dict:
    results, failures = [], []
    for corpus, architecture, fixup in VARIANTS:
        label = f"{corpus}/{architecture}-{fixup}"
        try:
            result = verify_variant(neverd, work / label, corpus, architecture, timeout)
            results.append({"variant": label, **result})
            print(f"PASS single session {label}", flush=True)
        except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
            failures.append({"variant": label, "error": str(error)})
            print(f"FAIL single session {label}: {error}", file=sys.stderr, flush=True)
    evidence = {"schema_version": 1, "status": "error" if failures else "success",
                "requested_variants": len(VARIANTS), "completed_variants": len(results),
                "results": results, "failures": failures}
    (work / "single-session.json").write_text(json.dumps(evidence, indent=2) + "\n")
    if failures or len(results) != 12:
        raise RuntimeError(f"Single-session qualification incomplete: {len(results)}/12")
    return evidence


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    verify(arguments.neverd.resolve(), arguments.work_dir.resolve(), arguments.timeout)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
