#!/usr/bin/env python3
"""Measure real worker round trips separately from synthetic viewport rendering."""
import argparse
import importlib.util
import json
import math
import platform
from pathlib import Path
import statistics
import subprocess
import tempfile
import time


def distribution(samples):
    ordered = sorted(samples)
    def percentile(p):
        return ordered[max(0, math.ceil(len(ordered) * p) - 1)]
    return dict(samples=len(samples), p50_ms=percentile(.5), p95_ms=percentile(.95),
                p99_ms=percentile(.99), min_ms=ordered[0], max_ms=ordered[-1], mean_ms=statistics.mean(samples))


def rss(pid):
    if platform.system() in ("Linux", "Darwin"):
        result = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True)
        if result.returncode == 0 and result.stdout.strip():
            return int(result.stdout.strip()) * 1024
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--binary", type=Path, help="Optional existing real PE/ELF/Mach-O or VM fixture")
    args = parser.parse_args()
    if not 10 <= args.samples <= 10000:
        parser.error("samples must be 10–10000")
    transport_path = Path(__file__).resolve().parents[2] / "neverd-worker" / "tests" / "transport_test.py"
    spec = importlib.util.spec_from_file_location("worker_benchmark_transport", transport_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    report = dict(schema=1, kind="real-worker-ipc", platform=platform.platform(), machine=platform.machine(),
                  python=platform.python_version(), samples=args.samples, clock="time.perf_counter_ns",
                  caveats=["Round-trip completion is not input-to-display latency.",
                           "RSS is resident process memory, not private/PSS; no child-process tree aggregation.",
                           "Default fixture is tiny EVM arithmetic; large native pipeline performance is not implied."])
    with tempfile.TemporaryDirectory(prefix="neverd-ipc-benchmark-") as directory:
        binary = args.binary
        if binary is None:
            binary = Path(directory) / "arithmetic.evm"
            binary.write_text("0x600160020100", encoding="ascii")
        start = time.perf_counter_ns()
        client = module.Client(str(args.worker.resolve()))
        report["startup_to_hello_ms"] = (time.perf_counter_ns() - start) / 1e6
        report["engine_version"] = client.hello["engine_version"]
        try:
            start = time.perf_counter_ns()
            opened = client.call("open", {"path": str(binary.resolve()), "read_only": True})
            report["open_to_metadata_ms"] = (time.perf_counter_ns() - start) / 1e6
            if opened["status"] != "ok":
                raise RuntimeError(opened)
            report["image"] = {key: opened["payload"][key] for key in ("architecture", "format", "file_size", "bitness")}
            report["rss_after_open_bytes"] = rss(client.process.pid)
            start = time.perf_counter_ns()
            analyzed = client.call("analyze")
            report["first_analysis_ms"] = (time.perf_counter_ns() - start) / 1e6
            if analyzed["status"] != "ok":
                raise RuntimeError(analyzed)
            functions = client.call("functions", {"limit": 1})
            address = functions["payload"]["items"][0]["address"] if functions["payload"]["items"] else opened["payload"]["entry_address"]
            cases = [("metadata", {}), ("functions", {"limit": 128}),
                     ("disasm", {"address": address, "limit": 128}),
                     ("bytes", {"address": address, "size": 4096}),
                     ("decompile", {"address": address, "representation": "c", "limit": 128}),
                     ("cfg", {"address": address})]
            report["operations"] = {}
            for operation, payload in cases:
                for _ in range(5):
                    warm = client.call(operation, payload)
                    if warm["status"] != "ok":
                        break
                if warm["status"] != "ok":
                    report["operations"][operation] = {"status": warm["status"], "error": warm.get("error")}
                    continue
                timings, sizes = [], []
                for _ in range(args.samples):
                    start = time.perf_counter_ns()
                    response = client.call(operation, payload)
                    timings.append((time.perf_counter_ns() - start) / 1e6)
                    if response["status"] != "ok":
                        raise RuntimeError(response)
                    sizes.append(len(json.dumps(response, ensure_ascii=False, separators=(",", ":")).encode()))
                report["operations"][operation] = dict(distribution(timings), response_json_bytes_max=max(sizes))
            report["rss_after_queries_bytes"] = rss(client.process.pid)
            report["acceptance"] = "recorded measurement only; no P0 acceptance decision"
        finally:
            client.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(args.output)


if __name__ == "__main__":
    main()
