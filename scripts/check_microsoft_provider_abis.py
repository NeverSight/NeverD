#!/usr/bin/env python3
"""Compare new Windows provider declarations with Microsoft's compiler ABI.

This opt-in check requires the retained per-target generation inputs. It
compares original SDK/WDK headers with the published declarations on x86, x64
and ARM64; it does not load a driver or certify native execution.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import build_wdk_driver_fixtures as wdk
import microsoft_ucrt_types as ucrt


CASES = {
    "NetworkInterfaces": ("NmrRegisterClient", "WskRegister", "WskCaptureProviderNPI"),
    "KernelStreaming": ("KsCreatePin", "KsAddEvent", "KsAcquireControl"),
    "StorageClass": ("ClassReadDriveCapacity", "ClassClaimDevice"),
    "TDI": ("TdiCopyBufferToMdl", "TdiMapUserRequest"),
    "VideoPort": ("VideoPortInitialize", "VideoPortGetDeviceBase"),
    "ATA": ("AtaPortCompleteRequest", "AtaPortGetDeviceBase"),
    "Battery": ("BatteryClassInitializeDevice", "BatteryClassIoctl"),
    "HIDPort": ("HidRegisterMinidriver", "HidP_GetCollectionDescription"),
    "SmartCard": ("SmartcardDeviceControl", "SmartcardT0Request"),
    "EventTracing": ("EventRegister", "EventWriteTransfer", "EventUnregister"),
    "Path": ("PathCchCombineEx", "PathCchCanonicalizeEx"),
    "WinRTStrings": ("WindowsCreateString", "WindowsDeleteString", "RoInitialize"),
    "RPCProxy": ("NdrDllGetClassObject", "MesDecodeBufferHandleCreate"),
    "Credentials": ("CredReadW", "CredWriteW"),
    "DeviceQuery": ("DevFindProperty", "DevCreateObjectQuery"),
    "ApplicationModel": ("GetCurrentPackageFamilyName", "GetFeatureEnabledState"),
    "PropertyVariants": ("InitPropVariantFromBuffer", "PropVariantToStringAlloc"),
    "PerformanceData": ("PdhOpenQueryW", "PdhAddCounterW"),
    "Biometrics": ("WinBioOpenSession", "WinBioCloseSession"),
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compiler_contracts(ir: str, names: tuple[str, ...]) -> dict:
    # DLL storage visibility is not a calling convention or parameter ABI.
    # Keep every width, symbol, return attribute and parameter attribute.
    contracts = ucrt.bridge_abis(ir, names)
    return {name: (symbol, prefix.removeprefix("dllimport ").removeprefix("dso_local "), parameters)
            for name, (symbol, prefix, parameters) in contracts.items()}


def generated_compiler(command: list[str]) -> list[str]:
    # Generated declarations use the compiler's freestanding standard headers.
    # Including the SDK's CRT as well would define the same records twice.
    result = []
    arguments = iter(command)
    for argument in arguments:
        if argument in ("-I", "-ivfsoverlay"):
            next(arguments)
        else:
            result.append(argument)
    return result


def check(args: argparse.Namespace) -> dict:
    args.output.mkdir(parents=True, exist_ok=False)
    rows = []
    report = {"schema": 1, "scope": "compiler ABI; no native driver execution",
              "generated_manifest_sha256": digest(args.generated / "manifest.json"),
              "cases": rows}
    for arch in args.architecture or ("x86", "x64", "arm64"):
        target = args.generated / arch
        manifest = json.loads((target / "manifest.json").read_text(encoding="utf-8"))
        for library in manifest["libraries"]:
            header = args.generated / (library["name"] + ".h")
            if digest(header) != library["sha256"]:
                raise ValueError(f"generated header changed: {header}")
            for provider in library["providers"]:
                name = provider["name"]
                if name not in CASES or args.provider and name not in args.provider:
                    continue
                names = CASES[name]
                directory = args.output / arch / name
                directory.mkdir(parents=True)
                row = {"architecture": arch, "provider": name, "functions": names,
                       "commands": [], "passed": False}
                rows.append(row)
                try:
                    command = provider["commands"][0]
                    if command[-2] != "-E":
                        raise ValueError(f"provider lacks its preprocessing command: {name}")
                    compiler = command[:-2]
                    original = Path(command[-1])
                    row["original_source_sha256"] = digest(original)
                    row["generated_header_sha256"] = digest(header)
                    # Select the published prototypes and their full type closure.
                    # Unrelated compiler intrinsic declarations in the complete
                    # catalog are not interchangeable with Clang's builtins.
                    empty = directory / "empty.h"
                    empty.write_text("", encoding="utf-8")
                    selected = directory / "selected.h"
                    selection = [args.neverd, "types", "--data-model",
                                 "ILP32" if arch == "x86" else "LLP64",
                                 "--record-abi", "microsoft", "--declare", str(empty),
                                 "--declare", str(header), "--merge-prototypes",
                                 "--all", "--prototypes", "--no-offsets",
                                 "-o", str(selected)]
                    selection += ["--import-prototype=" + symbol for symbol in names]
                    row["commands"].append(selection)
                    result = subprocess.run(selection, capture_output=True, text=True,
                                            timeout=args.timeout)
                    (directory / "selection.log").write_text(result.stderr, encoding="utf-8")
                    result.check_returncode()
                    row["selected_header_sha256"] = digest(selected)
                    references = "".join(
                        f"__typeof__(&{symbol}) neverd_ucrt_abi_{index} = &{symbol};\n"
                        for index, symbol in enumerate(names))
                    contracts = []
                    for view, include in (("original", original), ("generated", selected)):
                        source = directory / (view + ".c")
                        source.write_text('#include "' + include.as_posix() + '"\n' + references,
                                          encoding="utf-8")
                        flags = compiler if view == "original" else generated_compiler(compiler)
                        invocation = flags + ["-S", "-emit-llvm", "-O0", str(source), "-o", "-"]
                        row["commands"].append(invocation)
                        result = subprocess.run(invocation, capture_output=True, text=True,
                                                timeout=args.timeout)
                        (directory / (view + ".log")).write_text(result.stderr, encoding="utf-8")
                        result.check_returncode()
                        (directory / (view + ".ll")).write_text(result.stdout, encoding="utf-8")
                        contracts.append(compiler_contracts(result.stdout, names))
                    row["contracts"] = contracts
                    row["passed"] = contracts[0] == contracts[1]
                    if not row["passed"]:
                        row["error"] = "original and generated compiler ABIs differ"
                except (ValueError, subprocess.SubprocessError) as error:
                    row["error"] = str(error)
                (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                                          encoding="utf-8")
                print(f"{arch} {name}: {'passed' if row['passed'] else 'FAILED'}", flush=True)
    expected = set(args.provider or CASES)
    for arch in args.architecture or ("x86", "x64", "arm64"):
        if {row["provider"] for row in rows if row["architecture"] == arch} != expected:
            raise ValueError(f"provider ABI matrix is incomplete for {arch}")
    report["passed"] = bool(rows) and all(row["passed"] for row in rows)
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--architecture", choices=("x86", "x64", "arm64"), action="append")
    parser.add_argument("--provider", choices=tuple(CASES), action="append")
    parser.add_argument("--timeout", type=int, default=wdk.LIMITS["command_timeout"])
    args = parser.parse_args()
    args.generated = args.generated.resolve()
    args.output = args.output.resolve()
    if not check(args)["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
