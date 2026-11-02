#!/usr/bin/env python3
"""Compare JSON output for a large merged patch with alternating native samples.

Example: patch-payload.py --baseline /path/before --candidate /path/after
  --output /path/report.json. Snapshots contain bin/neverd and bin/libneverd.so.
Owned raw x64 fixtures are never executed. Setup, admission, JSON/byte checks
and input preservation are outside timing; CLI timing includes the full process.
"""

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
    "entry", Path(__file__).with_name("patch-entrypoints.py"))
entry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--bytes", type=int, default=1024 * 1024)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux" or not __debug__ or min(args.bytes, args.samples) <= 0:
        parser.error("requires Linux, enabled assertions and positive bytes/samples")
    snapshots = {v: getattr(args, v).resolve(strict=True)
                 for v in ("baseline", "candidate")}
    for snapshot in snapshots.values():
        trace = subprocess.run(["ldd", str(snapshot / "bin/neverd")],
                               env={**os.environ, "LD_LIBRARY_PATH": str(snapshot / "bin")},
                               capture_output=True, text=True, check=True)
        if str(snapshot / "bin/libneverd.so") not in trace.stdout:
            parser.error(f"CLI does not resolve the selected SDK: {snapshot}")
    hosts = {v: entry.bench.HostAPI(ctypes.CDLL(str(p / "bin/libneverd.so")))
             for v, p in snapshots.items()}
    report = {"samples": args.samples, "bytes": args.bytes,
              "platform": platform.platform(), "load_average": os.getloadavg(),
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "benchmark_sha256": entry.bench.digest(__file__),
              "snapshots": {v: {"path": str(p), "sha256": {
                  n: entry.bench.digest(p / "bin" / n)
                  for n in ("neverd", "libneverd.so")}} for v, p in snapshots.items()},
              "results": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="neverd-patch-payload-") as temporary:
        files = {}
        original = b"\xcc" * (args.bytes + 16)
        for variant, host in hosts.items():
            folder = Path(temporary) / variant
            folder.mkdir()
            path = folder / "input.bin"
            path.write_bytes(original)
            with entry.bench.session_for(host, path) as session:
                session.apply_byte_patch(entry.bench.BASE, data=b"\x90" * args.bytes)
                session.save_byte_patches()
                assert host.call("neverd_load_options_save", session._native_handle) == 0
            files[variant] = path
        for operation in ("sdk_json", "cli_list"):
            values = {v: {"cpu": [], "wall": []} for v in snapshots}
            for repeat in range(-1, args.samples):
                names = list(snapshots)
                if repeat % 2:
                    names.reverse()
                for variant in names:
                    path, host = files[variant], hosts[variant]
                    if operation == "sdk_json":
                        with entry.bench.session_for(host, path) as session:
                            result, elapsed, used = entry.measure(lambda: host.owned_string(
                                "neverd_byte_patches_json", session._native_handle))
                    else:
                        env = {**os.environ, "LD_LIBRARY_PATH": str(snapshots[variant] / "bin")}
                        before = resource.getrusage(resource.RUSAGE_CHILDREN)
                        start = time.perf_counter_ns()
                        run = subprocess.run([str(snapshots[variant] / "bin/neverd"),
                                              "patch-bytes", str(path)],
                                             env=env, capture_output=True, check=True, timeout=120)
                        elapsed = (time.perf_counter_ns() - start) / 1e6
                        after = resource.getrusage(resource.RUSAGE_CHILDREN)
                        used = ((after.ru_utime + after.ru_stime) -
                                (before.ru_utime + before.ru_stime)) * 1000
                        result = run.stdout
                    rows = json.loads(result)
                    assert len(rows) == 1 and rows[0]["size"] == args.bytes
                    assert rows[0]["original"] == "cc" * args.bytes
                    assert rows[0]["current"] == "90" * args.bytes
                    assert rows[0]["addr"] == rows[0]["file_off"] == "0x0"
                    assert rows[0]["va"] == hex(entry.bench.BASE) and rows[0]["kind"] == "bytes"
                    if repeat >= 0:
                        values[variant]["cpu"].append(used)
                        values[variant]["wall"].append(elapsed)
            report["results"].append({"operation": operation, "variants": {
                v: {k: entry.bench.summary(x) for k, x in data.items()}
                for v, data in values.items()}})
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            print("checked", operation, args.bytes, flush=True)
        for path in files.values():
            assert path.read_bytes() == original


if __name__ == "__main__":
    main()
