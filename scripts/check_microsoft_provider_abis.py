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
import re
import subprocess

import build_wdk_driver_fixtures as wdk
from generate_microsoft_windows_types import PROVIDER_INLINE_ROUTINES, PROVIDER_PREFIXES
import generate_microsoft_windows_types as windows
import microsoft_gdiplus_types as gdiplus
import microsoft_flat_imports as flat
import microsoft_ucrt_types as ucrt


CASES = {
    # NDIS belongs to the primary kernel unit. Enumerate its original profile
    # instead of allowing an incomplete published header to choose the tests.
    "NDIS": (),
    "GDIPlus": (),
    **{provider: () for provider in (*flat.CURRENT, *flat.LEGACY)},
    **{provider: () for provider in PROVIDER_PREFIXES},
    "NetworkInterfaces": ("NmrRegisterClient", "WskRegister", "WskCaptureProviderNPI"),
    "KernelStreaming": ("KsCreatePin", "KsAddEvent", "KsAcquireControl"),
    "StorageClass": ("ClassReadDriveCapacity", "ClassClaimDevice"),
    "TDI": ("TdiCopyBufferToMdl", "TdiMapUserRequest"),
    "VideoPort": ("VideoPortInitialize", "VideoPortGetDeviceBase"),
    "ATA": ("AtaPortCompleteRequest", "AtaPortGetDeviceBase"),
    "Battery": ("BatteryClassInitializeDevice", "BatteryClassIoctl"),
    "HIDPort": ("HidRegisterMinidriver", "HidP_GetCollectionDescription"),
    "SmartCard": ("SmartcardDeviceControl", "SmartcardT0Request"),
    "SDPort": ("SdPortInitialize", "SdPortGetPciConfigSpace"),
    "StreamClass": ("StreamClassRegisterAdapter", "StreamClassGetPhysicalAddress"),
    "USBCamera": ("USBCAMD_DriverEntry", "USBCAMD_AdapterReceivePacket",
                  "USBCAMD_ControlVendorCommand", "USBCAMD_SelectAlternateInterface",
                  "USBCAMD_GetRegistryKeyValue", "USBCAMD_InitializeNewInterface"),
    "AudioPort": ("PcAddAdapterDevice", "PcNewPort"),
    "GraphicsKernel": ("DxgkInitialize", "DlGetDeviceBase"),
    "RDBSS": ("RxRegisterMinirdr", "RxStartMinirdr", "RxpUnregisterMinirdr",
              "RxCreateRxContext", "RxInitializeDispatcher", "RxInitializeRxTimer",
              "RxTearDownDispatcher"),
    "EventTracing": ("EventRegister", "EventWriteTransfer", "EventUnregister"),
    "Path": ("PathCchCombineEx", "PathCchCanonicalizeEx"),
    "COMRegistration": ("DllRegisterServer", "DllUnregisterServer"),
    "WinRTStrings": ("WindowsCreateString", "WindowsDeleteString", "RoInitialize"),
    "RPCProxy": ("NdrDllGetClassObject", "MesDecodeBufferHandleCreate"),
    "Credentials": ("CredReadW", "CredWriteW"),
    "DeviceQuery": ("DevFindProperty", "DevCreateObjectQuery"),
    "ApplicationModel": ("GetCurrentPackageFamilyName", "GetFeatureEnabledState"),
    "PropertyVariants": ("InitPropVariantFromBuffer", "PropVariantToStringAlloc"),
    "PerformanceData": ("PdhOpenQueryW", "PdhAddCounterW"),
    "Biometrics": ("WinBioOpenSession", "WinBioCloseSession"),
    "BluetoothGATT": ("BluetoothGATTGetServices", "BluetoothGATTGetIncludedServices",
                      "BluetoothGATTGetCharacteristics", "BluetoothGATTGetDescriptors",
                      "BluetoothGATTGetCharacteristicValue", "BluetoothGATTGetDescriptorValue",
                      "BluetoothGATTBeginReliableWrite", "BluetoothGATTSetCharacteristicValue",
                      "BluetoothGATTEndReliableWrite", "BluetoothGATTAbortReliableWrite",
                      "BluetoothGATTSetDescriptorValue", "BluetoothGATTRegisterEvent",
                      "BluetoothGATTUnregisterEvent"),
    "ConnectionManager": ("WcmQueryProperty", "WcmSetProperty", "WcmGetProfileList",
                          "WcmSetProfileList", "WcmFreeMemory"),
    "GraphicsThunk": ("D3DKMTCreateDevice", "D3DKMTDestroyDevice"),
    "Direct3D11": ("D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain"),
    "Direct3D12": ("D3D12CreateDevice", "D3D12SerializeVersionedRootSignature", "D3D12GetInterface"),
    "DXGI": ("CreateDXGIFactory2", "DXGIGetDebugInterface1", "DXGIDeclareAdapterRemovalSupport"),
    "ShaderCompiler": ("D3DCompile", "D3DCompile2", "D3DCreateBlob", "D3DDisassemble10Effect"),
    "CLFS": ("CreateLogFile", "ReserveAndAppendLog"),
    "TPM": ("Tbsi_Context_Create", "Tbsip_Submit_Command"),
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compiler_contracts(ir: str, names: tuple[str, ...]) -> dict:
    # DLL storage visibility is not a calling convention or parameter ABI.
    # Keep every width, symbol, return attribute and parameter attribute.
    return ucrt.normalized_bridge_abis(ir, names)


def generated_compiler(command: list[str], *, keep_sdk_headers: bool = False) -> list[str]:
    # Generated declarations use the compiler's freestanding standard headers.
    # Including the SDK's CRT as well would define the same records twice.
    # Their calling conventions must also stand alone: inheriting /Gz would
    # hide a lost __stdcall annotation in an otherwise compilable declaration.
    result = []
    arguments = iter(command)
    for argument in arguments:
        if not keep_sdk_headers and argument in ("-I", "-ivfsoverlay"):
            next(arguments)
        elif argument == "-Xclang":
            option = next(arguments)
            if not option.startswith("-fdefault-calling-conv="):
                result += [argument, option]
        else:
            result.append(argument)
    return result


def official_names(args: argparse.Namespace, compiler: list[str], original: Path,
                   directory: Path, row: dict, arch: str, provider: str,
                   prefixes: tuple[str, ...]) -> tuple[str, ...]:
    """Inventory declarations from the original target's official headers."""
    preprocessed = directory / (provider + "-official.h")
    command = compiler + ["-E", str(original)]
    row["commands"].append(command)
    result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
    (directory / "inventory-preprocess.log").write_text(result.stderr, encoding="utf-8")
    result.check_returncode()
    preprocessed.write_text(result.stdout, encoding="utf-8")
    row["inventory_sha256"] = digest(preprocessed)
    command = [args.neverd, "types", "--data-model", "ILP32" if arch == "x86" else "LLP64",
               "--record-abi", "microsoft", "--declare", str(preprocessed),
               "--keep-going", "--json"]
    row["commands"].append(command)
    result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
    (directory / "inventory.log").write_text(result.stderr, encoding="utf-8")
    result.check_returncode()
    (directory / "inventory.json").write_text(result.stdout, encoding="utf-8")
    parsed = json.loads(result.stdout)
    if parsed["problems"]:
        raise ValueError(f"official {provider} declarations were refused")
    names = tuple(sorted(name for name in parsed["prototypes"]
                         if name.startswith(prefixes) and
                         name not in PROVIDER_INLINE_ROUTINES.get(provider, ())))
    if not names:
        raise ValueError(f"official {provider} profile declares no functions")
    return names


def alljoyn_public_types(original, compiler, published, c_compiler, renamed, directory, row):
    """Check callback conventions and the layouts of anonymous SDK record typedefs."""
    command = compiler + ["-fsyntax-only", "-Xclang", "-ast-dump=json",
                          "-Xclang", "-ast-dump-filter=alljoyn_", str(original)]
    row["commands"].append(command)
    dump = windows.run(command, directory / "alljoyn-types.log")
    decoder = json.JSONDecoder()
    records, typedefs = {}, []
    offset = 0
    while offset < len(dump):
        if dump[offset].isspace():
            offset += 1
            continue
        node, offset = decoder.raw_decode(dump, offset)
        if node["kind"] == "CXXRecordDecl" and node.get("completeDefinition"):
            records[node["id"]] = node
        if node["kind"] == "TypedefDecl" and node["name"].startswith("alljoyn_"):
            typedefs.append(node)
    callbacks, fields = [], {}
    for node in typedefs:
        name, spelling = node["name"], node["type"]["qualType"]
        if re.search(r"\(\*\)\(", spelling):
            callbacks.append(name)
        if spelling.startswith("struct ") and "*" not in spelling:
            record, = [r for r in node["inner"] if r["kind"] == "RecordType"]
            members = [f for f in records[record["decl"]["id"]]["inner"] if f["kind"] == "FieldDecl"]
            if any(f.get("isBitfield") or not f.get("name") for f in members):
                raise ValueError("unaddressable Microsoft SDK record member: " + name)
            fields[name] = [f["name"] for f in members]
    if not fields or not callbacks:
        raise ValueError("AllJoyn compiler returned no public record or callback inventory")
    aliases = [f"neverd_sdk_callback_{i}" for i in range(len(callbacks))]
    observations = []
    for tag, header, flags in (("original", original, compiler), ("published", published, c_compiler)):
        names = renamed if tag == "published" else {}
        code = []
        for i, (typedef, alias) in enumerate(zip(callbacks, aliases)):
            name = names.get(typedef, typedef)
            code += [f"extern __typeof__(*({name})0) {alias};",
                     f"__typeof__(&{alias}) neverd_ucrt_abi_{i} = &{alias};"]
        measures = []
        for typedef, members in fields.items():
            name = names.get(typedef, typedef)
            measures += [f"sizeof({name})", f"__alignof({name})"]
            for field in members:
                measures += [f"__builtin_offsetof({name}, {field})", f"sizeof((({name} *)0)->{field})"]
        code.append("unsigned long long neverd_sdk_layout[] = {" + ",\n".join(measures) + "};")
        if tag == "original":
            code = ['extern "C" {', *code, '}']
        source = directory / ("alljoyn-types-" + tag + ".c")
        source.write_text(f'#include "{header.as_posix()}"\n' + "\n".join(code) + "\n", encoding="utf-8")
        command = flags + ["-S", "-emit-llvm", "-O0", str(source), "-o", "-"]
        row["commands"].append(command)
        ir = windows.run(command, source.with_suffix(".log"), source.with_suffix(".ll"))
        line = next((s for s in ir.splitlines() if s.startswith("@neverd_sdk_layout = ")), "")
        sizes = [int(v) for v in re.findall(r"\bi64 (\d+)", line)]
        if len(sizes) != len(measures):
            raise ValueError("incomplete AllJoyn compiler record layout: " + tag)
        observations.append((compiler_contracts(ir, aliases), sizes))
    if observations[0] != observations[1]:
        raise ValueError("AllJoyn callback ABI or record layout differs from the original SDK")
    row.update(callback_types=callbacks, record_fields=fields, layout_measurements=observations[0][1])


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
            providers = library["providers"]
            if library["name"].startswith("WindowsKernel"):
                providers = providers + [{"name": "NDIS", "commands": library["commands"]}]
            for provider in providers:
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
                    metadata = provider.get("metadata", {})
                    bridge = metadata.get("sdk_import_bridge") or metadata.get("cpp_header_bridge")
                    original = Path(bridge["source"] if bridge else command[-1])
                    row["original_source_sha256"] = digest(original)
                    if bridge and row["original_source_sha256"] != bridge["source_sha256"]:
                        raise ValueError("original SDK/WDK provider source changed")
                    row["generated_header_sha256"] = digest(header)
                    if name == "GDIPlus":
                        _, functions, invocation = gdiplus.declarations(
                            original, bridge["arguments"], directory, windows.run)
                        imports = bridge["import_library"]
                        imported = gdiplus.imported_functions(functions, imports, directory, windows.run)
                        names = tuple(gdiplus.supported(imported))
                        row["commands"] += [invocation, imports["command"]]
                        row.update(functions=names, inventory="original Microsoft C++ flat API",
                                   declarations_without_import=sorted(functions.keys() - imported.keys()),
                                   reference_parameters=sorted(imported.keys() - set(names)))
                    elif "sdk_import_bridge" in metadata:
                        declared = official_names(args, compiler, original, directory, row, arch, name, ("",))
                        inventory = dict(bridge)
                        names = tuple(flat.select(inventory, declared, directory, windows.run))
                        row.update(functions=names, inventory="original SDK headers and target import library",
                                   imports_without_declaration=inventory["imports_without_declaration"])
                    elif name == "NDIS" or name in PROVIDER_PREFIXES:
                        prefixes = ("Ndis",) if name == "NDIS" else PROVIDER_PREFIXES[name]
                        names = official_names(args, compiler, original, directory,
                                               row, arch, name, prefixes)
                        row.update(functions=names, inventory="original official target profile")
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
                    if name == "AllJoyn":
                        renamed = {r["original"]: r["name"] for r in provider["import"]["renamed"]}
                        alljoyn_public_types(original, bridge["arguments"], selected,
                                             generated_compiler(compiler), renamed, directory, row)
                    bindings = names
                    if name == "NDIS":
                        row["contract_kind"] = "function types, including inline declarations"
                        # Type-only aliases cover SDK inline functions without
                        # comparing their definitions to external declarations.
                        bindings = tuple(f"neverd_ndis_contract_{i}" for i in range(len(names)))
                        references = "".join(
                            f"extern __typeof__({symbol}) {alias};\n"
                            f"__typeof__(&{alias}) neverd_ucrt_abi_{i} = &{alias};\n"
                            for i, (symbol, alias) in enumerate(zip(names, bindings)))
                    else:
                        references = "".join(
                            f"__typeof__(&{symbol}) neverd_ucrt_abi_{index} = &{symbol};\n"
                            for index, symbol in enumerate(names))
                    contracts = []
                    for view, include in (("original", original), ("generated", selected)):
                        source = directory / (view + ".c")
                        cpp = view == "original" and bridge is not None and bridge.get("language", "c++") == "c++"
                        body = 'extern "C" {\n' + references + '}\n' if cpp else references
                        source.write_text('#include "' + include.as_posix() + '"\n' + body,
                                          encoding="utf-8")
                        if view == "generated":
                            flags = generated_compiler(compiler)
                        else:
                            flags = bridge["arguments"] if bridge else compiler
                        invocation = flags + ["-S", "-emit-llvm", "-O0", str(source), "-o", "-"]
                        row["commands"].append(invocation)
                        result = subprocess.run(invocation, capture_output=True, text=True,
                                                timeout=args.timeout)
                        (directory / (view + ".log")).write_text(result.stderr, encoding="utf-8")
                        result.check_returncode()
                        (directory / (view + ".ll")).write_text(result.stdout, encoding="utf-8")
                        compared = compiler_contracts(result.stdout, bindings)
                        contracts.append({symbol: compared[alias]
                                          for symbol, alias in zip(names, bindings)})
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
        profiles = manifest.get("profiles", {})
        if args.provider is None:
            if "com_registration" not in profiles:
                expected.discard("COMRegistration")
            if "wdk_exports" not in profiles:
                expected.difference_update(windows.WDK_EXPORT_PREFIXES)
            if "flat_imports" not in profiles:
                expected.difference_update(flat.CURRENT.keys() - PROVIDER_PREFIXES.keys())
            if "legacy_sdk" not in profiles:
                expected.difference_update(flat.LEGACY)
        if arch == "x86":
            expected.difference_update(flat.WIDE_ONLY)
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
