#!/usr/bin/env python3
"""Measure A -> A -> B -> A in one production worker, reading all source pages."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import tempfile
import time

import ida_compare_bench as comparison


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--entry", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--switch-entry", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--representation", default="source")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    if args.threads < 1 or args.timeout <= 0:
        parser.error("threads and timeout must be positive")
    if args.entry < 0 or args.switch_entry < 0 or args.entry == args.switch_entry:
        parser.error("entry and switch-entry must be distinct nonnegative addresses")
    for field in ("worker", "engine", "binary"):
        setattr(args, field, getattr(args, field).resolve(strict=True))
    if platform.system() != "Linux" or args.engine.name != "libneverd.so":
        parser.error("this library-loading profile requires Linux/libneverd.so")
    library_path = os.environ.get("LD_LIBRARY_PATH")
    os.environ["LD_LIBRARY_PATH"] = str(args.engine.parent) + (
        os.pathsep + library_path if library_path else "")
    os.environ["NEVERD_THREADS"] = str(args.threads)
    os.environ["NEVERD_NATIVE_PHASES"] = "1"
    report = dict(schema=1, kind="single-worker-source-revisit",
                  started_at=datetime.now(timezone.utc).isoformat(),
                  platform=platform.platform(), threads=args.threads,
                  cpu_affinity=sorted(os.sched_getaffinity(0)),
                  representation=args.representation, steps=[],
                  caveats=["Warm OS caches; one fresh worker and input copy.",
                           "Background analysis disabled as in GUI analysis replicas.",
                           "Includes full source paging and IPC, excludes Qt painting and GUI caches.",
                           "Host load is observed, not controlled.",
                           "Source hashes check reproducibility, not native semantic equivalence."])
    for field in ("worker", "engine", "binary"):
        report[field + "_sha256"] = hashlib.sha256(getattr(args, field).read_bytes()).hexdigest()
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    with tempfile.TemporaryDirectory(prefix="neverd-revisit-") as directory:
        binary = Path(directory) / args.binary.name
        shutil.copyfile(args.binary, binary)
        client = comparison.load_transport().Client(str(args.worker))

        def call(operation, payload):
            return comparison.call(client, operation, payload, args.timeout)

        try:
            call("open", {"path": str(binary), "read_only": True, "analysis": False})
            loaded = sorted({line.split()[-1] for line in
                             Path(f"/proc/{client.process.pid}/maps").read_text().splitlines()
                             if line.split()[-1].endswith("/libneverd.so")})
            report["loaded_engine_paths"] = loaded
            if str(args.engine) not in loaded:
                raise RuntimeError(f"worker loaded a different engine: {loaded}")
            identities = {}
            for label, entry in (("A_first", args.entry), ("A_repeat", args.entry),
                                 ("B_first", args.switch_entry), ("A_revisit", args.entry)):
                row = dict(label=label, entry=f"0x{entry:x}", load_before=os.getloadavg())
                request = dict(address=row["entry"], representation=args.representation, limit=256)
                started = time.perf_counter()
                page = call("decompile", request)
                row["first_page_ms"] = (time.perf_counter() - started) * 1000
                total = page["total_lines"]
                if type(total) is not int or total <= 0:
                    raise RuntimeError("decompilation returned no source lines")
                digest, size = hashlib.sha256(), 0
                for offset in range(0, total, 256):
                    if offset:
                        page = call("decompile", {**request, "offset": offset})
                    if page["total_lines"] != total or page["offset"] != offset:
                        raise RuntimeError("source extent or offset changed during paging")
                    data = page["text"].encode("utf-8")
                    if not data:
                        raise RuntimeError("source page is empty before total_lines")
                    digest.update(data)
                    size += len(data)
                row.update(complete_source_ms=(time.perf_counter() - started) * 1000,
                           total_lines=total, source_bytes=size, source_sha256=digest.hexdigest(),
                           load_after=os.getloadavg())
                identity = (total, size, row["source_sha256"])
                if entry in identities and identities[entry] != identity:
                    raise RuntimeError("source changed when revisiting the same function")
                identities[entry] = identity
                report["steps"].append(row)
                save()
                print(json.dumps(row), flush=True)
            report["status"] = "ok"
        except (RuntimeError, TimeoutError, EOFError, OSError, KeyError) as error:
            report.update(status="error", error=str(error))
        finally:
            report["peak_rss_bytes"] = comparison.peak_rss_bytes(client.process.pid)
            client.process.kill()
            client.process.wait()
            client.stderr.join(timeout=2)
            report["phase_trace"] = [line for line in
                                     client.logs.decode("utf-8", errors="replace").splitlines()
                                     if line.startswith("[neverd-pipeline-stage]")]
            save()
    return int(report["status"] != "ok")


if __name__ == "__main__":
    raise SystemExit(main())
