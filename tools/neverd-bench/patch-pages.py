#!/usr/bin/env python3
"""Compare complete worker patch inventory requests against explicit binary snapshots.

Both snapshots contain bin/neverd-worker and bin/libneverd.so. Alternating
A/B and B/A samples include snapshot acquisition, projection, serialization
and transport. Fixtures and complete response/file oracles are outside timing.
"""

from contextlib import ExitStack
import argparse
import ctypes
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location(
    "paired", Path(__file__).with_name("patch-paired.py"))
paired = importlib.util.module_from_spec(spec)
spec.loader.exec_module(paired)
bench = paired.bench


def run_case(host, snapshots, count, samples, include_ranges=False):
    with tempfile.TemporaryDirectory(prefix="neverd-patch-pages-") as directory, ExitStack() as stack:
        clients, clocks, fixtures = {}, {}, {}
        for name, snapshot in snapshots.items():
            root = Path(directory) / name
            root.mkdir()
            path, original = paired.entry.fixture(host, root, count)
            sidecar, = root.rglob("patches.json")
            text = sidecar.read_bytes()
            expected = [dict(row, address=row["va"]) for row in json.loads(text)]
            fixtures[name] = (path, original, sidecar, text, sidecar.stat().st_mtime_ns, expected)
            previous = os.environ.get("LD_LIBRARY_PATH")
            os.environ["LD_LIBRARY_PATH"] = str(snapshot / "bin")
            try:
                client = stack.enter_context(bench.Client(str(snapshot / "bin/neverd-worker")))
            finally:
                if previous is None:
                    os.environ.pop("LD_LIBRARY_PATH", None)
                else:
                    os.environ["LD_LIBRARY_PATH"] = previous
            bench.verify_worker_sdk(client, snapshot / "bin/libneverd.so")
            opened = client.call("open", {"path": str(path), "analysis": False})
            assert opened["status"] == "ok", opened
            clients[name], clocks[name] = client, paired.process_clock(client.process.pid)
        results = []
        pages = [("first", 0, 128), ("middle", count // 2, 128),
                 ("tail", count - 1, 128), ("empty", count, 128), ("zero", 0, 0)]
        if include_ranges:
            pages.append(("ranges", None, None))
        for label, offset, limit in pages:
            request = {"ranges_only": True} if label == "ranges" else {"offset": offset, "limit": limit}
            values = {name: {"cpu": [], "wall": []} for name in snapshots}
            output_hashes = {}
            for sample in range(-1, samples):
                names = list(snapshots)
                if sample % 2:
                    names.reverse()
                for name in names:
                    path, original, sidecar, text, timestamp, expected = fixtures[name]
                    clock = clocks[name]
                    cpu_start, wall_start = clock(), time.perf_counter_ns()
                    reply = clients[name].call("patches", request)
                    wall = (time.perf_counter_ns() - wall_start) / 1e6
                    cpu = clock() - cpu_start
                    assert reply["status"] == "ok", reply
                    if label == "ranges":
                        items = [{"address": row["va"], "size": row["size"]} for row in expected]
                        wanted = {"items": items, "total": count, "complete": True}
                    else:
                        items = expected[offset:offset + limit]
                        wanted = {"items": items, "total": count, "offset": offset,
                                  "complete": offset + len(items) >= count}
                    assert reply["payload"] == wanted, reply
                    output_hashes[name] = hashlib.sha256(json.dumps(
                        reply["payload"], sort_keys=True, separators=(",", ":")).encode()).hexdigest()
                    assert sidecar.read_bytes() == text and sidecar.stat().st_mtime_ns == timestamp
                    assert path.read_bytes() == original
                    if sample >= 0:
                        values[name]["cpu"].append(cpu)
                        values[name]["wall"].append(wall)
            assert len(set(output_hashes.values())) == 1, output_hashes
            results.append({"rows": count, "page": label, "offset": offset, "limit": limit,
                            "output_sha256": output_hashes,
                            "variants": {name: {metric: bench.summary(data)
                                                 for metric, data in metrics.items()}
                                         for name, metrics in values.items()}})
        memory = {}
        for name, client in clients.items():
            status = Path(f"/proc/{client.process.pid}/status").read_text()
            memory[name] = {key: int(value.split()[0]) for line in status.splitlines()
                            if ":" in line for key, value in [line.split(":", 1)]
                            if key in ("VmRSS", "VmHWM")}
        return results, {"rows": count, "kib": memory,
                         "fixture_sha256": {name: {"input": bench.digest(fixture[0]),
                                                   "sidecar": bench.digest(fixture[2])}
                                            for name, fixture in fixtures.items()},
                         "scope": "Worker startup, SDK load and all checked inventory requests"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--rows", type=int, nargs="+", default=[64, 8192, 32768])
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--include-ranges", action="store_true",
                        help="also measure complete unpaged ranges_only responses")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux" or not __debug__ or min(args.rows + [args.samples]) <= 0:
        parser.error("requires Linux, enabled assertions and positive workload sizes")
    snapshots = {name: path.resolve(strict=True) for name, path in
                 (("baseline", args.baseline), ("candidate", args.candidate))}
    host = bench.HostAPI(ctypes.CDLL(str(snapshots["baseline"] / "bin/libneverd.so")))
    report = {"platform": platform.platform(), "load_average": os.getloadavg(),
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "cpu_affinity": sorted(os.sched_getaffinity(0)), "samples": args.samples,
              "include_ranges": args.include_ranges,
              "benchmark_sha256": bench.digest(__file__),
              "snapshots": {name: {"path": str(path), "sha256": {
                  binary: bench.digest(path / "bin" / binary)
                  for binary in ("neverd-worker", "libneverd.so")}}
                  for name, path in snapshots.items()}, "results": [], "memory": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for count in args.rows:
        results, memory = run_case(host, snapshots, count, args.samples, args.include_ranges)
        report["results"].extend(results)
        report["memory"].append(memory)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print("checked complete patch pages", count, flush=True)


if __name__ == "__main__":
    main()
