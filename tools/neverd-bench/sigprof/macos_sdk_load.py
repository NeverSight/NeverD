#!/usr/bin/env python3
"""Measure first and repeat loads of a Release SDK in fresh macOS processes."""

import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def compile_probe(directory):
    probe = directory / "macos-sdk-load"
    source = Path(__file__).with_name("macos-sdk-load.c")
    subprocess.run(
        ["xcrun", "clang", "-O2", "-Wall", "-fblocks", "-arch", platform.machine(),
         "-mmacosx-version-min=13.0", str(source), "-o", str(probe)],
        check=True,
    )
    return probe


def observation(probe, mode, sdk, env):
    output = subprocess.check_output([probe, mode, sdk], env=env, text=True)
    phases = {}
    resident = pages = identity = None
    for line in output.splitlines():
        fields = line.split()
        if fields[:1] == ["residency"]:
            resident, pages = map(int, fields[1:])
        elif fields[:1] == ["image"]:
            device, inode = map(int, fields[1:])
            identity = (device, inode)
        elif fields[:1] == ["phase"]:
            name = fields[1]
            wall, cpu, runnable = (int(x) / 1e6 for x in fields[2:])
            phases[name] = {
                "wall_ms": wall,
                "cpu_ms": cpu,
                "ready_ms": runnable - cpu,
                "other_ms": wall - runnable,
            }
    expected = ["empty", "dlopen"] if mode == "direct" else [
        "empty", "inspect", "register_signature", "check_library", "dlopen"
    ]
    if resident is None or not pages or identity is None or list(phases) != expected:
        raise ValueError("incomplete SDK timing record")
    if mode == "staged":
        phases["staged_total"] = {
            metric: sum(phases[name][metric] for name in expected if name != "empty")
            for metric in phases["dlopen"]
        }
    return {"resident_pages": resident, "pages": pages, "image_id": identity, "phases": phases}


def measure(source, pairs, directory):
    # clonefile fails on an unsupported filesystem instead of silently populating
    # the new file's cache with a conventional copy.
    clone = ctypes.CDLL(None, use_errno=True).clonefile
    clone.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
    clone.restype = ctypes.c_int
    reference = digest(source)
    sdk = directory / "libneverd.dylib"
    probe = compile_probe(directory)
    env = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    observations = []
    for pair in range(pairs):
        states = ["fresh", "pre_read"]
        if pair % 2:
            states.reverse()
        for state in states:
            modes = ["direct", "staged"]
            if (pair + (state == "pre_read")) % 2:
                modes.reverse()
            for mode in modes:
                if clone(os.fsencode(source), os.fsencode(sdk), 0):
                    error = ctypes.get_errno()
                    raise OSError(error, f"clonefile: {os.strerror(error)}. Use a scratch "
                                  "directory on the SDK's APFS volume")
                target = sdk.stat()
                if state == "pre_read":
                    with sdk.open("rb") as stream:
                        while stream.read(1024 * 1024):
                            pass
                for launch in ("first", "repeat"):
                    row = observation(probe, mode, sdk, env)
                    if row["image_id"] != (target.st_dev, target.st_ino):
                        raise ValueError("first and repeat launches must use the same SDK file")
                    observations.append({"pair": pair + 1, "mode": mode, "state": state,
                                         "launch": launch, **row})
                # Reading this copy before either launch changes the experiment.
                if digest(sdk) != reference:
                    raise ValueError("SDK bytes differ from the original")
                sdk.unlink()
    return {"sdk_sha256": reference, "sdk_bytes": source.stat().st_size,
            "macos": platform.mac_ver()[0], "architecture": platform.machine(),
            "pairs": pairs, "observations": observations}


def print_summary(rows):
    groups = {}
    residency = {}
    for row in rows:
        cell = (row["state"], row["launch"], row["mode"])
        residency.setdefault(cell, []).append(100 * row["resident_pages"] / row["pages"])
        for phase, timings in row["phases"].items():
            groups.setdefault((*cell, phase), []).append(timings)
    print("state    launch mode   phase               n  resident%   wall p50   wall p95"
          "    cpu p50  ready p50  other p50  (ms)")
    for key in sorted(groups, key=lambda key: key[:3]):
        state, launch, mode, phase = key
        values = groups[key]
        walls = sorted(t["wall_ms"] for t in values)
        p95 = walls[math.ceil(0.95 * len(walls)) - 1]
        medians = [statistics.median(t[k] for t in values)
                   for k in ("cpu_ms", "ready_ms", "other_ms")]
        print(f"{state:8} {launch:6} {mode:6} {phase:18} {len(walls):2}"
              f" {statistics.median(residency[key[:3]]):10.1f}"
              f" {statistics.median(walls):10.2f} {p95:10.2f}"
              + "".join(f" {v:10.2f}" for v in medians))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", type=Path, help="path to the SDK dylib to measure")
    parser.add_argument("--pairs", type=int, default=6)
    parser.add_argument("--scratch-dir", type=Path, help="directory on the SDK's APFS volume")
    parser.add_argument("--json", type=Path, help="write raw observations after verification")
    args = parser.parse_args()
    if platform.system() != "Darwin" or int(platform.mac_ver()[0].split(".")[0]) < 13:
        parser.error("requires macOS 13 or later")
    if args.pairs < 1:
        parser.error("--pairs must be positive")
    try:
        if args.json and args.json.exists() and args.sdk.samefile(args.json):
            parser.error("choose a report path distinct from the input SDK")
        with tempfile.TemporaryDirectory(prefix="neverd-sdk-load-", dir=args.scratch_dir) as tmp:
            result = measure(args.sdk.resolve(strict=True), args.pairs, Path(tmp))
        if args.json:
            args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print_summary(result["observations"])
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
