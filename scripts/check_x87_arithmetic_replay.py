#!/usr/bin/env python3
"""Freeze and compare protocol-v1 finite binary80 instruction observations.

Both executable paths are explicitly selected test drivers, never imported
target programs. Preserve all inputs, outputs and mismatches with SHA-256
bindings. Rosetta describes itself separately from an original x86-64 process.
This protocol checks numerical bits, exception flags and C1, not whole state,
translation, termination, or a proof of arbitrary-program equivalence.
"""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import struct
import subprocess
import time


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
            "unittests/lift/core/X87ArithmeticReplay.cpp",
            "include/neverd/ir/X87Binary80.h", "include/neverd/ir/X87Control.h",
            "include/neverd/ir/low/X87Arithmetic.h", "lib/ir/low/X87Arithmetic.cpp",
        )
    }
    try:
        record["drivers"]["portable"] = run_driver(
            args.portable_driver.resolve(), input_path, portable_path)
        record["drivers"]["original"] = run_driver(
            args.original_driver.resolve(), input_path, original_path)
        for kind, driver in record["drivers"].items():
            if driver["returncode"]:
                raise ValueError(f"{kind} driver failed: {driver['returncode']}")
        expected_backends = {"x86-64-original-instructions",
                             "rosetta2-x86-64-original-instructions"}
        if record["drivers"]["portable"]["metadata"]["backend"] != "neverd-portable-binary80":
            raise ValueError("portable driver is not the portable implementation")
        if record["drivers"]["original"]["metadata"]["backend"] not in expected_backends:
            raise ValueError("original driver does not identify original x87 instructions")
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
        record["passed"] = not record["mismatches"]
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
