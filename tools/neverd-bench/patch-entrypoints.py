#!/usr/bin/env python3
"""Measure patch JSON and complete CLI commands against an explicit SDK.

Uses owned raw x64 fixtures, never executes guest bytes. Setup, restoration,
JSON parsing and byte oracles are outside timing. CLI samples include process
startup, input/sidecar loading, the requested operation and captured output.
Run on the same filesystem in A/B/B/A order without overlapping own builds.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import time

spec = importlib.util.spec_from_file_location(
    "patch_scaling", Path(__file__).with_name("patch-scaling.py"))
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def measure(call):
    start_cpu = time.process_time_ns()
    start = time.perf_counter_ns()
    value = call()
    return value, (time.perf_counter_ns() - start) / 1e6, (
        time.process_time_ns() - start_cpu) / 1e6


def fixture(host, root, count):
    path = root / "input.bin"
    original = b"\xcc" * (count + 16)
    path.write_bytes(original)
    with bench.session_for(host, path) as session:
        bench.seed_rows(session, root, count)
        assert host.call("neverd_load_options_save", session._native_handle) == 0
        session.load_byte_patches()
        session.save_byte_patches()
    return path, original


def sdk_case(host, count, samples):
    with tempfile.TemporaryDirectory(prefix="neverd-patch-json-") as temporary:
        root = Path(temporary)
        path, original = fixture(host, root, count)
        sidecar, = root.rglob("patches.json")
        expected = sidecar.read_bytes()
        expected_rows = json.loads(expected)
        results = []
        with bench.session_for(host, path) as session:
            for name, call in (
                ("sdk_rows_json", lambda: host.owned_string(
                    "neverd_byte_patches_json", session._native_handle)),
                ("sdk_save", session.save_byte_patches),
                ("sdk_reload", session.load_byte_patches),
            ):
                wall, cpu = [], []
                for _ in range(samples):
                    value, elapsed, work = measure(call)
                    wall.append(elapsed)
                    cpu.append(work)
                    if name == "sdk_rows_json":
                        assert json.loads(value) == expected_rows
                    else:
                        assert sidecar.read_bytes() == expected
                results.append({"operation": name, "rows": count,
                                "wall": bench.summary(wall),
                                "cpu": bench.summary(cpu)})
            exported = root / "sdk-export.bin"
            session.export_byte_patches(exported)
            assert exported.read_bytes() == b"\x90" * count + original[count:]
        assert path.read_bytes() == original
        return results


def cli_case(host, cli, count, samples):
    import resource

    with tempfile.TemporaryDirectory(prefix="neverd-patch-cli-") as temporary:
        root = Path(temporary)
        path, original = fixture(host, root, count)
        sidecar, = root.rglob("patches.json")
        expected_rows = json.loads(sidecar.read_bytes())
        at = count // 2
        address = f"--address={hex(bench.BASE + at)}"

        def run(*args):
            result = subprocess.run([str(cli), "patch-bytes", str(path), *args],
                                    capture_output=True, timeout=120)
            assert result.returncode == 0, (args, result.stdout, result.stderr)
            return result.stdout

        results = []
        for name, args in (("cli_list", ()),
                           ("cli_preview", (address, "--bytes=91")),
                           ("cli_apply", (address, "--bytes=91", "--apply"))):
            wall, cpu = [], []
            for _ in range(samples):
                before = resource.getrusage(resource.RUSAGE_CHILDREN)
                output, elapsed, _ = measure(lambda: run(*args))
                after = resource.getrusage(resource.RUSAGE_CHILDREN)
                wall.append(elapsed)
                cpu.append(1000 * (after.ru_utime + after.ru_stime -
                                   before.ru_utime - before.ru_stime))
                if name == "cli_list":
                    assert json.loads(output) == expected_rows
                else:
                    plan = json.loads(output)
                    assert plan["original"] == "90" and plan["current"] == "91", plan
                if name == "cli_apply":
                    rows = json.loads(sidecar.read_bytes())
                    assert rows[at]["current"] == "91"
                    assert rows[:at] + rows[at+1:] == expected_rows[:at] + expected_rows[at+1:]
                    run(address, "--bytes=90", "--apply")
                assert json.loads(sidecar.read_bytes()) == expected_rows
            results.append({"operation": name, "rows": count,
                            "wall": bench.summary(wall),
                            "cpu": bench.summary(cpu)})
        exported = root / "cli-export.bin"
        run("--export", "-o", str(exported))
        assert exported.read_bytes() == b"\x90" * count + original[count:]
        assert path.read_bytes() == original
        return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--cli", type=Path)
    parser.add_argument("--rows", type=int, nargs="+", default=[64, 8192, 65536])
    parser.add_argument("--samples", type=int, default=15)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not __debug__ or args.samples <= 0 or any(n < 2 or n % 2 for n in args.rows):
        parser.error("enable assertions, positive samples and even row counts >= 2")
    if args.cli and sys.platform != "linux":
        parser.error("CLI library identity verification currently requires Linux")
    library = args.library.resolve(strict=True)
    cli = args.cli.resolve(strict=True) if args.cli else None
    os.environ["LD_LIBRARY_PATH"] = str(library.parent) + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")
    if cli:
        trace = subprocess.run([str(cli)], env={**os.environ, "LD_TRACE_LOADED_OBJECTS": "1"},
                               capture_output=True, text=True, check=True)
        assert any("libneverd.so => " + str(library) + " " in line
                   for line in trace.stdout.splitlines()), trace.stdout
    host = bench.HostAPI(ctypes.CDLL(str(library)))
    report = {"platform": platform.platform(), "machine": platform.machine(),
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "cpu_count": os.cpu_count(), "load_average": os.getloadavg(),
              "benchmark_sha256": bench.digest(__file__),
              "library": str(library), "library_sha256": bench.digest(library),
              "cli": str(cli) if cli else None,
              "cli_sha256": bench.digest(cli) if cli else None,
              "samples": args.samples, "results": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for count in args.rows:
        report["results"].extend(sdk_case(host, count, args.samples))
        if cli:
            report["results"].extend(cli_case(host, cli, count, args.samples))
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(f"checked {count} rows", flush=True)


if __name__ == "__main__":
    main()
