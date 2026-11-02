#!/usr/bin/env python3
"""Measure the workbench's engine work against IDA's on the same binaries.

The workbench side drives the real worker as the GUI does when it opens a
file: open, the first page of the function list and of the listing, then the
reference index (finished on demand, as an explicit cross-reference query
does), then every page of each sampled function's C. The first-page latency
is recorded separately. Optional warming finishes before the timed requests.
The IDA side runs IDA as a library (idalib) in its own
Python: it loads the file without analysis in one process, and in another
opens it with auto-analysis, waits for it to finish, then decompiles the same
functions with the decompiler and prints them as text.

Every sample starts fresh processes on a fresh copy of the input, so neither
side reuses a database or a decompiled function.  The function sample is the
seeded choice of entries both sides found.  The report keeps each sample's
raw timings beside nearest-rank percentiles.  OS file caches stay warm: this
is warm repeated processing, not a cold-cache result.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

TRANSPORT = Path(__file__).resolve().parents[2] / "neverd-worker" / "tests" / "transport_test.py"
# Lines of pseudocode the workbench asks for when F5 opens a function.
PAGE_LINES = 512


def percentiles(samples):
    if not samples:
        return {}
    ordered = sorted(samples)

    def rank(p):
        return ordered[max(0, math.ceil(len(ordered) * p) - 1)]

    return dict(count=len(ordered), p50_ms=rank(.5), p95_ms=rank(.95), max_ms=ordered[-1],
                total_ms=sum(ordered), mean_ms=statistics.mean(ordered))


def peak_rss_bytes(pid):
    """The process's peak resident set (VmHWM), on Linux."""
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) * 1024
    except OSError:
        pass
    return None


# --- The workbench side -------------------------------------------------------

def load_transport():
    spec = importlib.util.spec_from_file_location("ida_compare_transport", TRANSPORT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def call(client, operation, payload, timeout):
    request = client.send(operation, payload)
    response = client.next(lambda message: message.get("type") == "response"
                           and message.get("request_id") == request
                           and message.get("status") != "progress", timeout=timeout)
    if response["status"] != "ok":
        raise RuntimeError(f"{operation}: {response.get('error')}")
    return response["payload"]


def run_neverd(transport, worker, binary, functions, timeout, *, warm=False):
    with tempfile.TemporaryDirectory(prefix="neverd-compare-") as directory:
        copied = Path(directory) / binary.name
        shutil.copyfile(binary, copied)
        return measure_neverd(transport, worker, copied, functions, timeout, warm=warm)


def measure_neverd(transport, worker, binary, functions, timeout, *, warm=False):
    """Opens binary in a fresh worker as the GUI does, finishes its reference
    index, then reads the complete C of each of functions; returns the
    timings and the function entries."""
    started = time.perf_counter()
    client = transport.Client(str(worker))
    result = dict(startup_ms=(time.perf_counter() - started) * 1e3)

    def timed(name, operation, payload):
        started = time.perf_counter()
        value = call(client, operation, payload, timeout)
        result[name] = (time.perf_counter() - started) * 1e3
        return value

    try:
        opened = time.perf_counter()
        timed("open_ms", "open", {"path": str(binary), "read_only": True,
                                  "analysis": True, "warm": warm})
        page = timed("function_list_ms", "functions", {"limit": 512})
        items = page.get("items", [])
        first = items[0]["address"] if items else None
        timed("listing_ms", "disasm", {"address": first, "limit": 128} if first else {"limit": 128})
        result["browse_ms"] = result["open_ms"] + result["function_list_ms"] + result["listing_ms"]
        entries, offset = [int(item["address"], 16) for item in items], len(items)
        while len(items) == 512:
            items = call(client, "functions", {"offset": offset, "limit": 512}, timeout).get("items", [])
            entries += [int(item["address"], 16) for item in items]
            offset += len(items)
        if first:
            timed("references_ms", "xrefs", {"address": first, "direction": "to", "limit": 1})
        result["analysis_ms"] = result["browse_ms"] + result.get("references_ms", 0)
        if warm:
            deadline = time.monotonic() + timeout
            while True:
                beat = client.next(lambda message: message.get("type") == "heartbeat",
                                   timeout=max(0, deadline - time.monotonic()))
                state = (beat.get("background") or {}).get("warm") or {}
                if state.get("state") in ("complete", "full", "unavailable", "off"):
                    result["warm_state"] = state
                    break
            if state["state"] in ("unavailable", "off"):
                raise RuntimeError(f"requested warming did not run: {state}")
        result["ready_ms"] = (time.perf_counter() - opened) * 1e3
        result["functions"] = len(entries)
        result["entries"] = entries
        known_entries = set(entries)
        timings, failures, records, first_pages = [], [], [], []
        for entry in functions:
            started = time.perf_counter()
            record = dict(entry=f"0x{entry:x}")
            try:
                if entry not in known_entries:
                    raise RuntimeError("not recognized as a function entry")
                offset, seen, chunks = 0, set(), []
                while offset is not None:
                    if offset in seen:
                        raise RuntimeError(f"repeated source-page offset {offset}")
                    seen.add(offset)
                    page = call(client, "decompile", {"address": record["entry"],
                                                     "representation": "c", "offset": offset,
                                                     "limit": PAGE_LINES}, timeout)
                    if len(seen) == 1:
                        record["first_page_ms"] = (time.perf_counter() - started) * 1e3
                    chunks.append(page.get("text", ""))
                    following = page.get("next_offset")
                    if following is not None and (type(following) is not int or following <= offset):
                        raise RuntimeError(f"invalid source-page offset {following!r}")
                    offset = following
                source = "".join(chunks).encode("utf-8")
                elapsed = (time.perf_counter() - started) * 1e3
                if not source:
                    raise RuntimeError("empty C source")
                record.update(ms=elapsed, bytes=len(source), pages=len(seen),
                              sha256=hashlib.sha256(source).hexdigest())
                timings.append(elapsed)
                first_pages.append(record["first_page_ms"])
            except (RuntimeError, TimeoutError) as error:
                record["error"] = str(error)[:200]
                failures.append(dict(entry=record["entry"], error=record["error"]))
            records.append(record)
        result.update(decompile_ms=timings, first_page_ms=first_pages,
                      decompile_records=records, decompile_failures=failures,
                      peak_rss_bytes=peak_rss_bytes(client.process.pid))
    finally:
        try:
            client.close()
        except Exception:
            client.process.kill()
    return result


# --- The IDA side (runs in IDA's Python) ----------------------------------------

def ida_child(binary, functions_file, output):
    started = time.perf_counter()
    import idapro  # noqa: F401  (idalib must be imported first)
    import ida_auto
    import ida_hexrays
    import ida_kernwin
    import idautils
    result = dict(startup_ms=(time.perf_counter() - started) * 1e3,
                  kernel_version=ida_kernwin.get_kernel_version())
    load_only = functions_file == "--load-only"
    started = time.perf_counter()
    if idapro.open_database(str(binary), not load_only) != 0:
        raise SystemExit(f"IDA could not open {binary}")
    if load_only:
        result["browse_ms"] = (time.perf_counter() - started) * 1e3
        idapro.close_database(False)
        Path(output).write_text(json.dumps(result))
        return
    ida_auto.auto_wait()
    result["analysis_ms"] = (time.perf_counter() - started) * 1e3
    result["ready_ms"] = result["analysis_ms"]
    entries = list(idautils.Functions())
    result["functions"] = len(entries)
    result["entries"] = entries
    known_entries = set(entries)
    timings, failures, records = [], [], []
    functions = json.loads(Path(functions_file).read_text())
    if functions and not ida_hexrays.init_hexrays_plugin():
        raise SystemExit("the decompiler is not available")
    for entry in functions:
        started = time.perf_counter()
        record = dict(entry=f"0x{entry:x}")
        try:
            if entry not in known_entries:
                raise RuntimeError("not recognized as an IDA function entry")
            function = ida_hexrays.decompile(entry)
            if function is None:
                raise RuntimeError("decompile returned None")
            text = str(function).encode("utf-8")
            elapsed = (time.perf_counter() - started) * 1e3
            if not text:
                raise RuntimeError("empty C source")
            record.update(ms=elapsed, bytes=len(text), sha256=hashlib.sha256(text).hexdigest())
            timings.append(elapsed)
        except Exception as error:  # DecompilationFailure and friends.
            record["error"] = str(error)[:200]
            failures.append(dict(entry=record["entry"], error=record["error"]))
        records.append(record)
    result.update(decompile_ms=timings, decompile_records=records, decompile_failures=failures,
                  peak_rss_bytes=peak_rss_bytes(os.getpid()))
    idapro.close_database(False)
    Path(output).write_text(json.dumps(result))


def run_ida(ida_python, binary, functions, scratch, timeout, load_only=False):
    """Runs IDA on a fresh copy of binary, so no database exists yet."""
    copy_directory = Path(tempfile.mkdtemp(prefix="ida-", dir=scratch))
    try:
        copy = copy_directory / binary.name
        shutil.copyfile(binary, copy)
        functions_file = copy_directory / "functions.json"
        functions_file.write_text(json.dumps(functions))
        output = copy_directory / "result.json"
        started = time.perf_counter()
        child = subprocess.run([str(ida_python), str(Path(__file__).resolve()), "--ida-child",
                                str(copy), "--load-only" if load_only else str(functions_file),
                                str(output)],
                               timeout=timeout, cwd=copy_directory, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
        if child.returncode != 0 or not output.exists():
            raise RuntimeError(f"IDA failed on {binary.name}: {child.stderr[-2000:]}")
        result = json.loads(output.read_text())
        result["process_ms"] = (time.perf_counter() - started) * 1e3
        return result
    finally:
        shutil.rmtree(copy_directory, ignore_errors=True)


# --- Report -----------------------------------------------------------------------

def summarize(samples, browse=None):
    decompile = [value for sample in samples for value in sample["decompile_ms"]]
    rss = [sample["peak_rss_bytes"] for sample in samples if sample.get("peak_rss_bytes")]
    return dict(browse=percentiles([sample["browse_ms"] for sample in browse or samples]),
                analysis=percentiles([sample["analysis_ms"] for sample in samples]),
                ready=percentiles([sample["ready_ms"] for sample in samples]),
                decompile=percentiles(decompile), functions=samples[0]["functions"],
                decompile_failures=max(len(sample["decompile_failures"]) for sample in samples),
                startup=percentiles([sample["startup_ms"] for sample in samples]),
                peak_rss_bytes=max(rss) if rss else None)


def markdown(report):
    def ms(value):
        return "-" if value is None else (f"{value / 1e3:.2f} s" if value >= 1e4 else f"{value:.0f} ms")

    lines = [f"Samples per binary: {report['samples']}; selected function addresses are retained "
             f"in JSON; machine: {report['machine']}.", "",
             "| Binary | Size | Functions (NeverD / IDA) | Browsable p50 (NeverD / IDA) "
             "| Analysis complete p50 | Ready p50 | F5 p50 (NeverD / IDA) | F5 p95 | F5 max | Failed F5 "
             "| Peak RSS (NeverD / IDA) |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for row in report["binaries"]:
        n, i = row["neverd"], row["ida"]

        def pair(key, field):
            return f"{ms(n[key].get(field))} / {ms(i[key].get(field))}"

        rss = " / ".join("-" if side["peak_rss_bytes"] is None else f"{side['peak_rss_bytes'] / 2**20:.0f} MiB"
                         for side in (n, i))
        lines.append(f"| {row['name']} | {row['size'] / 2**20:.1f} MiB | {n['functions']} / "
                     f"{i['functions']} | {pair('browse', 'p50_ms')} | {pair('analysis', 'p50_ms')} | "
                     f"{pair('ready', 'p50_ms')} | "
                     f"{pair('decompile', 'p50_ms')} | "
                     f"{pair('decompile', 'p95_ms')} | {pair('decompile', 'max_ms')} | "
                     f"{n['decompile_failures']} / {i['decompile_failures']} | {rss} |")
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--ida-child":
        ida_child(*sys.argv[2:])
        return 0
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--worker", type=Path, required=True, help="the neverd-worker executable")
    parser.add_argument("--engine", type=Path,
                        help="the libneverd the worker loads, recorded by its SHA-256")
    parser.add_argument("--ida-python", type=Path, required=True,
                        help="a Python interpreter with IDA's idapro package activated")
    parser.add_argument("--samples", type=int, default=3)
    parser.add_argument("--decompile", type=int, default=100,
                        help="functions to decompile per sample, chosen from those both sides found")
    parser.add_argument("--entries-file", type=Path,
                        help="one hexadecimal entry per line; requires exactly one binary")
    parser.add_argument("--warm", action="store_true",
                        help="finish the worker's background warming before decompiling")
    parser.add_argument("--seed", type=int, default=3389)
    parser.add_argument("--timeout", type=float, default=3600, help="seconds per step")
    parser.add_argument("--output", type=Path, required=True, help="the JSON report")
    parser.add_argument("--markdown", type=Path, help="also write a Markdown summary")
    parser.add_argument("binaries", type=Path, nargs="+")
    args = parser.parse_args()
    if args.samples < 1 or args.decompile < 0 or args.timeout <= 0:
        parser.error("--samples and --timeout must be positive and --decompile nonnegative")
    if args.entries_file and len(args.binaries) != 1:
        parser.error("--entries-file requires exactly one binary")
    transport = load_transport()
    report = dict(schema=2, kind="engine-work-vs-ida", platform=platform.platform(),
                  machine=platform.machine(), cpus=os.cpu_count(), samples=args.samples,
                  decompile=args.decompile, seed=args.seed, page_lines=PAGE_LINES,
                  warm=args.warm,
                  entries_file_sha256=(hashlib.sha256(args.entries_file.read_bytes()).hexdigest()
                                       if args.entries_file else None),
                  worker_sha256=hashlib.sha256(args.worker.read_bytes()).hexdigest(),
                  engine_sha256=(hashlib.sha256(args.engine.read_bytes()).hexdigest()
                                 if args.engine else None),
                  load_average=[round(value, 1) for value in os.getloadavg()],
                  caveats=["Warm OS caches; fresh processes and databases for every sample.",
                           "Browsable: the workbench's open, first function-list page and "
                           "first listing page; IDA's load without analysis (its GUI shows "
                           "the listing while auto-analysis runs).",
                           "Analysis complete: the workbench adds the whole function list "
                           "and its reference index; IDA's load with auto-analysis.",
                           "F5 reads complete C on both sides. Worker records also retain "
                           "the first-page latency, output size and SHA-256.",
                           "Warming, when requested, is included in ready_ms, outside F5.",
                           "IDA's figures include idalib's Python bindings."],
                  binaries=[])
    with tempfile.TemporaryDirectory(prefix="ida-compare-") as scratch:
        for binary in args.binaries:
            binary = binary.resolve()
            print(f"{binary.name}: discovering functions", file=sys.stderr)
            neverd = run_neverd(transport, args.worker, binary, [], args.timeout)
            ida = run_ida(args.ida_python, binary, [], scratch, args.timeout)
            common = sorted(set(neverd["entries"]) & set(ida["entries"]))
            chosen = (list(dict.fromkeys(int(value, 16) for value in
                                        args.entries_file.read_text().split()))
                      if args.entries_file else
                      sorted(random.Random(args.seed).sample(common, min(args.decompile, len(common)))))
            row = dict(name=binary.name, size=binary.stat().st_size,
                       sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                       common_functions=len(common), decompiled=[f"0x{entry:x}" for entry in chosen],
                       missing_neverd=[f"0x{entry:x}" for entry in chosen if entry not in neverd["entries"]],
                       missing_ida=[f"0x{entry:x}" for entry in chosen if entry not in ida["entries"]],
                       samples=dict(neverd=[], ida=[], ida_load=[]))
            for index in range(args.samples):
                print(f"{binary.name}: sample {index + 1}/{args.samples}", file=sys.stderr)
                for side in (("neverd", "ida") if index % 2 == 0 else ("ida", "neverd")):
                    result = (run_neverd(transport, args.worker, binary, chosen, args.timeout,
                                         warm=args.warm) if side == "neverd" else
                              run_ida(args.ida_python, binary, chosen, scratch, args.timeout))
                    result.pop("entries", None)
                    row["samples"][side].append(result)
                row["samples"]["ida_load"].append(run_ida(args.ida_python, binary, [], scratch,
                                                          args.timeout, load_only=True))
            row["neverd"] = summarize(row["samples"]["neverd"])
            row["ida"] = summarize(row["samples"]["ida"], row["samples"]["ida_load"])
            report["binaries"].append(row)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if args.markdown:
        args.markdown.write_text(markdown(report), encoding="utf-8")
    print(markdown(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
