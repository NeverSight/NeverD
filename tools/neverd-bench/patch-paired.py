#!/usr/bin/env python3
"""Pair complete CLI/worker patch operations on a busy Linux host.

Each snapshot contains bin/neverd, bin/neverd-worker and bin/libneverd.so.
Alternate A/B then B/A for each sample; fixtures and oracles remain outside
the timer. Both workers stay open. No guest code is executed.
"""

from contextlib import ExitStack
import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location(
    "entrypoints", Path(__file__).with_name("patch-entrypoints.py"))
entry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)
bench = entry.bench


def environment(snapshot):
    return {**os.environ, "LD_LIBRARY_PATH": str(snapshot / "bin")}


def process_clock(pid):
    getclock = ctypes.CDLL(None).clock_getcpuclockid
    getclock.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    getclock.restype = ctypes.c_int
    clock = ctypes.c_int()
    error = getclock(pid, ctypes.byref(clock))
    if error:
        raise OSError(error, "cannot read worker CPU clock")
    return lambda: time.clock_gettime_ns(clock.value) / 1e6


def run_case(host, snapshots, count, samples, worker, history=False):
    with tempfile.TemporaryDirectory(prefix="neverd-patch-paired-") as temporary, ExitStack() as clients:
        fixtures, calls, clocks = {}, {}, {}
        for name, snapshot in snapshots.items():
            root = Path(temporary) / name
            root.mkdir()
            path, original = entry.fixture(host, root, count)
            sidecar, = root.rglob("patches.json")
            fixtures[name] = (path, original, sidecar, json.loads(sidecar.read_bytes()))
            if worker:
                previous = os.environ.get("LD_LIBRARY_PATH")
                os.environ["LD_LIBRARY_PATH"] = environment(snapshot)["LD_LIBRARY_PATH"]
                try:
                    client = clients.enter_context(bench.Client(str(snapshot / "bin/neverd-worker")))
                finally:
                    if previous is None:
                        os.environ.pop("LD_LIBRARY_PATH", None)
                    else:
                        os.environ["LD_LIBRARY_PATH"] = previous
                bench.verify_worker_sdk(client, snapshot / "bin/libneverd.so")
                reply = client.call("open", {"path": str(path), "analysis": False})
                assert reply["status"] == "ok", reply
                calls[name] = client.call
                clocks[name] = process_clock(client.process.pid)
            else:
                def call(*args, snapshot=snapshot, path=path):
                    reply = subprocess.run([str(snapshot / "bin/neverd"), "patch-bytes", str(path), *args],
                                           env=environment(snapshot), capture_output=True, timeout=120)
                    assert reply.returncode == 0, (args, reply.stdout, reply.stderr)
                    return reply.stdout
                calls[name] = call
                def child_cpu():
                    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
                    return (usage.ru_utime + usage.ru_stime) * 1000
                clocks[name] = child_cpu

        at = count // 2
        address = hex(bench.BASE + at)
        operations = ["worker_commit"] if worker else ["cli_list", "cli_preview", "cli_apply"]
        if worker and history:
            operations += ["worker_undo", "worker_redo"]
        results = []
        for operation in operations:
            values = {name: {"wall": [], "cpu": []} for name in snapshots}
            # One unrecorded, checked pair warms each operation's path.
            for sample in range(-1, samples):
                names = list(snapshots)
                if sample % 2:
                    names.reverse()
                for name in names:
                    call, clock = calls[name], clocks[name]
                    path, original, sidecar, expected = fixtures[name]
                    if operation in ("worker_undo", "worker_redo"):
                        applied = call("patch_apply", {"address": address, "kind": "bytes",
                                                       "bytes": "91", "expected": "90"})
                        assert applied["status"] == "ok" and applied["payload"]["saved"], applied
                        if operation == "worker_redo":
                            assert call("undo")["status"] == "ok"
                    cpu_start = clock()
                    start = time.perf_counter_ns()
                    if worker:
                        if operation == "worker_commit":
                            output = call("patch_apply", {"address": address, "kind": "bytes",
                                                          "bytes": "91", "expected": "90"})
                        else:
                            output = call(operation.removeprefix("worker_"))
                    else:
                        args = [] if operation == "cli_list" else ["--address=" + address, "--bytes=91"]
                        if operation == "cli_apply":
                            args.append("--apply")
                        output = call(*args)
                    elapsed = (time.perf_counter_ns() - start) / 1e6
                    cpu = clock() - cpu_start
                    if sample >= 0:
                        values[name]["wall"].append(elapsed)
                        values[name]["cpu"].append(cpu)
                    if worker:
                        assert output["status"] == "ok" and output["payload"]["saved"], output
                        current = "90" if operation == "worker_undo" else "91"
                        assert call("bytes", {"address": address, "size": 1})["payload"]["data"] == current
                        rows = json.loads(sidecar.read_bytes())
                        assert rows[at]["current"] == current
                        assert rows[:at] + rows[at+1:] == expected[:at] + expected[at+1:]
                        if operation != "worker_undo":
                            assert call("undo")["status"] == "ok"
                    elif operation == "cli_list":
                        assert json.loads(output) == expected
                    else:
                        plan = json.loads(output)
                        assert plan["original"] == "90" and plan["current"] == "91", plan
                        if operation == "cli_apply":
                            rows = json.loads(sidecar.read_bytes())
                            assert rows[at]["current"] == "91"
                            assert rows[:at] + rows[at+1:] == expected[:at] + expected[at+1:]
                            call("--address=" + address, "--bytes=90", "--apply")
                    assert json.loads(sidecar.read_bytes()) == expected
            results.append({"operation": operation, "rows": count,
                            "variants": {name: {metric: bench.summary(v) for metric, v in data.items()}
                                         for name, data in values.items()}})
        for name, call in calls.items():
            path, original, _, _ = fixtures[name]
            exported = path.parent / "export.bin"
            if worker:
                assert call("patch_export", {"path": str(exported)})["status"] == "ok"
            else:
                call("--export", "-o", str(exported))
            assert exported.read_bytes() == b"\x90" * count + original[count:]
            assert path.read_bytes() == original
        return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--cli-rows", type=int, nargs="*", default=[64, 8192, 65536])
    parser.add_argument("--worker-rows", type=int, nargs="*", default=[64, 8192])
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--worker-history", action="store_true",
                        help="also time checked durable undo and redo operations")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux" or not __debug__ or args.samples <= 0:
        parser.error("requires Linux, enabled assertions and positive samples")
    if any(n < 2 or n % 2 for n in args.cli_rows + args.worker_rows):
        parser.error("row counts must be even and at least two")
    snapshots = {"baseline": args.baseline.resolve(strict=True), "candidate": args.candidate.resolve(strict=True)}
    for snapshot in snapshots.values():
        trace = subprocess.run([str(snapshot / "bin/neverd")],
                               env={**environment(snapshot), "LD_TRACE_LOADED_OBJECTS": "1"},
                               capture_output=True, text=True, check=True)
        assert "libneverd.so => " + str(snapshot / "bin/libneverd.so") + " " in trace.stdout, trace.stdout
    host = bench.HostAPI(ctypes.CDLL(str(snapshots["baseline"] / "bin/libneverd.so")))
    report = {"platform": platform.platform(), "load_average": os.getloadavg(),
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "benchmark_sha256": bench.digest(__file__), "samples": args.samples,
              "snapshots": {name: {"path": str(path), "sha256": {
                  binary: bench.digest(path / "bin" / binary)
                  for binary in ("neverd", "neverd-worker", "libneverd.so")}}
                  for name, path in snapshots.items()}, "results": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for worker, counts in ((False, args.cli_rows), (True, args.worker_rows)):
        for count in counts:
            report["results"].extend(run_case(host, snapshots, count, args.samples, worker,
                                              args.worker_history))
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            print("checked", "worker" if worker else "CLI", count, flush=True)


if __name__ == "__main__":
    main()
