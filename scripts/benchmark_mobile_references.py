#!/usr/bin/env python3
"""Generate independent DEX code-reference fixtures and verify timed queries.

The format is specified at https://source.android.com/docs/core/runtime/dex-format
and https://source.android.com/docs/core/runtime/dalvik-bytecode. Expectations
are recorded while emitting declarative instructions, never read from a decoder.
Synthetic APK containers are query fixtures, not installable applications.
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
import shlex
import signal
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import zipfile
import zlib

if __package__:
    from . import benchmark_mobile_inventory as inventory
else:
    import benchmark_mobile_inventory as inventory


KINDS = ("string", "type", "method", "field")
NEEDLE = "Lbench/Needle;"
NOISE = "Lbench/Noise;"


def targets(owner: str = NEEDLE, string: str = "benchmark needle") -> dict[str, str]:
    return {"string": string, "type": owner, "method": owner + "->call()V", "field": owner + "->value:I"}


def utf16_units(value: str) -> list[int]:
    data = value.encode("utf-16-le", errors="surrogatepass")
    return list(struct.unpack("<" + "H" * (len(data) // 2), data))


def mutf8(value: str) -> bytes:
    units = utf16_units(value)
    data = bytearray(inventory.uleb128(len(units)))
    for unit in units:
        if 0 < unit < 128:
            data.append(unit)
        elif unit < 2048:
            data.extend((0xC0 | (unit >> 6), 0x80 | (unit & 63)))
        else:
            data.extend((0xE0 | (unit >> 12), 0x80 | ((unit >> 6) & 63), 0x80 | (unit & 63)))
    data.append(0)
    return bytes(data)


def json_string(value: str) -> str | None:
    # A paired surrogate sequence has the same Unicode value as its scalar.
    try:
        return value.encode("utf-16-le", "surrogatepass").decode("utf-16-le")
    except UnicodeError:
        return None


def matches(query: dict, kind: str, target: str) -> bool:
    if query["kind"] != kind:
        return False
    if query.get("owner") and target.split("->", 1)[0] != query["owner"]:
        return False
    wanted = query["text"]
    return target == wanted if query["exact"] else wanted in target


def default_queries(*, needle_text: str = "benchmark needle") -> list[dict]:
    values = targets(string=needle_text)
    return [{"kind": kind, "text": values[kind], "exact": True, "owner": None} for kind in KINDS]


def make_reference_dex(*, class_count: int = 100, methods_per_class: int = 16,
                       matching_methods: int = 2, references_per_method: int = 2,
                       dex_index: int = 0, extra_strings: int = 0,
                       needle_text: str = "benchmark needle", payload_lookalikes: bool = True,
                       queries: list[dict] | None = None) -> tuple[bytes, dict, dict]:
    method_count = class_count * methods_per_class
    if class_count < 1 or class_count + 6 > 65535 or methods_per_class < 1 or method_count + 2 > 65535:
        raise ValueError("positive class/method counts must fit 65535 method IDs per DEX")
    if not 0 <= matching_methods <= method_count or not 1 <= references_per_method <= 1024:
        raise ValueError("matching-methods must fit the method count; references-per-method must be 1..1024")
    if not 0 <= extra_strings <= 1000000:
        raise ValueError("extra-strings must be 0..1000000")
    queries = default_queries(needle_text=needle_text) if queries is None else queries
    if not queries or len({query["kind"] for query in queries}) != len(queries):
        raise ValueError("supply one query per selected kind")
    needle, noise = targets(string=needle_text), targets(NOISE, "benchmark noise")
    classes = [f"Lbench/d{dex_index:03d}/C{index:06d};" for index in range(class_count)]
    definitions = [(owner, f"probe{index:04d}") for owner in classes for index in range(methods_per_class)]
    method_refs = sorted(definitions + [(NEEDLE, "call"), (NOISE, "call")])
    field_refs = [(NEEDLE, "value"), (NOISE, "value")]
    types = sorted(classes + ["I", NEEDLE, NOISE, "Ljava/lang/Object;", "V", "[S"])
    strings = sorted(set(types + ["call", "value", needle_text, noise["string"]] +
                         [name for _, name in definitions] + [f"!extra{index:08d}" for index in range(extra_strings)]),
                     key=utf16_units)
    string_ids = {value: index for index, value in enumerate(strings)}
    type_ids = {value: index for index, value in enumerate(types)}
    method_ids = {owner + "->" + name + "()V": index for index, (owner, name) in enumerate(method_refs)}
    field_ids = {owner + "->" + name + ":I": index for index, (owner, name) in enumerate(field_refs)}
    pools = {"string": string_ids, "type": type_ids, "method": method_ids, "field": field_ids}
    expected = {query["kind"]: [] for query in queries}
    statistics_out = {"class_count": class_count, "defined_method_count": method_count,
                      "code_item_count": method_count, "instruction_count": 0,
                      "code_units": 0, "payload_code_units": 0,
                      "lookalike_count": method_count * (11 if payload_lookalikes else 4),
                      "matching_pool_entries": {query["kind"]: sum(matches(query, query["kind"], value)
                                                  for value in pools[query["kind"]]) for query in queries}}

    def body(identity: str, hit: bool) -> list[int]:
        chosen = needle if hit else noise
        words: list[int] = []

        def instruction(*encoded: int) -> int:
            pc = len(words)
            words.extend(encoded)
            statistics_out["instruction_count"] += 1
            return pc

        def reference(kind: str, target: str, opcode: str, encoded: list[int]) -> None:
            pc = instruction(*encoded)
            for query in queries:
                if matches(query, kind, target):
                    row = {"method": identity, "pc_code_units": pc, "opcode": opcode, "kind": kind,
                           "target_index": pools[kind][target], "target": json_string(target)}
                    if kind == "string":
                        row["target_utf16"] = utf16_units(target)
                    expected[kind].append(row)

        sid, tid = string_ids[needle["string"]], type_ids[needle["type"]]
        mid, fid = method_ids[needle["method"]], field_ids[needle["field"]]
        # Real const operands contain complete-looking references at PCs 1/4/6/9.
        instruction(0x0014, 0x001A, sid & 65535)
        instruction(0x0018, 0x0071, mid, 0x0060, fid)
        instruction(0x0014, 0x001C, tid)

        def cycle(index: int) -> None:
            string_id = string_ids[chosen["string"]]
            jumbo = string_id > 65535 or index % 2 == 1
            reference("string", chosen["string"], "const-string/jumbo" if jumbo else "const-string",
                      [0x001B, string_id & 65535, string_id >> 16] if jumbo else [0x001A, string_id])
            reference("type", chosen["type"], "const-class", [0x001C, type_ids[chosen["type"]]])
            reference("field", chosen["field"], "sget", [0x0060, field_ids[chosen["field"]]])
            reference("method", chosen["method"], "invoke-static/range" if index % 2 else "invoke-static",
                      [0x0077 if index % 2 else 0x0071, method_ids[chosen["method"]], 0])

        for index in range(references_per_method - 1):
            cycle(index)
        fake_array = [0x001B, sid & 65535, sid >> 16, 0x001C, tid, 0x0071, mid, 0, 0x0060, fid]
        if not payload_lookalikes:
            fake_array = [0] * len(fake_array)
        instruction(0x0013, len(fake_array))  # const/16 v0, array length
        reference("type", "[S", "new-array", [0x0023, type_ids["[S"]])
        array_pc = instruction(0x0026, 0, 0)
        instruction(0x0012)  # const/4 v0, 0 before switches
        packed_pc = instruction(0x002B, 0, 0)
        sparse_pc = instruction(0x002C, 0, 0)
        jump_pc = instruction(0x002A, 0, 0)

        def payload(encoded: list[int]) -> int:
            if len(words) % 2:
                instruction(0)
            pc = len(words)
            words.extend(encoded)
            statistics_out["payload_code_units"] += len(encoded)
            return pc

        def halves(value: int) -> list[int]:
            return [value & 65535, (value >> 16) & 65535]

        array_at = payload([0x0300, 2, len(fake_array), 0, *fake_array])
        packed_at = payload([0x0100, 1, 0x001A if payload_lookalikes else 0,
                             sid & 65535 if payload_lookalikes else 0, 0, 0])
        keys = sorted([(tid << 16) | 0x001C, (fid << 16) | 0x0060]) if payload_lookalikes else [0, 1]
        sparse_at = payload([0x0200, 2, *halves(keys[0]), *halves(keys[1]), 0, 0, 0, 0])
        after = len(words)
        for start, destination in ((array_pc, array_at), (packed_pc, packed_at),
                                   (sparse_pc, sparse_at), (jump_pc, after)):
            words[start + 1:start + 3] = halves(destination - start)
        words[packed_at + 4:packed_at + 6] = halves(after - packed_pc)
        words[sparse_at + 6:sparse_at + 10] = halves(after - sparse_pc) * 2
        cycle(references_per_method - 1)
        instruction(0x000E)
        statistics_out["code_units"] += len(words)
        return words

    data, sections = bytearray(112), [(0, 1, 0)]

    def table(kind: int, count: int, width: int) -> int:
        at = len(data)
        data.extend(bytes(count * width))
        sections.append((kind, count, at))
        return at

    string_at = table(1, len(strings), 4)
    type_at = table(2, len(types), 4)
    proto_at = table(3, 1, 12)
    field_at = table(4, len(field_refs), 8)
    method_at = table(5, len(method_refs), 8)
    class_at = table(6, len(classes), 32)
    data_at = len(data)
    sections.append((0x2002, len(strings), len(data)))
    for index, value in enumerate(strings):
        struct.pack_into("<I", data, string_at + index * 4, len(data))
        data.extend(mutf8(value))
    for index, value in enumerate(types):
        struct.pack_into("<I", data, type_at + index * 4, string_ids[value])
    struct.pack_into("<III", data, proto_at, string_ids["V"], type_ids["V"], 0)
    for index, (owner, name) in enumerate(field_refs):
        struct.pack_into("<HHI", data, field_at + index * 8, type_ids[owner], type_ids["I"], string_ids[name])
    for index, (owner, name) in enumerate(method_refs):
        struct.pack_into("<HHI", data, method_at + index * 8, type_ids[owner], 0, string_ids[name])
    code_offsets = []
    for index, (owner, name) in enumerate(definitions):
        words = body(owner + "->" + name + "()V", index < matching_methods)
        data.extend(bytes(-len(data) % 4))
        code_offsets.append(len(data))
        data.extend(struct.pack("<HHHHII", 2, 0, 0, 0, 0, len(words)))
        data.extend(struct.pack("<" + "H" * len(words), *words))
    sections.append((0x2001, len(definitions), code_offsets[0]))
    class_data_offsets = []
    for class_index, owner in enumerate(classes):
        class_data_offsets.append(len(data))
        data.extend(b"\0\0" + inventory.uleb128(methods_per_class) + b"\0")
        previous = 0
        for local in range(methods_per_class):
            index = class_index * methods_per_class + local
            method_id = method_ids[owner + "->" + definitions[index][1] + "()V"]
            data.extend(inventory.uleb128(method_id - previous) + b"\x09" + inventory.uleb128(code_offsets[index]))
            previous = method_id
    sections.append((0x2000, len(classes), class_data_offsets[0]))
    for index, owner in enumerate(classes):
        struct.pack_into("<8I", data, class_at + index * 32, type_ids[owner], 1,
                         type_ids["Ljava/lang/Object;"], 0, 0xFFFFFFFF, 0, class_data_offsets[index], 0)
    data.extend(bytes(-len(data) % 4))
    map_at = len(data)
    sections.append((0x1000, 1, map_at))
    data.extend(struct.pack("<I", len(sections)))
    for kind, count, offset in sorted(sections, key=lambda entry: entry[2]):
        data.extend(struct.pack("<HHII", kind, 0, count, offset))
    data[:8] = b"dex\n035\0"
    struct.pack_into("<20I", data, 32, len(data), 112, 0x12345678, 0, 0, map_at,
                     len(strings), string_at, len(types), type_at, 1, proto_at,
                     len(field_refs), field_at, len(method_refs), method_at,
                     len(classes), class_at, len(data) - data_at, data_at)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xFFFFFFFF)
    return bytes(data), expected, statistics_out


def generate_workloads(output_dir: Path, *, queries: list[dict], dex_count: int = 2,
                       resource_bytes: int = 0, selected_workloads: list[str] | None = None, **options) -> dict:
    selected = set(inventory.WORKLOAD_NAMES if selected_workloads is None else selected_workloads)
    if not selected or not selected <= set(inventory.WORKLOAD_NAMES):
        raise ValueError("select at least one known workload")
    if not 2 <= dex_count <= 64 or not 0 <= resource_bytes <= 1024 ** 3:
        raise ValueError("dex-count must be 2..64 and resource-bytes must be 0..1 GiB")
    output_dir.mkdir(parents=True, exist_ok=False)
    fixtures = [make_reference_dex(dex_index=index, queries=queries, **options) for index in range(dex_count)]
    resource = hashlib.shake_256(b"NeverD mobile references resource v1").digest(resource_bytes)
    manifest = {"schema_version": 1, "generator_sha256": inventory.sha256_file(Path(__file__).resolve()),
                "parameters": {**options, "dex_count": dex_count, "resource_bytes": resource_bytes},
                "queries": queries, "workloads": []}
    for label in inventory.WORKLOAD_NAMES:
        if label not in selected:
            continue
        count = dex_count if label.startswith("multidex") else 1
        name = "classes.dex" if label == "dex" else label + ".apk"
        path = output_dir / name
        if label == "dex":
            path.write_bytes(fixtures[0][0])
        else:
            compression = zipfile.ZIP_STORED if label.endswith("stored") else zipfile.ZIP_DEFLATED
            inventory.write_apk(path, [fixture[0] for fixture in fixtures[:count]], compression, resource)
        workload = {"name": label, "input": name, "sha256": inventory.sha256_file(path),
                    "size_bytes": path.stat().st_size,
                    "statistics": {key: sum(fixture[2][key] for fixture in fixtures[:count])
                                   for key in fixtures[0][2] if key != "matching_pool_entries"},
                    "expectations": {}}
        for query in queries:
            kind = query["kind"]
            rows = [{"dex_entry": "classes.dex" if index == 0 else f"classes{index + 1}.dex", **row}
                    for index, (_, expected, _) in enumerate(fixtures[:count]) for row in expected[kind]]
            expected_name = f"{label}.{kind}.expected.json"
            counts = {"dex_count": count, "class_count": workload["statistics"]["class_count"],
                      "defined_method_count": workload["statistics"]["defined_method_count"],
                      "scanned_method_count": workload["statistics"]["defined_method_count"],
                      "scanned_code_item_count": workload["statistics"]["code_item_count"],
                      "matching_pool_entries": sum(fixture[2]["matching_pool_entries"][kind] for fixture in fixtures[:count]),
                      "reference_count": len(rows)}
            (output_dir / expected_name).write_text(json.dumps({"query": query, "references": rows, "counts": counts},
                                                               ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
            workload["expectations"][kind] = {"file": expected_name, "sha256": inventory.sha256_file(output_dir / expected_name),
                                               "reference_count": len(rows)}
        manifest["workloads"].append(workload)
    (output_dir / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    return manifest


def canonical_rows(rows: list[dict]) -> list[str]:
    canonical = []
    for row in rows:
        if not isinstance(row, dict) or row.get("kind") not in KINDS:
            raise RuntimeError("invalid reference row kind")
        for name in ("dex_entry", "method", "opcode"):
            if not isinstance(row.get(name), str):
                raise RuntimeError("invalid reference row " + name)
        if "target" not in row:
            raise RuntimeError("missing resolved reference target")
        if type(row.get("pc_code_units")) is not int or row["pc_code_units"] < 0:
            raise RuntimeError("invalid reference PC")
        keys = ("dex_entry", "method", "pc_code_units", "opcode", "kind", "target")
        result = {key: row.get(key) for key in keys}
        if row["kind"] == "string":
            units = row.get("target_utf16")
            if not isinstance(units, list) or any(type(unit) is not int or not 0 <= unit <= 65535 for unit in units):
                raise RuntimeError("string reference is missing exact UTF-16 units")
            value = struct.pack("<" + "H" * len(units), *units).decode("utf-16-le", "surrogatepass")
            if row.get("target") != json_string(value):
                raise RuntimeError("string target disagrees with UTF-16 units")
            result["target_utf16"] = units
        elif not isinstance(row.get("target"), str):
            raise RuntimeError("invalid resolved reference target")
        # Pool indices are file-local transport details; exact resolved identity,
        # caller, opcode, PC, and multiplicity are the cross-tool contract.
        canonical.append(json.dumps(result, ensure_ascii=True, sort_keys=True))
    return sorted(canonical)


def validate_output(stdout: bytes, expected: list[dict], *, expected_counts: dict | None = None,
                    require_full_scan: bool = True) -> dict:
    try:
        report = json.loads(stdout.decode("utf-8"))
    except (ValueError, UnicodeError) as error:
        raise RuntimeError("reference output is not UTF-8 JSON") from error
    if not isinstance(report, dict) or not isinstance(report.get("references"), list):
        raise RuntimeError("reference output must contain a references array")
    if report.get("status") != "success":
        raise RuntimeError("reference report is not successful")
    if require_full_scan and report.get("code_scan_complete") is not True:
        raise RuntimeError("reference benchmark requires a complete code scan")
    if type(report.get("reference_count")) is not int or report["reference_count"] != len(report["references"]):
        raise RuntimeError("reference_count disagrees with occurrence rows")
    if expected_counts is not None:
        for key, value in expected_counts.items():
            if type(report.get(key)) is not int or report[key] != value:
                raise RuntimeError("reference report count mismatch: " + key)
    if canonical_rows(report["references"]) != canonical_rows(expected):
        raise RuntimeError(f"reference mismatch: expected {len(expected)} sites, got {len(report['references'])}")
    return {key: value for key, value in report.items() if key != "references"}


def run_once(command: list[str], expected: list[dict], *, timeout: float, measure_rss: bool = True,
             expected_counts: dict | None = None, require_full_scan: bool = True) -> dict:
    if not command or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("a command and positive timeout are required")
    with tempfile.TemporaryDirectory(prefix="neverd-reference-run-") as temporary:
        rss_path = Path(temporary) / "rss.txt"
        use_time = measure_rss and sys.platform.startswith("linux") and Path("/usr/bin/time").is_file()
        argv = (["/usr/bin/time", "-f", "%M", "-o", str(rss_path), "--"] if use_time else []) + command
        started = time.perf_counter()
        process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=os.name == "posix")
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            os.killpg(process.pid, signal.SIGKILL) if os.name == "posix" else process.kill()
            process.communicate()
            raise RuntimeError(f"reference command timed out after {timeout:g}s") from error
        elapsed = time.perf_counter() - started
        if process.returncode:
            raise RuntimeError(f"reference command exited {process.returncode}: " + stderr.decode("utf-8", "replace")[-4000:])
        metadata = validate_output(stdout, expected, expected_counts=expected_counts, require_full_scan=require_full_scan)
        return {"wall_seconds": elapsed, "peak_rss_kib": int(rss_path.read_text().strip()) if use_time else None,
                "stdout_sha256": hashlib.sha256(stdout).hexdigest(), "observed_report": metadata}


def benchmark(output_dir: Path, manifest: dict, commands: dict[str, list[str]], *,
              repetitions: int, warmups: int, timeout: float, measure_rss: bool) -> dict:
    if repetitions < 1 or warmups < 0 or not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("positive repetitions/timeout and nonnegative warmups are required")
    report = {"schema_version": 1, "platform": platform.platform(), "python": sys.version,
              "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
              "repository": inventory.repository_context(), "manifest_sha256": inventory.sha256_file(output_dir / "manifest.json"),
              "selected_workloads": [workload["name"] for workload in manifest["workloads"]],
              "fixture_profile": "payload-lookalikes" if manifest["parameters"].get("payload_lookalikes", True)
                                  else "plain-payloads",
              "queries": manifest["queries"], "repetitions": repetitions, "warmups": warmups, "timeout_seconds": timeout,
              "timing_scope": "fresh process wall clock including launch, output capture, and optional GNU time wrapper",
              "cache_policy": "OS page cache is not flushed; fresh processes do not imply cold storage",
              "rss_method": "GNU time maximum child RSS (KiB)" if measure_rss and sys.platform.startswith("linux")
                            and Path("/usr/bin/time").is_file() else "unavailable or disabled",
              "commands": {label: inventory.command_context(command) for label, command in commands.items()}, "results": []}
    for workload in manifest["workloads"]:
        path = output_dir / workload["input"]
        if inventory.sha256_file(path) != workload["sha256"]:
            raise RuntimeError("fixture changed since manifest generation")
        for query in manifest["queries"]:
            expectation = workload["expectations"][query["kind"]]
            expected_path = output_dir / expectation["file"]
            if inventory.sha256_file(expected_path) != expectation["sha256"]:
                raise RuntimeError("expected references changed since manifest generation")
            expected_document = json.loads(expected_path.read_text())
            expected = expected_document["references"]
            replacements = {"{input}": str(path.resolve()), "{kind}": query["kind"], "{query}": query["text"],
                            "{exact}": "--exact" if query["exact"] else "", "{owner}": query["owner"] or ""}
            expanded = {}
            for label, command in commands.items():
                argv = []
                for arg in command:
                    if arg in ("{exact}", "{owner}") and not replacements[arg]:
                        continue
                    arg = re.sub(r"\{(?:input|kind|query|exact|owner)\}", lambda match: replacements[match[0]], arg)
                    argv.append(arg)
                expanded[label] = argv
            samples = {label: [] for label in commands}
            for iteration in range(warmups + repetitions):
                order = list(commands) if iteration % 2 == 0 else list(reversed(commands))
                for label in order:
                    result = run_once(expanded[label], expected, timeout=timeout, measure_rss=measure_rss,
                                      expected_counts=expected_document["counts"] if label == "neverd" else None,
                                      require_full_scan=label == "neverd")
                    if iteration >= warmups:
                        samples[label].append(result)
            for label, runs in samples.items():
                wall = [run["wall_seconds"] for run in runs]
                rss = [run["peak_rss_kib"] for run in runs if run["peak_rss_kib"] is not None]
                report["results"].append({"workload": workload["name"], "kind": query["kind"], "command": label,
                                          "argv": expanded[label], "input_sha256": workload["sha256"],
                                          "reference_count": len(expected), "statistics": workload["statistics"], "samples": runs,
                                          "median_seconds": statistics.median(wall), "min_seconds": min(wall), "max_seconds": max(wall),
                                          "median_peak_rss_kib": statistics.median(rss) if rss else None})
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory; existing paths are rejected")
    parser.add_argument("--workload", action="append", choices=inventory.WORKLOAD_NAMES)
    parser.add_argument("--kind", action="append", choices=KINDS, help="repeat to select query kinds; default all")
    parser.add_argument("--query", help="query text; requires one selected kind")
    parser.add_argument("--exact", action="store_true")
    parser.add_argument("--owner", help="exact target method/field owner descriptor")
    parser.add_argument("--needle-text", default="benchmark needle")
    parser.add_argument("--no-payload-lookalikes", action="store_true",
                        help="neutral payload data with the same layout; retain literal decoys for a common timing workload")
    parser.add_argument("--class-count", type=int, default=100)
    parser.add_argument("--methods-per-class", type=int, default=16)
    parser.add_argument("--matching-methods", type=int, default=2, help="matching methods per DEX")
    parser.add_argument("--references-per-method", type=int, default=2, help="occurrences per queried kind per matching method")
    parser.add_argument("--extra-strings", type=int, default=0, help="strings ordered before targets, to exercise jumbo indices")
    parser.add_argument("--dex-count", type=int, default=2)
    parser.add_argument("--resource-bytes", type=int, default=0)
    parser.add_argument("--neverd", type=Path, default=inventory.ROOT / "build-release/bin/neverd")
    parser.add_argument("--peer-command", help="shell-quoted argv template with {input}, {kind}, {query}, optionally {exact}/{owner}; no shell executes")
    parser.add_argument("--repetitions", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--no-rss", action="store_true")
    parser.add_argument("--generate-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        kinds = [kind for kind in KINDS if args.kind is None or kind in args.kind]
        if args.query is not None and len(kinds) != 1:
            raise ValueError("--query requires exactly one --kind")
        if args.owner is not None and any(kind not in ("method", "field") for kind in kinds):
            raise ValueError("--owner is only valid for method/field queries")
        if args.repetitions < 1 or args.warmups < 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError("positive repetitions/timeout and nonnegative warmups are required")
        estimate = args.class_count * args.methods_per_class * (512 + args.references_per_method * 32)
        if estimate * args.dex_count + args.extra_strings * args.dex_count * 32 + args.resource_bytes > 1024 ** 3:
            raise ValueError("estimated combined fixture payload exceeds 1 GiB")
        values = targets(string=args.needle_text)
        queries = [{"kind": kind, "text": args.query if args.query is not None else values[kind],
                    "exact": args.exact, "owner": args.owner} for kind in kinds]
        manifest = generate_workloads(args.output_dir, queries=queries, class_count=args.class_count,
                                      methods_per_class=args.methods_per_class, matching_methods=args.matching_methods,
                                      references_per_method=args.references_per_method, extra_strings=args.extra_strings,
                                      needle_text=args.needle_text, payload_lookalikes=not args.no_payload_lookalikes,
                                      dex_count=args.dex_count,
                                      resource_bytes=args.resource_bytes, selected_workloads=args.workload)
        if args.generate_only:
            print(args.output_dir / "manifest.json")
            return 0
        command = [str(args.neverd.resolve()), "mobile", "{input}", "--find-refs={kind}", "--query", "{query}",
                   "{exact}", "--json", "--max-files", str(max(20000, args.class_count * args.methods_per_class * args.dex_count,
                   max(expectation["reference_count"] for workload in manifest["workloads"]
                       for expectation in workload["expectations"].values())))]
        if args.owner:
            command.extend(["--owner", "{owner}"])
        commands = {"neverd": command}
        if args.peer_command:
            peer = shlex.split(args.peer_command)
            if not peer or not any("{input}" in arg for arg in peer):
                raise ValueError("peer command must contain {input}")
            commands["peer"] = peer
        report = benchmark(args.output_dir, manifest, commands, repetitions=args.repetitions,
                           warmups=args.warmups, timeout=args.timeout, measure_rss=not args.no_rss)
        path = args.output_dir / "results.json"
        path.write_text(json.dumps(report, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
        print(path)
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
