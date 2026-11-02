#!/usr/bin/env python3
"""Compare durable patch edits with a retained history of large commands.

Use explicit baseline/candidate binary snapshots and alternating paired samples.
Fixture setup, history population and independent byte checks are outside timing.
No guest code is executed. Run on Linux with Python assertions enabled.
"""

from contextlib import ExitStack
import argparse
import ctypes
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


def run_case(host, snapshots, size, commands, samples):
    with tempfile.TemporaryDirectory(prefix="neverd-patch-commands-") as temporary, ExitStack() as stack:
        clients, clocks, fixtures, current = {}, {}, {}, {}
        for name, snapshot in snapshots.items():
            root = Path(temporary) / name
            root.mkdir()
            path = root / "input.bin"
            original = b"\xcc" * (size + 16)
            path.write_bytes(original)
            with bench.session_for(host, path) as session:
                session.apply_byte_patch(bench.BASE, kind="bytes", data=b"\x90" * size)
                session.save_byte_patches()
                assert host.call("neverd_load_options_save", session._native_handle) == 0
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
            assert client.call("open", {"path": str(path), "analysis": False})["status"] == "ok"
            clients[name], clocks[name], current[name] = client, paired.process_clock(client.process.pid), "90"
            fixtures[name] = (path, original, next(root.rglob("patches.json")))

        def apply(name):
            after = "91" if current[name] == "90" else "90"
            reply = clients[name].call("patch_apply", {"address": hex(bench.BASE), "kind": "bytes",
                                                       "bytes": after * size, "expected": current[name] * size})
            assert reply["status"] == "ok" and reply["payload"]["saved"], reply
            current[name] = after

        def verify(name):
            path, original, sidecar = fixtures[name]
            rows = json.loads(sidecar.read_bytes())
            assert len(rows) == 1 and rows[0]["size"] == size
            assert rows[0]["original"] == "cc" * size and rows[0]["current"] == current[name] * size
            reply = clients[name].call("bytes", {"address": hex(bench.BASE), "size": size})
            assert reply["status"] == "ok" and reply["payload"]["data"] == current[name] * size
            assert path.read_bytes() == original

        for _ in range(commands):
            for name in snapshots:
                apply(name)
                verify(name)
        retained = {name: client.call("history")["payload"]["total"] for name, client in clients.items()}
        assert len(set(retained.values())) == 1, retained
        results = []
        for operation in ("commit", "undo", "redo"):
            values = {name: {"wall": [], "cpu": []} for name in snapshots}
            for sample in range(-1, samples):
                names = list(snapshots)
                if sample % 2:
                    names.reverse()
                for name in names:
                    client, clock = clients[name], clocks[name]
                    if operation != "commit":
                        before = current[name]
                        apply(name)
                        if operation == "redo":
                            assert client.call("undo")["status"] == "ok"
                    cpu_start, start = clock(), time.perf_counter_ns()
                    if operation == "commit":
                        apply(name)
                    else:
                        reply = client.call(operation)
                        assert reply["status"] == "ok" and reply["payload"]["saved"], reply
                    elapsed, cpu = (time.perf_counter_ns() - start) / 1e6, clock() - cpu_start
                    if sample >= 0:
                        values[name]["wall"].append(elapsed)
                        values[name]["cpu"].append(cpu)
                    if operation == "undo":
                        current[name] = before
                    verify(name)
            results.append({"operation": "worker_" + operation, "payload_bytes": size,
                            "retained_commands": retained,
                            "variants": {name: {metric: bench.summary(data) for metric, data in metrics.items()}
                                         for name, metrics in values.items()}})
        memory = {}
        for name, client in clients.items():
            path, original, _ = fixtures[name]
            exported = path.parent / "export.bin"
            assert client.call("patch_export", {"path": str(exported)})["status"] == "ok"
            assert exported.read_bytes() == bytes.fromhex(current[name]) * size + original[size:]
            for line in Path(f"/proc/{client.process.pid}/status").read_text().splitlines():
                if line.startswith("VmHWM:"):
                    memory[name] = int(line.split()[1])
        return results, {"payload_bytes": size, "peak_rss_kib": memory,
                         "scope": "Worker startup, warmup and all three operations; one process per variant"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--payload-bytes", type=int, nargs="+", default=[8192])
    parser.add_argument("--warmup-commands", type=int, default=72)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux" or not __debug__ or min(args.payload_bytes + [args.samples, args.warmup_commands]) <= 0:
        parser.error("requires Linux, enabled assertions and positive workload sizes")
    snapshots = {"baseline": args.baseline.resolve(strict=True), "candidate": args.candidate.resolve(strict=True)}
    host = bench.HostAPI(ctypes.CDLL(str(snapshots["baseline"] / "bin/libneverd.so")))
    report = {"platform": platform.platform(), "load_average": os.getloadavg(),
              "cpu_affinity": sorted(os.sched_getaffinity(0)),
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "benchmark_sha256": bench.digest(__file__), "samples": args.samples,
              "warmup_commands": args.warmup_commands,
              "snapshots": {name: {"path": str(path), "sha256": {
                  binary: bench.digest(path / "bin" / binary) for binary in ("neverd-worker", "libneverd.so")}}
                  for name, path in snapshots.items()}, "results": [], "memory": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for size in args.payload_bytes:
        results, memory = run_case(host, snapshots, size, args.warmup_commands, args.samples)
        report["results"].extend(results)
        report["memory"].append(memory)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print("checked command payload", size, flush=True)


if __name__ == "__main__":
    main()
