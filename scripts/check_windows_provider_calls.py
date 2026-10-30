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
import subprocess
import zipfile

import build_wdk_driver_fixtures as wdk
from check_microsoft_provider_abis import compiler_contracts


FIXTURE = Path(__file__).resolve().parents[1] / "unittests/types/fixtures/windows_provider_calls.c"
FUNCTIONS = {"UseEventRegistration": "EventRegister", "UsePath": "PathCchCombineEx",
             "UseRuntimeString": "WindowsCreateString"}
LIBRARIES = ("advapi32.lib", "pathcch.lib", "runtimeobject.lib")


def check(args: argparse.Namespace) -> dict:
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"schema": 1, "scope": "source compilation and compiler ABI; no native execution",
              "fixture_sha256": wdk.digest(FIXTURE), "cases": [], "commands": []}

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
                        for p in library["providers"] if p["name"] == "WinRTStrings")
        preprocessing = provider["commands"][0]
        if preprocessing[-2] != "-E":
            raise ValueError("provider has no preprocessing command")
        compiler = preprocessing[:-2]
        package, = [p for p in manifest["target_packages"] if p["architecture"] == arch]
        archive = wdk.download(args.cache, package["file"], package["url"], package["sha256"])
        libraries = []
        with zipfile.ZipFile(archive) as source:
            members = {name.lower(): name for name in source.namelist()}
            for name in LIBRARIES:
                member = members[f"c/um/{arch}/{name}"]
                destination = directory / name
                destination.write_bytes(source.read(member))
                libraries.append(str(destination))
        obj, binary = directory / "calls.obj", directory / "calls.dll"
        run(compiler + ["-O1", "-fno-stack-protector", "-fno-optimize-sibling-calls",
                        "-c", str(FIXTURE), "-o", str(obj)], directory / "original.log").check_returncode()
        run([args.linker, "/dll", "/noentry", "/nodefaultlib", "/timestamp:0",
             "/out:" + str(binary), str(obj), *libraries], directory / "link.log").check_returncode()
        for function, api in FUNCTIONS.items():
            references = f"\n__typeof__(&{api}) neverd_ucrt_abi_0 = &{api};\n"
            original = directory / (function + "-original.c")
            original.write_text('#include "' + FIXTURE.as_posix() + '"\n' + references,
                                encoding="utf-8")
            oracle = run(compiler + ["-S", "-emit-llvm", "-O0", str(original), "-o", "-"],
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
                    row["passed"] = (result.returncode == 0 and compiled.returncode == 0
                                     and actual == expected and api + "(" in text
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
    parser.add_argument("--architecture", choices=("x86", "x64", "arm64"), action="append")
    parser.add_argument("--timeout", type=int, default=wdk.LIMITS["command_timeout"])
    args = parser.parse_args()
    args.generated = args.generated.resolve()
    args.output = args.output.resolve()
    if not check(args)["passed"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
