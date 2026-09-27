#!/usr/bin/env python3
"""Generate deterministic Android inventory fixtures and benchmark fresh processes.

DEX layout follows https://source.android.com/docs/core/runtime/dex-format.
These synthetic APK containers are inventory inputs, not installable apps.
Timing is accepted only after every process returns the expected inventory.
No source implementation is used to derive the expected class descriptors.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
import zlib


ROOT = Path(__file__).resolve().parents[1]
NO_INDEX = 0xFFFFFFFF
WORKLOAD_NAMES = ("dex", "single-stored", "multidex-stored", "single-deflated", "multidex-deflated")


def uleb128(value: int) -> bytes:
    if not 0 <= value <= NO_INDEX:
        raise ValueError("ULEB128 value is outside the DEX uint32 domain")
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    result.append(value)
    return bytes(result)


def class_descriptors(count: int, dex_index: int = 0) -> list[str]:
    return sorted(f"L{'bench' if i % 2 == 0 else 'other'}/d{dex_index:03d}/C{i:06d};"
                  for i in range(count))


def make_dex(class_count: int, *, dex_index: int = 0,
             extra_string_bytes: int = 0, code_units: int = 0) -> bytes:
    """Emit DEX035 with mapped strings, class definitions, and optional methods.

    Each optional public static void method contains code_units - 1 NOPs and a
    return-void. Extra payload consists of real, unreferenced string_data items.
    """
    if not 1 <= class_count <= 65533:
        raise ValueError("class_count must be between 1 and 65533 per DEX")
    if extra_string_bytes < 0 or code_units < 0:
        raise ValueError("payload sizes must be nonnegative")
    descriptors = class_descriptors(class_count, dex_index)
    types = sorted(descriptors + ["Ljava/lang/Object;"] + (["V"] if code_units else []))
    strings = types + (["run"] if code_units else [])
    # Each unique ASCII string has a bounded payload and a separate string ID.
    for index, start in enumerate(range(0, extra_string_bytes, 4096)):
        strings.append(f"payload{index:08d}:" + "x" * min(4096, extra_string_bytes - start))
    strings.sort()
    string_ids = {value: index for index, value in enumerate(strings)}
    type_ids = {value: index for index, value in enumerate(types)}
    data = bytearray(112)
    sections = [(0, 1, 0)]

    def table(kind: int, count: int, width: int) -> int:
        if not count:
            return 0
        offset = len(data)
        data.extend(bytes(count * width))
        sections.append((kind, count, offset))
        return offset

    string_off = table(1, len(strings), 4)
    type_off = table(2, len(types), 4)
    proto_off = table(3, int(bool(code_units)), 12)
    method_off = table(5, class_count if code_units else 0, 8)
    class_off = table(6, class_count, 32)
    data_off = len(data)
    sections.append((0x2002, len(strings), len(data)))
    for index, value in enumerate(strings):
        struct.pack_into("<I", data, string_off + index * 4, len(data))
        data.extend(uleb128(len(value)) + value.encode("ascii") + b"\0")
    for index, value in enumerate(types):
        struct.pack_into("<I", data, type_off + index * 4, string_ids[value])

    code_offsets, class_data_offsets = [], []
    if code_units:
        struct.pack_into("<III", data, proto_off, string_ids["V"], type_ids["V"], 0)
        instructions = bytes((code_units - 1) * 2) + b"\x0e\0"
        for index, descriptor in enumerate(descriptors):
            data.extend(bytes(-len(data) % 4))
            code_offsets.append(len(data))
            data.extend(struct.pack("<HHHHII", 0, 0, 0, 0, 0, code_units))
            data.extend(instructions)
            struct.pack_into("<HHI", data, method_off + index * 8,
                             type_ids[descriptor], 0, string_ids["run"])
        sections.append((0x2001, class_count, code_offsets[0]))
        for index, offset in enumerate(code_offsets):
            class_data_offsets.append(len(data))
            data.extend(b"\0\0\x01\0" + uleb128(index) + uleb128(9) + uleb128(offset))
        sections.append((0x2000, class_count, class_data_offsets[0]))

    for index, descriptor in enumerate(descriptors):
        struct.pack_into("<8I", data, class_off + index * 32,
                         type_ids[descriptor], 1, type_ids["Ljava/lang/Object;"],
                         0, NO_INDEX, 0, class_data_offsets[index] if code_units else 0, 0)
    data.extend(bytes(-len(data) % 4))
    map_off = len(data)
    sections.append((0x1000, 1, map_off))
    data.extend(struct.pack("<I", len(sections)))
    for kind, count, offset in sorted(sections, key=lambda entry: entry[2]):
        data.extend(struct.pack("<HHII", kind, 0, count, offset))
    data[:8] = b"dex\n035\0"
    struct.pack_into("<20I", data, 32, len(data), 112, 0x12345678, 0, 0, map_off,
                     len(strings), string_off, len(types), type_off,
                     int(bool(code_units)), proto_off, 0, 0,
                     class_count if code_units else 0, method_off,
                     class_count, class_off, len(data) - data_off, data_off)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & NO_INDEX)
    return bytes(data)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_apk(path: Path, dexes: list[bytes], compression: int, resource: bytes) -> None:
    with zipfile.ZipFile(path, "w", compression=compression, compresslevel=6) as archive:
        entries = [("classes.dex" if i == 0 else f"classes{i + 1}.dex", dex)
                   for i, dex in enumerate(dexes)]
        if resource:
            entries.append(("assets/unrelated.bin", resource))
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=(2000, 1, 1, 0, 0, 0))
            info.compress_type = compression
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data, compresslevel=6)


def generate_workloads(output_dir: Path, *, class_count: int = 1000,
                       dex_count: int = 3, extra_string_bytes: int = 0,
                       code_units: int = 0, resource_bytes: int = 0,
                       class_prefix: str = "", selected_workloads: list[str] | None = None) -> dict:
    if not 2 <= dex_count <= 64:
        raise ValueError("dex_count must be between 2 and 64")
    if not 0 <= resource_bytes <= 1024 ** 3:
        raise ValueError("resource_bytes must be between 0 and 1 GiB")
    selected = set(WORKLOAD_NAMES if selected_workloads is None else selected_workloads)
    if not selected or not selected <= set(WORKLOAD_NAMES):
        raise ValueError("select at least one known workload")
    # Exclusive creation prevents a failed or generation-only run from leaving
    # an older timing report beside a replacement manifest and input files.
    output_dir.mkdir(parents=True, exist_ok=False)
    dexes = [make_dex(class_count, dex_index=i, extra_string_bytes=extra_string_bytes,
                     code_units=code_units) for i in range(dex_count)]
    resource = hashlib.shake_256(b"NeverD mobile inventory resource v1").digest(resource_bytes)
    workloads = []
    if "dex" in selected:
        (output_dir / "classes.dex").write_bytes(dexes[0])
        workloads.append(("dex", "classes.dex", 1))
    for label, compression in (("stored", zipfile.ZIP_STORED), ("deflated", zipfile.ZIP_DEFLATED)):
        for prefix, count in (("single", 1), ("multidex", dex_count)):
            workload = f"{prefix}-{label}"
            if workload not in selected:
                continue
            name = f"{workload}.apk"
            write_apk(output_dir / name, dexes[:count], compression, resource)
            workloads.append((workload, name, count))
    manifest = {
        "schema_version": 1,
        "generator_sha256": sha256_file(Path(__file__).resolve()),
        "selected_workloads": [name for name in WORKLOAD_NAMES if name in selected],
        "parameters": {"class_count_per_dex": class_count, "dex_count": dex_count,
                       "extra_string_bytes_per_dex": extra_string_bytes,
                       "code_units_per_method": code_units,
                       "resource_bytes_per_apk": resource_bytes, "class_prefix": class_prefix},
        "workloads": [],
    }
    for label, name, count in workloads:
        expected = sorted(descriptor for i in range(count)
                          for descriptor in class_descriptors(class_count, i)
                          if descriptor.startswith(class_prefix))
        expected_name = f"{label}.expected.txt"
        (output_dir / expected_name).write_text("".join(value + "\n" for value in expected), encoding="utf-8")
        path = output_dir / name
        manifest["workloads"].append({"name": label, "input": name, "size_bytes": path.stat().st_size,
                                      "sha256": sha256_file(path), "expected": expected_name,
                                      "expected_sha256": sha256_file(output_dir / expected_name),
                                      "class_count": len(expected)})
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def validate_output(stdout: bytes, expected: list[str]) -> None:
    try:
        actual = stdout.decode("utf-8").splitlines()
    except UnicodeDecodeError as error:
        raise RuntimeError("inventory stdout is not UTF-8") from error
    # Inventory ordering can differ across tools. Multiplicity and every line
    # must still agree: banners, empty lines, and duplicates are failures.
    if sorted(actual) != expected:
        raise RuntimeError(f"inventory mismatch: expected {len(expected)} descriptors, got {len(actual)} lines")


def run_once(command: list[str], expected: list[str], *, timeout: float,
             measure_rss: bool = True) -> dict:
    if not command or timeout <= 0:
        raise ValueError("a command and positive timeout are required")
    with tempfile.TemporaryDirectory(prefix="neverd-inventory-run-") as temporary:
        rss_path = Path(temporary) / "rss.txt"
        time_tool = Path("/usr/bin/time")
        use_time = measure_rss and sys.platform.startswith("linux") and time_tool.is_file()
        argv = ([str(time_tool), "-f", "%M", "-o", str(rss_path), "--"] if use_time else []) + command
        started = time.perf_counter()
        process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=os.name == "posix")
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.communicate()
            raise RuntimeError(f"inventory command timed out after {timeout:g}s") from error
        elapsed = time.perf_counter() - started
        if process.returncode:
            raise RuntimeError(f"inventory command exited {process.returncode}: " +
                               stderr.decode("utf-8", errors="replace")[-4000:])
        validate_output(stdout, expected)
        peak_rss_kib = int(rss_path.read_text().strip()) if use_time else None
        return {"wall_seconds": elapsed, "peak_rss_kib": peak_rss_kib,
                "stdout_sha256": hashlib.sha256(stdout).hexdigest()}


def command_context(command: list[str]) -> dict:
    resolved = shutil.which(command[0])
    executable = Path(resolved).resolve() if resolved else None
    context = {"argv_template": command, "executable": str(executable) if executable else None,
               "executable_sha256": sha256_file(executable) if executable and executable.is_file() else None}
    if executable:
        for parent in executable.parents:
            cache = parent / "CMakeCache.txt"
            if cache.is_file():
                context["cmake_cache"] = str(cache)
                context["cmake_cache_sha256"] = sha256_file(cache)
                context["cmake_build_type"] = next((line.split("=", 1)[1]
                    for line in cache.read_text(errors="replace").splitlines()
                    if line.startswith("CMAKE_BUILD_TYPE:STRING=")), None)
                break
    return context


def repository_context() -> dict:
    def git(*args):
        result = subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True,
                                text=True, timeout=10, check=False)
        return result.stdout.strip() if result.returncode == 0 else None
    return {"root": str(ROOT), "commit": git("rev-parse", "HEAD"),
            "status": git("status", "--short", "--untracked-files=no")}


def benchmark(output_dir: Path, manifest: dict, commands: dict[str, list[str]], *,
              repetitions: int, warmups: int, timeout: float, measure_rss: bool) -> dict:
    if repetitions < 1 or warmups < 0:
        raise ValueError("positive repetitions and nonnegative warmups are required")
    report = {"schema_version": 1, "platform": platform.platform(), "python": sys.version,
              "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
              "repository": repository_context(), "manifest_sha256": sha256_file(output_dir / "manifest.json"),
              "selected_workloads": [workload["name"] for workload in manifest["workloads"]],
              "repetitions": repetitions, "warmups": warmups, "timeout_seconds": timeout,
              "timing_scope": "fresh process wall clock including launch, output capture, and optional GNU time wrapper",
              "cache_policy": "OS page cache is not flushed; fresh processes do not imply cold storage",
              "rss_method": "GNU time maximum child RSS (KiB)" if measure_rss and sys.platform.startswith("linux")
                            and Path("/usr/bin/time").is_file() else "unavailable or disabled",
              "commands": {label: command_context(command) for label, command in commands.items()},
              "results": []}
    prefix = manifest["parameters"]["class_prefix"]
    for workload in manifest["workloads"]:
        path = output_dir / workload["input"]
        if sha256_file(path) != workload["sha256"]:
            raise RuntimeError("fixture changed since manifest generation")
        expected_path = output_dir / workload["expected"]
        if sha256_file(expected_path) != workload["expected_sha256"]:
            raise RuntimeError("expected inventory changed since manifest generation")
        expected = expected_path.read_text(encoding="utf-8").splitlines()
        samples = {label: [] for label in commands}
        expanded = {label: [arg.replace("{input}", str(path.resolve())).replace("{prefix}", prefix)
                            for arg in command] for label, command in commands.items()}
        # Alternate command order to reduce systematic drift between compared tools.
        for iteration in range(warmups + repetitions):
            order = list(commands)
            if iteration % 2:
                order.reverse()
            for label in order:
                sample = run_once(expanded[label], expected, timeout=timeout, measure_rss=measure_rss)
                if iteration >= warmups:
                    samples[label].append(sample)
        for label, runs in samples.items():
            values = [run["wall_seconds"] for run in runs]
            rss = [run["peak_rss_kib"] for run in runs if run["peak_rss_kib"] is not None]
            report["results"].append({"workload": workload["name"], "command": label,
                                      "argv": expanded[label], "input_sha256": workload["sha256"],
                                      "class_count": len(expected), "samples": runs,
                                      "median_seconds": statistics.median(values), "min_seconds": min(values),
                                      "max_seconds": max(values), "median_peak_rss_kib": statistics.median(rss) if rss else None})
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory; existing paths are rejected")
    parser.add_argument("--workload", action="append", choices=WORKLOAD_NAMES,
                        help="workload to generate and run for every command; repeat to select several (default: all)")
    parser.add_argument("--class-count", type=int, default=1000, help="classes per DEX (1..65533)")
    parser.add_argument("--dex-count", type=int, default=3, help="DEX files in multidex workloads (2..64)")
    parser.add_argument("--extra-string-bytes", type=int, default=0, help="unreferenced string payload per DEX")
    parser.add_argument("--code-units", type=int, default=0, help="16-bit instructions per class method; 0 omits methods")
    parser.add_argument("--resource-bytes", type=int, default=0, help="unrelated incompressible APK asset size")
    parser.add_argument("--class-prefix", default="", help="descriptor prefix, for example Lbench/")
    parser.add_argument("--max-files", type=int, help="inventory limit; defaults to at least the full multidex class count")
    parser.add_argument("--neverd", type=Path, default=ROOT / "build-release/bin/neverd")
    parser.add_argument("--peer-command", help="shell-quoted argv template with {input} and optional {prefix}; no shell executes")
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--no-rss", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.timeout <= 0 or args.repetitions < 1 or args.warmups < 0:
            raise ValueError("timeout/repetitions must be positive and warmups nonnegative")
        if args.max_files is not None and args.max_files < 1:
            raise ValueError("max-files must be positive")
        # Avoid accidental multi-gigabyte allocations from mistyped workload sizes.
        estimated = args.class_count * (128 + args.code_units * 2) + args.extra_string_bytes
        if estimated < 0 or estimated * args.dex_count + args.resource_bytes > 1024 ** 3:
            raise ValueError("combined uncompressed fixture payload exceeds 1 GiB")
        manifest = generate_workloads(args.output_dir, class_count=args.class_count,
                                      dex_count=args.dex_count, extra_string_bytes=args.extra_string_bytes,
                                      code_units=args.code_units, resource_bytes=args.resource_bytes,
                                      class_prefix=args.class_prefix, selected_workloads=args.workload)
        if args.generate_only:
            print(args.output_dir / "manifest.json")
            return 0
        command = [str(args.neverd.resolve()), "mobile", "{input}", "--list-classes", "--max-files",
                   str(args.max_files if args.max_files is not None else max(20000, args.class_count * args.dex_count))]
        if args.class_prefix:
            command.extend(["--class-prefix", "{prefix}"])
        commands = {"neverd": command}
        if args.peer_command:
            peer = shlex.split(args.peer_command)
            if not peer or not any("{input}" in arg for arg in peer):
                raise ValueError("peer command must contain an {input} placeholder")
            commands["peer"] = peer
        report = benchmark(args.output_dir, manifest, commands, repetitions=args.repetitions,
                           warmups=args.warmups, timeout=args.timeout, measure_rss=not args.no_rss)
        report_path = args.output_dir / "results.json"
        report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(report_path)
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
