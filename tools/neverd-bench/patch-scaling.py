#!/usr/bin/env python3
"""Measure byte-patch edits and range queries against an explicit SDK/worker.

Fixtures are owned raw x64 images; no guest code is executed. Loading/seeding
and correctness checks are outside timed samples. CPU time accompanies SDK
wall time so concurrent build load is visible, not mistaken for patch work.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import sys
import tempfile
import time
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pluginsdk/python"))
sys.path.insert(0, str(ROOT / "tools/neverd-worker/tests"))

from neverd_plugin import Session
from neverd_plugin.ffi import HostAPI
from transport_test import Client

BASE = 0x1000


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def summary(samples):
    ordered = sorted(samples)
    return {"median_ms": statistics.median(ordered),
            "p95_ms": ordered[math.ceil(len(ordered) * 0.95) - 1],
            "samples_ms": samples}


def seed_rows(session, root, count):
    session.apply_byte_patch(BASE, kind="bytes", data=b"\x90")
    session.save_byte_patches()
    sidecar, = root.rglob("patches.json")
    rows = [{"addr": hex(i), "file_off": hex(i), "va": hex(BASE + i),
             "size": 1, "original": "cc", "current": "90",
             "kind": "nop" if i % 2 else "bytes"} for i in range(count)]
    sidecar.write_text(json.dumps(rows, separators=(",", ":")))


def verify_worker_sdk(client, library):
    # A benchmark must not silently compare two workers loading the same SDK
    # from their original build RUNPATH. Worker timing currently uses Linux.
    mappings = Path(f"/proc/{client.process.pid}/maps").read_text()
    if not any(line.endswith(" " + str(library)) for line in mappings.splitlines()):
        raise RuntimeError(f"worker did not load the selected SDK: {library}")


@contextmanager
def session_for(host, path):
    pointer = host.call("neverd_session_create")
    if not pointer:
        raise RuntimeError("session creation failed")
    try:
        options = json.dumps({"loader": "binary", "processor": "x86_64",
                              "platform": "sysv", "base": BASE}).encode()
        if host.call("neverd_session_set_load_options", pointer, options) != 0:
            raise RuntimeError(host.owned_string("neverd_last_error", pointer))
        if host.call("neverd_session_load", pointer, str(path).encode()) != 1:
            raise RuntimeError(host.owned_string("neverd_last_error", pointer))
        yield Session(pointer, _host=host,
                      _native=SimpleNamespace(session_address=lambda handle: handle))
    finally:
        host.call("neverd_session_destroy", pointer)


def edit_case(host, count, samples):
    with tempfile.TemporaryDirectory(prefix="neverd-patch-scaling-") as temporary:
        root = Path(temporary)
        path = root / "input.bin"
        original = b"\xcc" * (count + 16)
        path.write_bytes(original)
        with session_for(host, path) as session:
            seed_rows(session, root, count)
            at = count // 2
            wall, cpu = [], []
            for _ in range(samples):
                session.load_byte_patches()
                start = time.perf_counter_ns()
                start_cpu = time.process_time_ns()
                plan = session.apply_byte_patch(BASE + at, kind="bytes", data=b"\x91", expected=b"\x90")
                cpu.append((time.process_time_ns() - start_cpu) / 1e6)
                wall.append((time.perf_counter_ns() - start) / 1e6)
                assert plan["original"] == "90" and plan["current"] == "91", plan
            output = root / "export.bin"
            session.export_byte_patches(output)
            expected = bytearray(original)
            expected[:count] = b"\x90" * count
            expected[at] = 0x91
            assert output.read_bytes() == expected
            assert path.read_bytes() == original
        return {"operation": "sdk_single_byte_edit", "adjacent_rows": count,
                "wall": summary(wall), "cpu": summary(cpu)}


def range_case(host, worker, library, size, samples):
    with tempfile.TemporaryDirectory(prefix="neverd-patch-ranges-") as temporary:
        root = Path(temporary)
        path = root / "input.bin"
        path.write_bytes(b"\xcc" * (size + 16))
        with session_for(host, path) as session:
            session.apply_byte_patch(BASE, kind="bytes", data=b"\x90" * size)
            session.save_byte_patches()
        wall = []
        with Client(worker) as client:
            verify_worker_sdk(client, library)
            opened = client.call("open", {"path": str(path), "loader": "binary",
                                          "processor": "x86_64", "base": hex(BASE),
                                          "analysis": False})
            assert opened["status"] == "ok", opened
            for _ in range(samples):
                start = time.perf_counter_ns()
                reply = client.call("patches", {"ranges_only": True})
                wall.append((time.perf_counter_ns() - start) / 1e6)
                assert reply["status"] == "ok", reply
                assert reply["payload"]["items"] == [{"address": hex(BASE), "size": size}], reply
        return {"operation": "worker_patch_ranges", "changed_bytes": size,
                "wall": summary(wall)}


def worker_edit_case(host, worker, library, count, samples):
    with tempfile.TemporaryDirectory(prefix="neverd-patch-commit-") as temporary:
        root = Path(temporary)
        path = root / "input.bin"
        original = b"\xcc" * (count + 16)
        path.write_bytes(original)
        with session_for(host, path) as session:
            seed_rows(session, root, count)
        wall = []
        with Client(worker) as client:
            verify_worker_sdk(client, library)
            opened = client.call("open", {"path": str(path), "loader": "binary",
                                          "processor": "x86_64", "base": hex(BASE),
                                          "analysis": False})
            assert opened["status"] == "ok", opened
            at = count // 2
            for _ in range(samples):
                start = time.perf_counter_ns()
                reply = client.call("patch_apply", {"address": hex(BASE + at),
                                                    "kind": "bytes", "bytes": "91", "expected": "90"})
                wall.append((time.perf_counter_ns() - start) / 1e6)
                assert reply["status"] == "ok" and reply["payload"]["saved"], reply
                changed = client.call("bytes", {"address": hex(BASE + at), "size": 1})
                assert changed["status"] == "ok" and changed["payload"]["data"] == "91", changed
                assert client.call("undo")["status"] == "ok"
            output = root / "export.bin"
            assert client.call("patch_export", {"path": str(output)})["status"] == "ok"
            assert output.read_bytes() == b"\x90" * count + original[count:]
            assert path.read_bytes() == original
        return {"operation": "worker_single_byte_commit", "adjacent_rows": count,
                "wall": summary(wall)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--worker", type=Path)
    parser.add_argument("--worker-rows", type=int, nargs="*", default=[64, 1024, 2048])
    parser.add_argument("--rows", type=int, nargs="*", default=[64, 256, 512, 1024, 2048])
    parser.add_argument("--range-bytes", type=int, nargs="*", default=[4096, 262144, 1048576])
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not __debug__:
        parser.error("run without Python -O so fixture correctness is checked")
    if args.samples <= 0 or any(n <= 0 for n in args.rows + args.range_bytes + args.worker_rows):
        parser.error("sample counts and fixture sizes must be positive")
    if args.worker and sys.platform != "linux":
        parser.error("worker comparisons require Linux to verify SDK identity; omit --worker for SDK-only measurements")
    library = args.library.resolve(strict=True)
    worker = args.worker.resolve(strict=True) if args.worker else None
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Pin the worker's dynamic SDK before it starts; its build RUNPATH may
    # point at a different build than the explicitly selected comparison SDK.
    previous_path = os.environ.get("LD_LIBRARY_PATH", "")
    os.environ["LD_LIBRARY_PATH"] = str(library.parent) + (os.pathsep + previous_path if previous_path else "")
    host = HostAPI(ctypes.CDLL(str(library)))
    report = {"platform": platform.platform(), "machine": platform.machine(),
              "processor": platform.processor(),
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "benchmark_sha256": digest(__file__), "cpu_count": os.cpu_count(),
              "load_average": os.getloadavg() if hasattr(os, "getloadavg") else None,
              "library": str(library), "library_sha256": digest(library),
              "worker": str(worker) if worker else None,
              "worker_sha256": digest(worker) if worker else None,
              "samples": args.samples, "results": []}
    for count in args.rows:
        report["results"].append(edit_case(host, count, args.samples))
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report["results"][-1]), flush=True)
    if worker:
        for size in args.range_bytes:
            report["results"].append(range_case(host, str(worker), library, size, args.samples))
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            print(json.dumps(report["results"][-1]), flush=True)
        for count in args.worker_rows:
            report["results"].append(worker_edit_case(host, str(worker), library, count, args.samples))
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            print(json.dumps(report["results"][-1]), flush=True)


if __name__ == "__main__":
    main()
