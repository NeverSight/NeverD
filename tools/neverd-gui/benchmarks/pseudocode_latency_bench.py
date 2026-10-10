#!/usr/bin/env python3
"""Measure fresh-session F5 latency and complete source hashes at fixed entries.

Uses the production worker protocol and, optionally, the existing idalib
comparison harness. Each function starts a new process and input copy; OS
caches remain warm. Times include IPC and source generation, not Qt painting.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time

import ida_compare_bench as comparison


def run_worker(worker, binary, entry, representation, timeout):
    started = time.perf_counter()
    client = comparison.load_transport().Client(str(worker))
    result = {"startup_ms": (time.perf_counter() - started) * 1000}

    def call(operation, payload):
        return comparison.call(client, operation, payload, timeout)

    def timed(name, operation, payload):
        started = time.perf_counter()
        value = call(operation, payload)
        result[name] = (time.perf_counter() - started) * 1000
        return value

    try:
        timed("open_ms", "open", {"path": str(binary), "read_only": True})
        timed("functions_ms", "functions", {"offset": 0, "limit": 256})
        timed("listing_ms", "listing", {"address": entry, "after": 32})
        started = time.perf_counter()
        page = call("decompile", {"address": entry,
                                   "representation": representation, "limit": 256})
        result["first_page_ms"] = (time.perf_counter() - started) * 1000
        total = page["total_lines"]
        if not isinstance(total, int) or total <= 0:
            raise RuntimeError("decompilation returned no source lines")
        digest = hashlib.sha256()
        size = 0
        for offset in range(0, total, 256):
            if offset:
                page = call("decompile", {"address": entry,
                                           "representation": representation,
                                           "offset": offset, "limit": 256})
            if page["total_lines"] != total:
                raise RuntimeError("source extent changed while paging")
            text = page["text"].encode("utf-8")
            digest.update(text)
            size += len(text)
        result.update(status="ok", complete_source_ms=(time.perf_counter() - started) * 1000,
                      total_lines=total, source_bytes=size, source_sha256=digest.hexdigest())
    except (RuntimeError, TimeoutError, EOFError, OSError, KeyError) as error:
        result.update(status="error", error=str(error))
    finally:
        result["peak_rss_bytes"] = comparison.peak_rss_bytes(client.process.pid)
        # A timed-out synchronous call may never acknowledge a close request.
        # This benchmark owns the process and always reaps it before the next run.
        client.process.kill()
        client.process.wait()
        client.stderr.join(timeout=2)
        result["phase_trace"] = [line for line in client.logs.decode("utf-8", errors="replace").splitlines()
                                 if line.startswith("[neverd-pipeline-stage]")]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True,
                        help="matching libneverd; its directory is prepended to the library path")
    parser.add_argument("--baseline-engine", type=Path,
                        help="compare another engine, alternating run order")
    parser.add_argument("--baseline-worker", type=Path,
                        help="worker paired with --baseline-engine (defaults to --worker)")
    parser.add_argument("--ida-python", type=Path)
    parser.add_argument("--entry", action="append", required=True,
                        type=lambda value: f"0x{int(value, 0):x}")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--representation", default="source")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if args.samples < 1 or args.threads < 1 or args.timeout <= 0:
        parser.error("samples, threads and timeout must be positive")
    if args.baseline_worker and not args.baseline_engine:
        parser.error("--baseline-worker requires --baseline-engine")
    worker, engine, binary = (path.resolve() for path in
                              (args.worker, args.engine, args.binary))
    if engine.name != "libneverd.so" or platform.system() != "Linux":
        parser.error("this controlled library-loading profile currently requires Linux/libneverd.so")
    engines = {"neverd": engine}
    workers = {"neverd": worker}
    if args.baseline_engine:
        baseline = args.baseline_engine.resolve()
        if baseline.name != "libneverd.so":
            parser.error("--baseline-engine must name libneverd.so")
        engines["baseline"] = baseline
        workers["baseline"] = (args.baseline_worker.resolve()
                               if args.baseline_worker else worker)
    os.environ["NEVERD_THREADS"] = str(args.threads)
    os.environ["NEVERD_NATIVE_PHASES"] = "1"
    library_path = os.environ.get("LD_LIBRARY_PATH")
    report = dict(schema=1, kind="fresh-function-source-latency", platform=platform.platform(),
                  started_at=datetime.now(timezone.utc).isoformat(),
                  logical_cpus=os.cpu_count(), threads=args.threads,
                  cpu_affinity=sorted(os.sched_getaffinity(0)),
                  representation=args.representation,
                  binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  worker_sha256=hashlib.sha256(worker.read_bytes()).hexdigest(),
                  engine_sha256=hashlib.sha256(engine.read_bytes()).hexdigest(),
                  caveats=["Warm OS caches; each entry starts a fresh worker/database.",
                           "NeverD: first 256 source lines and all subsequent pages, including IPC.",
                           "IDA: fresh auto-analysis, then decompile() and complete text.",
                           "Wall-clock measurements include other host activity; load is recorded.",
                           "Source hashes check reproducibility, not semantic equivalence to the binary."],
                  samples=[])
    if "baseline" in engines:
        report["baseline_engine_sha256"] = hashlib.sha256(engines["baseline"].read_bytes()).hexdigest()
        report["baseline_worker_sha256"] = hashlib.sha256(workers["baseline"].read_bytes()).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="neverd-source-latency-") as directory:
        scratch = Path(directory)
        for sample in range(args.samples):
            for entry in args.entry:
                row = dict(sample=sample + 1, entry=entry, load_average=os.getloadavg())
                order = list(engines)
                if sample % 2:
                    order.reverse()
                row["engine_order"] = order
                for name in order:
                    copy = scratch / f"input-{sample}-{entry}-{name}-{binary.name}"
                    shutil.copyfile(binary, copy)
                    os.environ["LD_LIBRARY_PATH"] = str(engines[name].parent) + (
                        os.pathsep + library_path if library_path else "")
                    load_before = os.getloadavg()
                    row[name] = run_worker(workers[name], copy, entry, args.representation, args.timeout)
                    row[name].update(load_before=load_before, load_after=os.getloadavg())
                if args.ida_python:
                    try:
                        copy = scratch / f"input-{sample}-{entry}-ida-{binary.name}"
                        shutil.copyfile(binary, copy)
                        # Preserve a virtual environment's python symlink:
                        # resolving it selects the base interpreter's modules.
                        ida = comparison.run_ida(args.ida_python.absolute(), copy, [int(entry, 0)],
                                                 scratch, args.timeout)
                        ida.pop("entries", None)
                        row["ida"] = ida
                    except (RuntimeError, subprocess.TimeoutExpired) as error:
                        row["ida"] = {"decompile_failures": [{"error": str(error)}]}
                report["samples"].append(row)
                args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
                print(json.dumps(row), flush=True)
    return int(any(any(row[name]["status"] != "ok" for name in engines) or
                   row.get("ida", {}).get("decompile_failures") for row in report["samples"]))


if __name__ == "__main__":
    raise SystemExit(main())
