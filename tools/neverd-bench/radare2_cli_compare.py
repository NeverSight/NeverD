#!/usr/bin/env python3
"""Compare fresh NeverD/radare2 CLI reads against exact file-backed bytes.

Linux only. This deliberately starts with one narrowly comparable operation:
opening an image and returning an entire mapped byte range as JSON. It does
not qualify function discovery, decompilation, GUI painting or cold OS caches.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time


def identity(path):
    path = Path(path).resolve(strict=True)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "bytes": path.stat().st_size,
            "sha256": digest.hexdigest()}


def dependencies(executable, environment):
    result = subprocess.run(["ldd", str(executable)], env=environment,
                            capture_output=True, text=True, timeout=30, check=True)
    if "not found" in result.stdout:
        raise RuntimeError(result.stdout)
    paths = {}
    for line in result.stdout.splitlines():
        match = re.match(r"\s*(\S+) => (/.*?) \(0x[0-9a-f]+\)", line)
        if match:
            paths[match[1]] = identity(match[2])
    return paths


def integer(value):
    if type(value) is int:
        return value
    if isinstance(value, str):
        return int(value, 0)
    raise ValueError("expected an integer or a decimal/hexadecimal string")


def load_cases(manifest):
    document = json.loads(manifest.read_text())
    if document.get("schema") != 1 or not document.get("cases"):
        raise ValueError("manifest must have schema 1 and nonempty cases")
    cases = []
    names = set()
    for item in document["cases"]:
        name = item["name"]
        if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", name):
            raise ValueError("case names must contain only letters, digits, _ or -")
        if name in names:
            raise ValueError(f"duplicate case: {name}")
        names.add(name)
        binary = (manifest.parent / item["input"]).resolve(strict=True)
        artifact = identity(binary)
        if artifact["sha256"] != item["input_sha256"]:
            raise ValueError(f"{name}: input hash differs from manifest")
        address = integer(item["address"])
        offset = integer(item["file_offset"])
        size = integer(item["size"])
        if not (0 <= address < 2**64 and 0 < size <= 2**31 - 1
                and address + size <= 2**64 and 0 <= offset
                and offset + size <= artifact["bytes"]):
            raise ValueError(f"{name}: invalid file-backed range")
        with binary.open("rb") as stream:
            stream.seek(offset)
            expected = stream.read(size)
        cases.append({"name": name, "input": artifact, "address": address,
                      "file_offset": offset, "size": size, "expected": expected})
    return cases


def validate_result(tool, content, case):
    value = json.loads(content)
    if tool == "neverd":
        if not isinstance(value, dict) or integer(value.get("addr")) != case["address"]:
            raise ValueError("NeverD returned a different address")
        if type(value.get("size")) is not int or value["size"] != case["size"]:
            raise ValueError("NeverD returned an incomplete byte range")
        data = bytes.fromhex(value["bytes"])
    else:
        if not isinstance(value, list) or any(type(x) is not int or not 0 <= x < 256
                                              for x in value):
            raise ValueError("radare2 did not return a JSON byte array")
        data = bytes(value)
    if data != case["expected"]:
        raise ValueError(f"{tool} returned wrong or incomplete bytes")
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def measure(command, environment, directory, timeout):
    """wait4 supplies a blocking exit observation without polling sleeps."""
    process = None
    cleanup_group = True
    old_handler = signal.getsignal(signal.SIGALRM)
    if signal.getitimer(signal.ITIMER_REAL) != (0.0, 0.0):
        raise RuntimeError("an existing real-time alarm must not be replaced")

    def expired(_signal, _frame):
        raise TimeoutError(f"CLI exceeded {timeout:g} seconds")

    directory.mkdir()
    try:
        signal.signal(signal.SIGALRM, expired)
        with (directory / "stdout").open("wb") as stdout, \
             (directory / "stderr").open("wb") as stderr:
            signal.setitimer(signal.ITIMER_REAL, timeout)
            started = time.perf_counter()
            process = subprocess.Popen(command, env=environment, stdout=stdout,
                                       stderr=stderr, start_new_session=True)
            _, status, usage = os.wait4(process.pid, 0)
            elapsed_ms = (time.perf_counter() - started) * 1000
            process.returncode = os.waitstatus_to_exitcode(status)
            signal.setitimer(signal.ITIMER_REAL, 0)
        if process.returncode:
            raise RuntimeError(f"CLI exited {process.returncode}; see {directory / 'stderr'}")
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            cleanup_group = False
        else:
            raise RuntimeError("CLI left descendants running after its exit")
        return {"command": command, "wall_ms": elapsed_ms,
                "user_ms": usage.ru_utime * 1000, "system_ms": usage.ru_stime * 1000,
                "wait4_max_rss_kb": usage.ru_maxrss,
                "stdout": identity(directory / "stdout"),
                "stderr": identity(directory / "stderr")}
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        if process is not None and cleanup_group:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            if process.returncode is None:
                process.wait()
        signal.signal(signal.SIGALRM, old_handler)


def command_for(tool, executable, binary, case):
    if tool == "neverd":
        return [str(executable), "hex", "--json", "--addr", hex(case["address"]),
                "--size", str(case["size"]), str(binary)]
    return [str(executable), "-N", "-q", "-e", "scr.color=0", "-c",
            f"pxj {case['size']} @ {case['address']}", str(binary)]


def summarize(records):
    by_case = {}
    for record in records:
        if not record["warmup"]:
            by_case.setdefault(record["case"], []).append(record)
    summaries = {}
    for name, pairs in by_case.items():
        timings = {tool: [pair["tools"][tool]["wall_ms"] for pair in pairs]
                   for tool in ("neverd", "radare2")}
        medians = {tool: statistics.median(values) for tool, values in timings.items()}
        summaries[name] = {"pairs": len(pairs), "wall_median_ms": medians,
                           "neverd_over_radare2": medians["neverd"] / medians["radare2"],
                           "neverd_faster_pairs": sum(n < r for n, r in
                               zip(timings["neverd"], timings["radare2"]))}
    return summaries


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--engine", type=Path, required=True)
    parser.add_argument("--radare2-prefix", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--radare2-revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--diagnostic", action="store_true",
                        help="Mark development runs as ineligible for timing claims")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("this profile requires Linux ldd, wait4 and CPU affinity")
    if (args.samples < 1 or args.warmup < 0 or args.threads < 1
            or not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("samples, threads and timeout must be positive; warmup nonnegative")
    manifest = args.manifest.resolve(strict=True)
    cases = load_cases(manifest)
    engine = args.engine.resolve(strict=True)
    prefix = args.radare2_prefix.resolve(strict=True)
    executables = {"neverd": args.neverd.resolve(strict=True),
                   "radare2": (prefix / "bin/radare2").resolve(strict=True)}
    environments = {tool: os.environ.copy() for tool in executables}
    for env in environments.values():
        env.pop("LD_PRELOAD", None)
    environments["neverd"].update(LD_LIBRARY_PATH=str(engine.parent),
                                 NEVERD_SIGNATURE_CACHE="off", NEVERD_THREADS=str(args.threads))
    environments["radare2"].update(LD_LIBRARY_PATH=str(prefix / "lib"), R2_NOPLUGINS="1",
                                  R2_LIBR_PLUGINS=str(prefix / "lib/radare2"))
    environment_keys = ("LANG", "LC_ALL", "LD_LIBRARY_PATH", "LD_AUDIT",
                        "GLIBC_TUNABLES", "MALLOC_ARENA_MAX", "MALLOC_CONF",
                        "NEVERD_THREADS", "NEVERD_SIGNATURE_CACHE", "R2_NOPLUGINS",
                        "R2_LIBR_PLUGINS")
    artifacts = {tool: {"executable": identity(path),
                        "dependencies": dependencies(path, environments[tool])}
                 for tool, path in executables.items()}
    if artifacts["neverd"]["dependencies"].get("libneverd.so", {}).get("path") != str(engine):
        raise RuntimeError("the CLI does not resolve the requested engine")
    for name, item in artifacts["radare2"]["dependencies"].items():
        if name.startswith("libr_") and not Path(item["path"]).is_relative_to(prefix):
            raise RuntimeError(f"radare2 selected a library outside its prefix: {name}")
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    report = {"schema": 1, "status": "running", "operation": "fresh-cli-mapped-bytes",
              "timing_qualification": not args.diagnostic,
              "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "platform": platform.platform(), "affinity": sorted(os.sched_getaffinity(0)),
              "environment": {tool: {key: value for key, value in env.items()
                                      if key in environment_keys or key.startswith("MIMALLOC_")}
                              for tool, env in environments.items()},
              "source_revision": args.source_revision, "radare2_revision": args.radare2_revision,
              "artifacts": artifacts, "manifest": identity(manifest),
              "neverd_threads": args.threads, "signature_disk_cache": "off",
              "radare2_external_plugins": False, "samples": args.samples, "warmup": args.warmup,
              "scope": "Process launch through exit and complete JSON byte output; warm OS caches.",
              "memory_scope": "wait4 peak RSS, not summed process-tree RSS/PSS.",
              "not_qualified": ["cold OS caches", "analysis", "decompilation", "GUI", "native other hosts"],
              "cases": [{k: v for k, v in case.items() if k != "expected"} for case in cases],
              "records": []}
    report_path = output / "assessment.json"
    save(report_path, report)
    try:
        for case in cases:
            for index in range(-args.warmup, args.samples):
                order = ["neverd", "radare2"] if index % 2 == 0 else ["radare2", "neverd"]
                pair = {"case": case["name"], "index": index, "warmup": index < 0,
                        "order": order, "load_start": os.getloadavg(), "tools": {}}
                report["records"].append(pair)
                with tempfile.TemporaryDirectory(prefix="cli-pair-", dir=output) as temporary:
                    binary = Path(temporary) / Path(case["input"]["path"]).name
                    shutil.copyfile(case["input"]["path"], binary)
                    for tool in order:
                        directory = output / f"{case['name']}-{index}-{tool}"
                        result = measure(command_for(tool, executables[tool], binary, case),
                                         environments[tool], directory, args.timeout)
                        result["validated"] = validate_result(tool, (directory / "stdout").read_bytes(), case)
                        pair["tools"][tool] = result
                    if identity(binary)["sha256"] != case["input"]["sha256"]:
                        raise RuntimeError("the copied input changed during measurement")
                pair["load_end"] = os.getloadavg()
                save(report_path, report)
                print(case["name"], index, {t: round(r["wall_ms"], 3)
                                          for t, r in pair["tools"].items()}, flush=True)
        for tool, paths in artifacts.items():
            for item in [paths["executable"], *paths["dependencies"].values()]:
                if identity(item["path"]) != item:
                    raise RuntimeError(f"{tool} artifact changed during measurement")
            if dependencies(executables[tool], environments[tool]) != paths["dependencies"]:
                raise RuntimeError(f"{tool} library resolution changed during measurement")
        for case in cases:
            if identity(case["input"]["path"]) != case["input"]:
                raise RuntimeError("input changed during measurement")
        if identity(manifest) != report["manifest"]:
            raise RuntimeError("manifest changed during measurement")
        report["summary"] = summarize(report["records"])
        report["status"] = "complete"
    except Exception as error:
        report["status"] = "failed"
        report["timing_qualification"] = False
        report["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        save(report_path, report)


if __name__ == "__main__":
    main()
