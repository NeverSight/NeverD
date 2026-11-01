#!/usr/bin/env python3
"""Compile HighC/LLVMC calls against the original Microsoft compiler ABI.

Requires retained --ucrt SDK/WDK generation inputs and their pinned package
cache. These are import-only fixtures; this check does not execute Windows
APIs or certify native driver execution.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import zipfile

import build_wdk_driver_fixtures as wdk
from check_microsoft_provider_abis import compiler_contracts


FIXTURE = Path(__file__).resolve().parents[1] / "unittests/types/fixtures/windows_provider_calls.c"
DRIVER_FIXTURE = FIXTURE.with_name("windows_driver_provider_calls.c")
RDBSS_FIXTURE = FIXTURE.with_name("windows_rdbss_provider_calls.cpp")
FUNCTIONS = {"UseEventRegistration": "EventRegister", "UsePath": "PathCchCombineEx",
             "UseRuntimeString": "WindowsCreateString", "UseGraphicsDevice": "D3DKMTCreateDevice",
             "UseLogFile": "CreateLogFile", "UseTpmContext": "Tbsi_Context_Create",
             "UseD3D11Device": "D3D11CreateDevice",
             "UseD3D11SwapChain": "D3D11CreateDeviceAndSwapChain",
             "UseD3D12Device": "D3D12CreateDevice",
             "UseD3D12RootSignature": "D3D12SerializeVersionedRootSignature",
             "UseDXGIFactory": "CreateDXGIFactory2",
             "UseAdapterRemovalSupport": "DXGIDeclareAdapterRemovalSupport",
             "UseShaderCompile": "D3DCompile", "UseShaderCompile2": "D3DCompile2",
             "UseShaderBlob": "D3DCreateBlob"}
LIBRARIES = ("advapi32.lib", "pathcch.lib", "runtimeobject.lib", "gdi32.lib", "clfsw32.lib", "tbs.lib",
             "d3d11.lib", "d3d12.lib", "dxgi.lib", "d3dcompiler.lib")
DRIVER_FUNCTIONS = {"UseSDConfig": "SdPortGetPciConfigSpace",
                    "UseAudioAdapter": "PcAddAdapterDevice",
                    "UseStreamClock": "StreamClassQueryMasterClockSync"}
DRIVER_IMPORTS = {"sdport.sys": ("SdPortGetPciConfigSpace",),
                  "portcls.sys": ("PcAddAdapterDevice",),
                  "stream.sys": ("StreamClassQueryMasterClockSync",)}
RDBSS_FUNCTIONS = {"UseRdbssRegister": "RxRegisterMinirdr",
                   "UseRdbssStart": "RxStartMinirdr",
                   "UseRdbssUnregister": "RxpUnregisterMinirdr",
                   "UseRdbssCreateContext": "RxCreateRxContext",
                   "UseRdbssInitializeDispatcher": "RxInitializeDispatcher",
                   "UseRdbssInitializeTimer": "RxInitializeRxTimer",
                   "UseRdbssTearDownDispatcher": "RxTearDownDispatcher"}


def check(args: argparse.Namespace) -> dict:
    args.output.mkdir(parents=True, exist_ok=False)
    if args.rdbss:
        fixture, functions, provider_name = RDBSS_FIXTURE, RDBSS_FUNCTIONS, "RDBSS"
    elif args.kernel:
        fixture, functions, provider_name = DRIVER_FIXTURE, DRIVER_FUNCTIONS, "AudioPort"
    else:
        fixture, functions, provider_name = FIXTURE, FUNCTIONS, "WinRTStrings"
    report = {"schema": 1, "scope": "source compilation and compiler ABI; no native execution",
              "imports": "synthetic from original compiler symbols" if args.kernel else "Microsoft SDK",
              "fixture_sha256": wdk.digest(fixture), "cases": [], "commands": []}

    def run(command, log):
        report["commands"].append(command)
        result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
        log.write_text(result.stderr, encoding="utf-8")
        return result

    for arch in args.architecture or ("x86", "x64", "arm64"):
        directory = args.output / arch
        directory.mkdir()
        inputs = args.generated / arch
        manifest = json.loads((inputs / "manifest.json").read_text(encoding="utf-8"))
        provider = next(p for library in manifest["libraries"]
                        for p in library["providers"]
                        if p["name"] == provider_name)
        preprocessing = provider["commands"][0]
        if preprocessing[-2] != "-E":
            raise ValueError("provider has no preprocessing command")
        compiler = preprocessing[:-2]
        original_compiler = (provider["metadata"]["cpp_header_bridge"]["arguments"]
                             if args.rdbss else compiler)
        libraries = []
        if args.kernel:
            # Import-only test images need no implementation of a driver API.
            # Derive x86 decoration from the original compiler, including its
            # WDK default convention, rather than guessing a stack byte count.
            names = tuple(functions.values())
            source = directory / "imports.c"
            references = "".join(
                f"__typeof__(&{api}) neverd_ucrt_abi_{index} = &{api};\n"
                for index, api in enumerate(names))
            if args.rdbss:
                references = 'extern "C" {\n' + references + '}\n'
            source.write_text('#include "' + fixture.as_posix() + '"\n' + references,
                              encoding="utf-8")
            original = run(original_compiler + ["-S", "-emit-llvm", "-O0", str(source), "-o", "-"],
                           directory / "imports.log")
            original.check_returncode()
            source.with_suffix(".ll").write_text(original.stdout, encoding="utf-8")
            contracts = compiler_contracts(original.stdout, names)
            imports = {"rdbss.sys": tuple(functions.values())} if args.rdbss else DRIVER_IMPORTS
            for module, apis in imports.items():
                exports = []
                for api in apis:
                    symbol = contracts[api][0]
                    if arch == "x86":
                        match = re.fullmatch(r'@"\\01_(' + re.escape(api) + r'@\d+)"', symbol)
                        if not match:
                            raise ValueError(f"unexpected original driver linkage: {symbol}")
                        exports.append(match[1])
                    else:
                        if symbol != "@" + api:
                            raise ValueError(f"unexpected original driver linkage: {symbol}")
                        exports.append(api)
                definition = directory / (module + ".def")
                definition.write_text(f"LIBRARY {module}\nEXPORTS\n" + "\n".join(exports) + "\n",
                                      encoding="utf-8")
                library = definition.with_suffix(".lib")
                machine = {"x86": "i386", "x64": "i386:x86-64", "arm64": "arm64"}[arch]
                run([args.dlltool, "-m", machine, "-k", "-d", str(definition), "-l", str(library)],
                    definition.with_suffix(".log")).check_returncode()
                libraries.append(str(library))
        else:
            package, = [p for p in manifest["target_packages"] if p["architecture"] == arch]
            archive = wdk.download(args.cache, package["file"], package["url"], package["sha256"])
            with zipfile.ZipFile(archive) as source:
                members = {name.lower(): name for name in source.namelist()}
                for name in LIBRARIES:
                    member = members[f"c/um/{arch}/{name}"]
                    destination = directory / name
                    destination.write_bytes(source.read(member))
                    libraries.append(str(destination))
        obj = directory / "calls.obj"
        binary = directory / ("calls.sys" if args.kernel else "calls.dll")
        run(original_compiler + ["-O1", "-fno-stack-protector", "-fno-optimize-sibling-calls",
                        "-c", str(fixture), "-o", str(obj)], directory / "original.log").check_returncode()
        run([args.linker, "/dll", "/noentry", "/nodefaultlib", "/timestamp:0",
             *(["/driver", "/subsystem:native"] if args.kernel else []),
             "/out:" + str(binary), str(obj), *libraries], directory / "link.log").check_returncode()
        for function, api in functions.items():
            references = f"\n__typeof__(&{api}) neverd_ucrt_abi_0 = &{api};\n"
            original_references = ('extern "C" {\n' + references + '}\n'
                                   if args.rdbss else references)
            original = directory / (function + "-original.c")
            original.write_text('#include "' + fixture.as_posix() + '"\n' + original_references,
                                encoding="utf-8")
            oracle = run(original_compiler + ["-S", "-emit-llvm", "-O0", str(original), "-o", "-"],
                         original.with_suffix(".log"))
            oracle.check_returncode()
            original.with_suffix(".ll").write_text(oracle.stdout, encoding="utf-8")
            expected = compiler_contracts(oracle.stdout, (api,))
            for route in ("highc", "llvmc"):
                stem = directory / (function + "-" + route)
                command = [args.neverd, "decompile", str(binary), "--func", function]
                if route == "llvmc":
                    command.append("--llvm")
                result = run(command, stem.with_suffix(".log"))
                text = "\n".join(line for line in result.stdout.splitlines()
                                 if not line.startswith(("=== NeverD", "File:", "Arch:")))
                generated = stem.with_suffix(".c")
                generated.write_text(text + references, encoding="utf-8")
                compiled = run(compiler + ["-Werror=incompatible-pointer-types",
                                          "-Werror=incompatible-function-pointer-types",
                                          "-S", "-emit-llvm", "-O0", str(generated), "-o", "-"],
                               stem.with_suffix(".clang.log"))
                stem.with_suffix(".ll").write_text(compiled.stdout, encoding="utf-8")
                row = {"architecture": arch, "function": function, "api": api, "route": route,
                       "input_sha256": wdk.digest(binary), "decompile_exit": result.returncode,
                       "compile_exit": compiled.returncode, "original_contract": expected,
                       "passed": False}
                try:
                    actual = compiler_contracts(compiled.stdout, (api,))
                    row["generated_contract"] = actual
                    row["retains_call"] = bool(re.search(
                        r"\b(?:call|invoke)\b[^\n]*" + re.escape(actual[api][0]) + r"\(",
                        compiled.stdout))
                    row["passed"] = (result.returncode == 0 and compiled.returncode == 0
                                     and actual == expected and row["retains_call"]
                                     and "unknown value" not in text and "__builtin_trap" not in text)
                except ValueError as error:
                    row["error"] = str(error)
                report["cases"].append(row)
                (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                                          encoding="utf-8")
                print(f"{arch} {function} {route}: {'passed' if row['passed'] else 'FAILED'}", flush=True)
    report["passed"] = bool(report["cases"]) and all(row["passed"] for row in report["cases"])
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--dlltool", default="llvm-dlltool")
    parser.add_argument("--kernel", action="store_true",
                        help="check original WDK callers with synthetic import libraries")
    parser.add_argument("--rdbss", action="store_true",
                        help="check the RDBSS C++ callers (requires --kernel)")
    parser.add_argument("--architecture", choices=("x86", "x64", "arm64"), action="append")
    parser.add_argument("--timeout", type=int, default=wdk.LIMITS["command_timeout"])
    args = parser.parse_args()
    if args.rdbss and not args.kernel:
        parser.error("--rdbss requires --kernel")
    args.generated = args.generated.resolve()
    args.output = args.output.resolve()
    if not check(args)["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
