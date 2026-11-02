"""Run pinned SBF assets in isolated workers, retaining every stage disposition."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys
import threading
import time

try:
    from .sbf_corpus_common import DEFAULT_MANIFEST, LIMITS, analysis_stages, atomic_json, load_manifest, sha256, verify_artifact
except ImportError:
    from sbf_corpus_common import DEFAULT_MANIFEST, LIMITS, analysis_stages, atomic_json, load_manifest, sha256, verify_artifact


STAGES = ("load", *analysis_stages())
KINDS = ("analysis", "llvm", "c", "rust")
OPTIONAL_KINDS = ("callx-contexts",)


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def fingerprint(worker, manifest_path):
    root = Path(__file__).resolve().parents[1]
    changed = git(root, "ls-files", "--modified", "--others", "--exclude-standard").splitlines()
    inputs = {}
    for name in sorted(set(changed)):
        path = root / name
        if path.is_file() and not path.is_symlink():
            inputs[name] = sha256(path)
    value = {"head": git(root, "rev-parse", "HEAD"),
             "source_changes": inputs, "worker": str(worker),
             "worker_sha256": sha256(worker), "manifest_sha256": sha256(manifest_path),
             "platform": platform.platform(), "machine": platform.machine(),
             "logical_cpus": os.cpu_count(), "python": sys.version,
             "started_at": datetime.now(timezone.utc).isoformat()}
    for name in ("neverd-dependency-inputs.json", "neverd-build-receipt.json"):
        path = worker.parent.parent / name
        if path.exists():
            value[name] = json.loads(path.read_text())
    return value


def read_events(path):
    events = []
    with path.open("rb") as stream:
        content = stream.read(LIMITS.max_report_bytes).decode(errors="replace")
    for line in content.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue  # A fatal signal can interrupt the last flushed record.
        if isinstance(event, dict) and "stage" in event and "state" in event:
            events.append(event)
    return events


def sample_process_group_usage(process_group):
    """Retain live child CPU observations alongside the same group RSS charge."""
    # macOS -g selects process-group leaders (Linux -g has different semantics).
    # Avoid inspecting every unrelated process for each request's RSS sample.
    selection = ["-g", str(process_group)] if sys.platform == "darwin" else ["-A"]
    result = subprocess.run(["ps", *selection, "-o", "pgid=,rss=,time=,comm="],
                            capture_output=True, text=True, timeout=1)
    processes = []
    for line in result.stdout.splitlines():
        group, rss, cpu_time, executable = line.split(None, 3)
        if int(group) == process_group:
            processes.append({"rss_bytes": int(rss) * 1024,
                              "cpu_time_text": cpu_time, "executable": executable})
    return ({"rss_bytes": sum(p["rss_bytes"] for p in processes), "processes": processes}
            if processes else None)


def execute(command, directory, timeout, memory_bytes, cancellation=None):
    directory.mkdir(parents=True, exist_ok=True)
    stdout, stderr = directory / "events.jsonl", directory / "stderr.txt"
    started = time.monotonic()
    stop_reason, peak_rss, last_usage = None, None, None
    with stdout.open("w") as out, stderr.open("w") as err:
        process = subprocess.Popen(command, stdout=out, stderr=err, start_new_session=True)
        while process.poll() is None:
            elapsed = time.monotonic() - started
            if cancellation is not None and cancellation.is_set():
                stop_reason = "cancelled"
            elif elapsed > timeout:
                stop_reason = "timeout"
            if stop_reason is None and os.name == "posix":
                try:
                    usage = sample_process_group_usage(process.pid)
                    if usage is not None:
                        last_usage = {**usage, "elapsed_seconds": elapsed}
                        rss = usage["rss_bytes"]
                        peak_rss = max(peak_rss or 0, rss)
                        if rss > memory_bytes:
                            stop_reason = "memory_budget_exhausted"
                except (OSError, ValueError, subprocess.TimeoutExpired):
                    pass
            if stop_reason is None and (stdout.stat().st_size > LIMITS.max_report_bytes or
                                        stderr.stat().st_size > LIMITS.max_output_bytes):
                stop_reason = "report_budget_exhausted"
            if stop_reason:
                if os.name == "posix":
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                else:
                    process.kill()
                break
            time.sleep(0.05)
        code = process.wait()
    events = read_events(stdout)
    stages = {name: {"state": "not_run"} for name in STAGES}
    began = {}
    for event in events:
        name, state = event["stage"], event["state"]
        if state == "started":
            began[name] = event.get("elapsed_us", 0)
        stages[name] = {"state": state, "details": event.get("details", {})}
        if state != "started" and name in began:
            stages[name]["duration_us"] = event.get("elapsed_us", 0) - began[name]
    for name, stage in stages.items():
        if stage["state"] == "started":
            stage["state"] = stop_reason or ("crashed" if code < 0 else "failed")
    # A zero exit without a terminal record never supplies a stage pass.
    return {"command": command, "exit_code": code, "signal": -code if code < 0 else None,
            "process_state": "passed" if code == 0 else stop_reason or ("crashed" if code < 0 else "failed"),
            "stop_reason": stop_reason, "elapsed_seconds": time.monotonic() - started,
            "peak_sampled_rss_bytes": peak_rss,
            "last_process_group_sample": last_usage,
            "cpu_observation": "last live process sample only; raw ps CPU-time format; exited children are absent; not total request CPU or an exclusive-host measurement",
            "rss_observation": "owned process-group RSS sum; shared pages charged per process; short-lived peaks may be missed",
            "stages": stages, "events_path": str(stdout), "stderr_path": str(stderr)}


def qualify(row, manifest, worker, cache, output, kinds, timeout, memory_bytes,
            retain_sources=False, compile_sources=False, cancellation=None):
    directory = output / row["name"]
    directory.mkdir(parents=True, exist_ok=True)
    result = {"name": row["name"], "file": row["file"], "sha256": row["sha256"],
              "profile": {**manifest["profile"], "loader": row["loader"]}, "requests": {}}
    try:
        verify_artifact(cache / row["file"], row)
        result["integrity"] = "passed"
    except (OSError, ValueError) as error:
        result["integrity"] = "failed"
        result["error"] = str(error)
        atomic_json(directory / "result.json", result)
        return result
    for kind in kinds:
        if cancellation is not None and cancellation.is_set():
            result["requests"][kind] = {"process_state": "cancelled",
                                        "stages": {s: {"state": "not_run"} for s in STAGES}}
            atomic_json(directory / "result.json", result)
            continue
        destination = directory / ("output." + {"llvm": "ll", "c": "c", "rust": "rs",
                                                 "callx-contexts": "callx-contexts.json"}.get(kind, "none"))
        command = [str(worker), str(cache / row["file"]), "--kind=" + kind,
                   "--loader=" + row["loader"], "--cluster=" + manifest["profile"]["cluster"],
                   "--max-input-bytes=" + str(LIMITS.max_file_bytes),
                   "--max-output-bytes=" + str(LIMITS.max_output_bytes)]
        if manifest["profile"]["slot"] is not None:
            command.append("--slot=" + str(manifest["profile"]["slot"]))
        if kind != "analysis":
            command.append("--output=" + str(destination))
        request = execute(command, directory / kind, timeout, memory_bytes, cancellation)
        if kind in ("c", "rust"):
            request["compilation"] = {"state": "not_run", "reason": "not requested"}
        result["requests"][kind] = request
        atomic_json(directory / "result.json", result)
        if destination.exists():
            request["output_bytes"] = destination.stat().st_size
            request["output_sha256"] = sha256(destination)
        if compile_sources and kind in ("c", "rust"):
            passed = request["stages"].get(kind, {}).get("state") == "passed"
            compiler = shutil.which("clang" if kind == "c" else "rustc")
            if not passed:
                request["compilation"] = {"state": "not_run", "reason": "emission did not pass"}
            elif not compiler:
                request["compilation"] = {"state": "not_run", "reason": "compiler unavailable"}
            else:
                source = destination
                object_path = directory / (kind + ".native.o")
                if kind == "rust":
                    source = directory / "rust-native-probe.rs"
                    shutil.copyfile(destination, source)
                    probe = Path(__file__).with_name("sbf_corpus_native_probe.rs")
                    with source.open("a") as stream:
                        stream.write("\n" + probe.read_text())
                cmd = ([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O0", "-c",
                        str(source), "-o", str(object_path)]
                       if kind == "c" else [compiler, "--edition=2021", "--crate-name=neverd_sbf_qualified",
                                            "-D", "warnings", "--crate-type=lib", "--emit=obj",
                                            "-C", "opt-level=0", str(source), "-o", str(object_path)])
                compiled = execute(cmd, directory / (kind + "-compile"), timeout, memory_bytes, cancellation)
                compiled["state"] = ("passed" if compiled["exit_code"] == 0 else
                                     compiled["stop_reason"] or "failed")
                compiled["compiler_version"] = subprocess.check_output([compiler, "--version"], text=True).splitlines()[0]
                compiled["kind"] = "native_object"
                compiled["source_sha256"] = sha256(source)
                if kind == "rust":
                    compiled["host_contract"] = "opaque unresolved Rust-ABI symbols; compile-only v2 instantiation"
                    compiled["probe_template_sha256"] = sha256(probe)
                if compiled["state"] == "passed":
                    if not object_path.is_file():
                        compiled["state"] = "failed"
                        compiled["stop_reason"] = "compiler returned success without an object"
                    else:
                        compiled["object_bytes"] = object_path.stat().st_size
                        if object_path.stat().st_size > LIMITS.max_output_bytes:
                            compiled["state"] = "budget_exhausted"
                            compiled["stop_reason"] = "native object output byte budget exceeded"
                        else:
                            compiled["object_sha256"] = sha256(object_path)
                request["compilation"] = compiled
                if not retain_sources:
                    object_path.unlink(missing_ok=True)
                    if source != destination:
                        source.unlink(missing_ok=True)
        if not retain_sources:
            destination.unlink(missing_ok=True)
        atomic_json(directory / "result.json", result)
    return result


def summarize(rows):
    summary = {"artifacts": len(rows), "integrity_passed": sum(r["integrity"] == "passed" for r in rows), "requests": {}}
    for kind in (*KINDS, *(kind for kind in OPTIONAL_KINDS
                          if any(kind in row["requests"] for row in rows))):
        names = list(STAGES) + ([] if kind == "analysis" else [kind])
        summary["requests"][kind] = {}
        process_counts = {}
        for row in rows:
            state = row["requests"].get(kind, {}).get("process_state", "not_run")
            process_counts[state] = process_counts.get(state, 0) + 1
        summary["requests"][kind]["process"] = process_counts
        for name in names:
            counts = {}
            for row in rows:
                state = row["requests"].get(kind, {}).get("stages", {}).get(name, {}).get("state", "not_run")
                counts[state] = counts.get(state, 0) + 1
            summary["requests"][kind][name] = counts
    summary["native_compilation"] = {}
    for kind in ("c", "rust"):
        counts = {}
        for row in rows:
            state = row["requests"].get(kind, {}).get("compilation", {}).get("state", "not_run")
            counts[state] = counts.get(state, 0) + 1
        summary["native_compilation"][kind] = counts
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kinds", nargs="+", choices=KINDS + OPTIONAL_KINDS, default=KINDS)
    parser.add_argument("--timeout", type=float, default=LIMITS.stage_timeout_seconds)
    parser.add_argument("--memory-bytes", type=int, default=LIMITS.worker_address_space_bytes)
    parser.add_argument("--jobs", type=int, default=LIMITS.default_jobs)
    parser.add_argument("--retain-sources", action="store_true")
    parser.add_argument("--compile", action="store_true", dest="compile_sources")
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout <= 0 or args.memory_bytes <= 0:
        parser.error("jobs, timeout and memory budget must be positive")
    worker, cache, output = args.worker.resolve(), args.cache.resolve(), args.output.resolve()
    manifest = load_manifest(args.manifest)
    output.mkdir(parents=True, exist_ok=False)
    identity = fingerprint(worker, args.manifest)
    identity["budgets"] = {"timeout_seconds_per_request": args.timeout,
                           "sampled_rss_bytes_per_request": args.memory_bytes,
                           "rss_scope": "owned_process_group_sum",
                           "jobs": args.jobs, "output_bytes": LIMITS.max_output_bytes}
    identity["expected_artifacts"] = len(manifest["artifacts"])
    identity["process_observer"] = {
        "selection": "macOS process group" if sys.platform == "darwin" else "all processes with group filter",
        "scope": "owned process group only; same RSS charge and numerical budgets"}
    atomic_json(output / "identity.json", identity)
    rows = []
    cancellation = threading.Event()
    def run(row):
        return qualify(row, manifest, worker, cache, output, args.kinds, args.timeout,
                       args.memory_bytes, args.retain_sources, args.compile_sources, cancellation)
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run, row) for row in manifest["artifacts"]]
        try:
            for future in futures:
                row = future.result()
                rows.append(row)
                atomic_json(output / "report.json", {"identity": identity, "summary": summarize(rows), "artifacts": rows})
                print(f"{len(rows)}/{len(manifest['artifacts'])} {row['name']}", flush=True)
        except KeyboardInterrupt:
            cancellation.set()
            rows = [future.result() for future in futures]
    final = {"identity": identity, "summary": summarize(rows), "artifacts": rows}
    atomic_json(output / "report.json", final)
    print(json.dumps(final["summary"], indent=2))


if __name__ == "__main__":
    main()
