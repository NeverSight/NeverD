#!/usr/bin/env python3
"""Freeze and compare protocol-v1 finite binary80 instruction observations.

Both executable paths are explicitly selected test drivers, never imported
target programs. Preserve all inputs, outputs and mismatches with SHA-256
bindings. Native i386/x64 and Rosetta describe themselves separately.
This protocol checks numerical bits, exception flags and C1, not whole state,
translation, termination, or a proof of arbitrary-program equivalence.
"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import shutil
import struct
import subprocess
import time

from check_x87_native_regression import linked_libraries


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inputs(seed):
    rng = random.Random(seed)
    integer_bit = 1 << 63

    def value(significand, exponent):
        return struct.pack("<QH", significand, exponent)

    boundaries = [
        value(0, 0), value(0, 0x8000), value(1, 0), value(1, 0x8000),
        value(integer_bit - 1, 0), value(integer_bit - 1, 0x8000),
        value(integer_bit, 1), value(integer_bit, 0x8001),
        value(integer_bit, 0x3fff), value(integer_bit, 0xbfff),
        value((1 << 64) - 1, 0x7ffe), value((1 << 64) - 1, 0xfffe),
    ]
    pairs = [(a, b) for a in boundaries for b in boundaries]
    for _ in range(256):
        a = value(rng.getrandbits(64) | integer_bit,
                  rng.randrange(1, 0x7fff) | (rng.getrandbits(1) << 15))
        b = value(rng.getrandbits(64) | integer_bit,
                  rng.randrange(1, 0x7fff) | (rng.getrandbits(1) << 15))
        pairs.append((a, b))
    # Values immediately above both reduced-precision halfway points.
    for precision in (24, 53):
        pairs.append((value(integer_bit, 0x3fff),
                      value(integer_bit + 1, 0x3fff - precision)))
    return b"".join(
        struct.pack("<BBH", op, 0, 0x7f | (pc << 8) | (rc << 10)) + a + b
        for a, b in pairs for pc in (0, 2, 3) for rc in range(4) for op in range(4)
    )


def run_driver(path, source, destination):
    metadata = json.loads(subprocess.check_output([str(path), "--describe"]))
    if metadata.get("protocol") != 1:
        raise ValueError("driver does not implement protocol v1")
    started = time.monotonic()
    completed = subprocess.run([str(path), str(source), str(destination)],
                               capture_output=True, check=False)
    return {
        "path": str(path), "sha256": sha256(path), "metadata": metadata,
        "returncode": completed.returncode,
        "stderr": completed.stderr.decode("utf-8", errors="replace"),
        "elapsed_seconds_including_process_io": time.monotonic() - started,
        "output_sha256": sha256(destination) if destination.exists() else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--portable-driver", type=Path, required=True)
    parser.add_argument("--original-driver", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--portable-build", required=True,
                        help="declared compiler/build command and flags for the portable driver")
    parser.add_argument("--original-build", required=True,
                        help="declared compiler/build command and flags for the original driver")
    parser.add_argument("--seed", type=lambda s: int(s, 0), default=0x871677)
    args = parser.parse_args()
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    payload = inputs(args.seed)
    input_path = destination / "inputs.bin"
    input_path.write_bytes(payload)
    portable_path, original_path = (destination / "portable.bin",
                                     destination / "original.bin")
    record = {
        "protocol": "neverd-x87-finite-arithmetic-v1",
        "scope": "masked finite numerical bits, new IE/DE/ZE/OE/UE/PE and C1",
        "not_evidence_for": ["whole architectural state", "OS exception delivery",
                             "translation", "termination", "formal equivalence"],
        "environment": {"platform": platform.platform(), "machine": platform.machine(),
                        "python": platform.python_version()},
        "seed": args.seed, "observations": len(payload) // 24,
        "inputs_sha256": sha256(input_path), "script_sha256": sha256(Path(__file__)),
        "drivers": {}, "mismatches": [],
        "declared_build_commands": {"portable": args.portable_build,
                                    "original": args.original_build},
    }
    source_root = Path(__file__).resolve().parent.parent
    record["source_sha256"] = {
        name: sha256(source_root / name) for name in (
            "unittests/lift/x86/X87ArithmeticReplay.cpp",
            "unittests/lift/x86/X87OriginalReplay.c",
            "include/neverd/ir/X86/X87Binary80.h", "include/neverd/ir/X86/X87Control.h",
            "include/neverd/ir/low/X86/X87Arithmetic.h", "lib/ir/low/X86/X87Arithmetic.cpp",
            "scripts/check_x87_arithmetic_replay.py",
            "scripts/check_x87_native_regression.py",
        )
    }
    try:
        selected = {"portable": args.portable_driver.resolve(strict=True),
                    "original": args.original_driver.resolve(strict=True)}
        record["driver_sha256_before"] = {kind: sha256(path)
                                          for kind, path in selected.items()}
        for name in record["source_sha256"]:
            copy = destination / "source" / name
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_root / name, copy)
        for kind, path in selected.items():
            copy = destination / "drivers" / kind
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, copy)
        runtime = {}
        if platform.system() == "Linux":
            for path in selected.values():
                runtime.update(linked_libraries(path))
            record["cpu"] = json.loads(subprocess.check_output(["lscpu", "--json"]))
        record["runtime_sha256"] = runtime
        record["runtime_binding"] = ("Linux ldd closure" if platform.system() == "Linux"
                                     else "not collected on this host")
        record["drivers"]["portable"] = run_driver(
            selected["portable"], input_path, portable_path)
        record["drivers"]["original"] = run_driver(
            selected["original"], input_path, original_path)
        for kind, driver in record["drivers"].items():
            if driver["returncode"]:
                raise ValueError(f"{kind} driver failed: {driver['returncode']}")
        expected_backends = {"x86-64-original-instructions", "i386-original-instructions",
                             "rosetta2-x86-64-original-instructions"}
        if record["drivers"]["portable"]["metadata"]["backend"] != "neverd-portable-binary80":
            raise ValueError("portable driver is not the portable implementation")
        if record["drivers"]["original"]["metadata"]["backend"] not in expected_backends:
            raise ValueError("original driver does not identify original x87 instructions")
        original = record["drivers"]["original"]["metadata"]
        if original["backend"] == "i386-original-instructions" and (
                original.get("process_arch") != "i386" or platform.system() != "Linux" or
                platform.machine() not in {"i386", "i686", "x86_64"}):
            raise ValueError("i386 original profile requires a native Linux x86 host")
        a, b = portable_path.read_bytes(), original_path.read_bytes()
        if len(a) != len(payload) // 2 or len(b) != len(a):
            raise ValueError("response count differs from request count")
        for i in range(record["observations"]):
            actual, expected = a[12*i:12*i+12], b[12*i:12*i+12]
            actual_flags = int.from_bytes(actual[10:], "little") & 0x23f
            expected_flags = int.from_bytes(expected[10:], "little") & 0x23f
            if actual[:10] != expected[:10] or actual_flags != expected_flags:
                record["mismatches"].append({
                    "index": i, "input": payload[24*i:24*i+24].hex(),
                    "portable": actual.hex(), "original": expected.hex(),
                })
        record["stable_inputs"] = (
            sha256(input_path) == record["inputs_sha256"] and
            sha256(Path(__file__)) == record["script_sha256"] and
            all(sha256(source_root / name) == digest and
                sha256(destination / "source" / name) == digest
                for name, digest in record["source_sha256"].items()) and
            all(sha256(path) == record["driver_sha256_before"][kind] and
                sha256(destination / "drivers" / kind) == record["driver_sha256_before"][kind]
                for kind, path in selected.items()) and
            all(sha256(Path(path)) == digest for path, digest in runtime.items()))
        record["passed"] = not record["mismatches"] and record["stable_inputs"]
    except (OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        record["passed"] = False
        record["error"] = str(error)
    (destination / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"passed": record["passed"], "observations": record["observations"],
                      "mismatches": len(record["mismatches"]),
                      "record": str(destination / "record.json")}, indent=2))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
