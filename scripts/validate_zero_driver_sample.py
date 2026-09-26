#!/usr/bin/env python3
# ===- validate_zero_driver_sample.py - External stream I/O validation -----===#
#
# NeverD Decompiler
#
# ===----------------------------------------------------------------------===#
#
# Download, verify and build Pavel Yosifovich's unmodified Zero WDM sample.
# This opt-in validation needs network access, Clang, LLD, MinGW-w64's DDK
# headers and nm. The downloaded source and its MIT license remain together.
# No host driver is loaded.
#
# ===----------------------------------------------------------------------===#

"""Validate synchronous direct READ/WRITE against an unmodified public driver."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "unittests/emulation/fixtures/zero-validation.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def fetch_sources(output: Path, manifest: dict) -> None:
    for source, expected_hash in manifest["files"].items():
        destination = output / Path(source).name
        if destination.exists():
            data = destination.read_bytes()
        else:
            url = f'{manifest["raw_root"]}/{manifest["revision"]}/{source}'
            with urllib.request.urlopen(url, timeout=30) as response:
                data = response.read()
        require(hashlib.sha256(data).hexdigest() == expected_hash,
                f"Upstream source hash mismatch: {source}")
        destination.write_bytes(data)
    (output / "source.json").write_text(json.dumps(manifest, indent=2) + "\n")


def build_driver(args: argparse.Namespace, output: Path) -> None:
    commands = []

    def run(command: list[str]) -> subprocess.CompletedProcess:
        commands.append(command)
        (output / "build-commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        with (output / "build.log").open("a") as log:
            log.write(result.stdout + result.stderr)
        require(result.returncode == 0,
                f"Build command failed: {command[0]}\n{result.stdout}{result.stderr}")
        return result

    (output / "build.log").write_text("")
    # MinGW's C++ intrinsic header and wdm.h both define these two helpers.
    # Keep wdm.h's implementations, suppressing only the duplicate definitions.
    # The Zero source is byte-for-byte upstream; InterlockedAdd64 remains real
    # guest machine code and is exercised by the final statistics query.
    run([args.clang, "--target=x86_64-w64-windows-gnu", "-std=c++17",
         "-isystem", str(args.headers), "-isystem", str(args.headers / "ddk"),
         "-fms-extensions", "-D__INTRINSIC_DEFINED_InterlockedBitTestAndSet",
         "-D__INTRINSIC_DEFINED_InterlockedBitTestAndReset",
         "-DDBG=0", "-D_WIN64", "-D_AMD64_", "-D_M_AMD64",
         "-fno-stack-protector", "-fno-exceptions", "-fno-rtti", "-O2",
         "-c", str(output / "Zero.cpp"), "-o", str(output / "zero.obj")])
    symbols = run([args.nm, "--undefined-only", "--format=posix",
                   str(output / "zero.obj")])
    exports = sorted({line.split()[0].removeprefix("__imp_")
                      for line in symbols.stdout.splitlines() if line.strip()})
    require(bool(exports), "Driver object has no observable kernel imports")
    (output / "ntoskrnl.def").write_text(
        "LIBRARY ntoskrnl.exe\nEXPORTS\n  " + "\n  ".join(exports) + "\n")
    run([args.linker, "/lib", "/machine:x64", f"/def:{output / 'ntoskrnl.def'}",
         f"/out:{output / 'ntoskrnl.lib'}"])
    run([args.linker, "/machine:x64", "/entry:DriverEntry", "/subsystem:native",
         "/driver", "/nodefaultlib", "/base:0x180000000",
         f"/out:{output / 'zero.sys'}", str(output / "zero.obj"),
         str(output / "ntoskrnl.lib")])


def validate_scenario(neverd: Path, output: Path, manifest: dict) -> None:
    scenario = output / "scenario.json"
    scenario.write_text(json.dumps(manifest["scenario"], indent=2) + "\n")
    run = subprocess.run([str(neverd), "emulate-driver", str(output / "zero.sys"),
                          "--scenario", str(scenario)],
                         capture_output=True, text=True, timeout=30)
    (output / "report.json").write_text(run.stdout)
    (output / "stderr.txt").write_text(run.stderr)
    require(bool(run.stdout), f"No report: {run.stderr}")
    report = json.loads(run.stdout)
    require(report["stop_reason"] == "returned",
            f"Incomplete execution: {report['diagnostic']}")
    require(report["nt_status"] == 0 and report["unload_completed"],
            "Entry/unload did not complete")
    requests = report["requests"]
    expected = manifest["expected_statuses"]
    require(len(requests) == len(expected) and all(item["completed"] for item in requests),
            "Incomplete requests")
    require([item["io_status"] for item in requests] == expected,
            "Unexpected completion status")
    require([item["dispatch_status"] for item in requests] == expected,
            "Unexpected dispatch status")
    require([item["information"] for item in requests] == manifest["expected_information"],
            "Unexpected transfer counts")
    for index, request in enumerate(requests):
        kind = manifest["scenario"]["requests"][index]["kind"]
        require(request["kind"] == kind, f"Wrong request kind at {index}")
        if kind == "read":
            output_bytes = bytes(manifest["expected_information"][index])
        elif kind == "ioctl":
            output_bytes = struct.pack("<QQ", manifest["expected_total_read"],
                                       manifest["expected_total_written"])
        else:
            output_bytes = b""
        require(bytes.fromhex(request["output_hex"]) == output_bytes,
                f"Wrong output for request {index} ({kind})")
    require(not report["devices"], "Device leaked after unload")
    # The original driver rejects a zero-length read and has no CLEANUP
    # handler. Keep both visible failures, then require CLOSE and unload.
    require(run.returncode == 2 and report["scenario_success"] is False,
            "Expected request failures were hidden")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, default=ROOT / "build-release/bin/neverd")
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-release/driver-validation/zero")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--headers", type=Path,
                        default=Path("/usr/share/mingw-w64/include"))
    parser.add_argument("--nm", default="nm")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(MANIFEST.read_text())
    fetch_sources(output, manifest)
    build_driver(args, output)
    validate_scenario(args.neverd.resolve(), output, manifest)
    print("Validated Zero: direct reads, write counts, guest statistics, "
          f"zero-length read rejection, default cleanup, close and unload.\n{output}")


if __name__ == "__main__":
    main()
