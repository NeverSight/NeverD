#!/usr/bin/env python3
# ===- validate_windows_driver_sample.py - External WDM validation ----------===#
#
# NeverD Decompiler
#
# ===----------------------------------------------------------------------===#
#
# Download, verify and build Microsoft's unmodified SIOCTL sample, then check
# buffered and direct-I/O scenarios. This opt-in validation needs network access,
# Clang, LLD and MinGW-w64's DDK headers and nm. No host driver is loaded.
#
# ===----------------------------------------------------------------------===#

"""Validate the bounded WDM lifecycle against an unmodified upstream driver."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "unittests/emulation/fixtures/sioctl-validation.json"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, default=ROOT / "build-release/bin/neverd")
    parser.add_argument("--output", type=Path, default=ROOT / "build-release/driver-validation/sioctl")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--headers", type=Path, default=Path("/usr/share/mingw-w64/include"))
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--debug", action="store_true", help="Enable the sample's DBG logging")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(MANIFEST.read_text())
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
    commands = [
        [args.clang, "--target=x86_64-w64-windows-gnu",
         "-isystem", str(args.headers), "-isystem", str(args.headers / "ddk"),
         "-fms-extensions", "-Dtry=__try", "-Dexcept=__except", "-D_Dispatch_type_(x)=",
         "-DMdlMappingNoExecute=0x40000000", f"-DDBG={int(args.debug)}", "-D_WIN64", "-D_AMD64_", "-D_M_AMD64",
         "-fno-stack-protector", "-O2", "-c", str(output / "sioctl.c"),
         "-o", str(output / "sioctl.obj")],
        [args.linker, "/machine:x64", "/entry:DriverEntry", "/subsystem:native", "/driver",
         "/nodefaultlib", "/base:0x180000000", f"/out:{output / 'sioctl.sys'}",
         str(output / "sioctl.obj"), str(output / "ntoskrnl.lib")],
    ]
    subprocess.run(commands[0], check=True, timeout=60)
    symbols = subprocess.run([args.nm, "--undefined-only", "--format=posix", str(output / "sioctl.obj")],
                             check=True, capture_output=True, text=True, timeout=30)
    exports = sorted({line.split()[0].removeprefix("__imp_")
                      for line in symbols.stdout.splitlines() if line.strip()})
    # Build the ordinary MS COFF import library from this object's dependencies.
    # This avoids MinGW archive member alignment differences; source is unchanged.
    (output / "ntoskrnl.def").write_text("LIBRARY ntoskrnl.exe\nEXPORTS\n  " + "\n  ".join(exports) + "\n")
    commands.insert(1, [args.linker, "/lib", "/machine:x64", f"/def:{output / 'ntoskrnl.def'}",
                        f"/out:{output / 'ntoskrnl.lib'}"])
    for command in commands[1:]:
        subprocess.run(command, check=True, timeout=60)
    (output / "build-commands.json").write_text(json.dumps(commands, indent=2) + "\n")
    for transfer in manifest["transfers"]:
        validate_transfer(args.neverd.resolve(), output, manifest, transfer, args.debug)
    print(f"Validated Microsoft SIOCTL: buffered, in-direct and out-direct IOCTLs, "
          f"default cleanup, close and unload (DBG={int(args.debug)}).\n{output}")


def validate_transfer(neverd: Path, output: Path, manifest: dict,
                      transfer: dict, debug: bool) -> None:
    configuration = copy.deepcopy(manifest["scenario"])
    request = configuration["requests"][1]
    expected = transfer["expected_output"].encode()
    request["code"] = transfer["code"]
    request["output_size"] = len(expected)
    if "direct_input" in transfer:
        request["direct_input"] = transfer["direct_input"].encode().hex()
    # Preserve the original artifact names for the buffered acceptance case.
    suffix = "" if transfer["name"] == "buffered" else f'-{transfer["name"]}'
    scenario = output / f"scenario{suffix}.json"
    scenario.write_text(json.dumps(configuration, indent=2) + "\n")
    run = subprocess.run([str(neverd), "emulate-driver", str(output / "sioctl.sys"),
                          "--scenario", str(scenario)], capture_output=True, text=True, timeout=30)
    (output / f"report{suffix}.json").write_text(run.stdout)
    (output / f"stderr{suffix}.txt").write_text(run.stderr)
    require(bool(run.stdout), f"No report: {run.stderr}")
    report = json.loads(run.stdout)
    require(report["stop_reason"] == "returned", f"Incomplete execution: {report['diagnostic']}")
    require(report["nt_status"] == 0 and report["unload_completed"], "Entry/unload did not complete")
    requests = report["requests"]
    require(len(requests) == 4 and all(item["completed"] for item in requests), "Incomplete requests")
    expected_statuses = [0, 0, manifest["expected_cleanup_status"], 0]
    require([item["io_status"] for item in requests] == expected_statuses, "Unexpected completion status")
    require([item["dispatch_status"] for item in requests] == expected_statuses, "Unexpected dispatch status")
    require(bytes.fromhex(requests[1]["output_hex"]) == expected, "Wrong IOCTL output")
    require(not report["devices"], "Device leaked after unload")
    # Upstream has no CLEANUP handler. The modeled default completes with
    # STATUS_INVALID_DEVICE_REQUEST, still allowing CLOSE and unload.
    require(run.returncode == 2 and report["scenario_success"] is False, "Cleanup failure was hidden")
    if debug:
        require(bool(report["messages"]), "Debug build produced no observed messages")


if __name__ == "__main__":
    main()
