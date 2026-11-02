#!/usr/bin/env python3
"""Observe original Windows SIMD faults across exception masks and sticky flags."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/emulation/fixtures"
DEFINITION = FIXTURES / "WindowsSIMDCases.def"
CASES = FIXTURES.parent / "x86_64" / "X64SIMDExceptionCases.def"


def definitions() -> tuple[dict, dict, list, list]:
    text = DEFINITION.read_text(encoding="utf-8")
    values = {name: int(value.removesuffix("ULL"), 0) for name, value in re.findall(
        r"^NEVERD_WINDOWS_SIMD_(?:VALUE|WIDE)\((\w+), (\w+)\)$", text, re.M)}
    settings = dict(re.findall(r'^NEVERD_WINDOWS_SIMD_TEXT\((\w+), "([^"]*)"\)$',
                               text, re.M))
    operations = [(name, int(status)) for name, status in re.findall(
        r"^NEVERD_SIMD_CASE\((\w+), (\d+),", CASES.read_text(encoding="utf-8"), re.M)]
    fields = re.findall(r"^NEVERD_WINDOWS_SIMD_FIELD\((\w+)\)$", text, re.M)
    if (not values or not settings or not operations or not fields
            or len({name for name, _ in operations}) != len(operations)
            or len(set(fields)) != len(fields)):
        raise ValueError("invalid Windows SIMD observation inventory")
    return values, settings, operations, fields


def parse_observations(data: bytes) -> list[dict]:
    values, _, operations, fields = definitions()
    record = struct.Struct("<" + "Q" * len(fields))
    if len(data) != record.size * len(operations) * 2 * values["MaskCount"] * 2 * values["ModeCount"]:
        raise ValueError("missing or extra Windows SIMD observations")
    records = []
    parameters = [field for field in fields if re.fullmatch(r"Parameter\d+", field)]
    fault_fields = ["Code", "Flags", "ParameterCount", "ExceptionPC", "ContextPC",
                    "ContextFlags", "ContextMXCSR", "ContextFXMXCSR", *parameters]
    fault_fields += [field for field in fields if field.startswith(("Handler", "Continue"))]
    for index, words in enumerate(record.iter_unpack(data)):
        row = dict(zip(fields, words))
        quotient, mode = divmod(index, values["ModeCount"])
        quotient, sticky = divmod(quotient, 2)
        quotient, unmask = divmod(quotient, values["MaskCount"])
        op, memory = divmod(quotient, 2)
        if tuple(row[key] for key in ("Operation", "Memory", "Unmask", "Sticky", "Mode")) != (
                op, memory, unmask, sticky, mode):
            raise ValueError("Windows SIMD observation order changed")
        expected = ((values["DefaultMXCSR"] & ~(unmask << values["MaskShift"]))
                    | (values["Sticky"] if sticky else 0))
        if row["BeforeMXCSR"] != expected or not row["FaultPC"] or row["ResumePC"] <= row["FaultPC"]:
            raise ValueError("Windows SIMD observation has inconsistent input state")
        if row["Traps"] not in (0, 1) or (not unmask and row["Traps"]) or (
                unmask & operations[op][1] and not row["Traps"]):
            raise ValueError("Windows SIMD trap count contradicts the original instruction")
        if row["Traps"]:
            if row["Continues"] != 1 or row["ContinueContextPC"] != row[
                    "ResumePC" if mode == values["ModeSkip"] else "FaultPC"]:
                raise ValueError("Windows SIMD continuation differs from its repair mode")
            if not row["Code"] or row["ParameterCount"] > len(parameters):
                raise ValueError("invalid Windows SIMD exception record")
            if not row["FaultPC"] == row["ExceptionPC"] == row["ContextPC"]:
                raise ValueError("Windows SIMD exception PC differs from its original fault site")
            if any(row[key] for key in parameters[row["ParameterCount"]:]):
                raise ValueError("undefined exception parameters were not normalized")
        elif any(row[key] for key in fault_fields):
            raise ValueError("nonfaulting Windows SIMD observation contains an exception record")
        records.append({"operation": operations[op][0], "record": row})
    return records


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    values, settings, _, _ = definitions()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"status": "failed", "native_windows_observed": False,
              "host": {"system": platform.system(), "machine": platform.machine(),
                       "version": platform.version()}, "commands": []}

    def git(*arguments: str) -> str:
        return subprocess.check_output(["git", *arguments], cwd=ROOT, text=True).strip()

    def run(command: list[str], prefix: str, timeout: int) -> subprocess.CompletedProcess:
        report["commands"].append(command)
        try:
            result = subprocess.run(command, cwd=output, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            (output / (prefix + ".stdout")).write_bytes(error.stdout or b"")
            (output / (prefix + ".stderr")).write_bytes(error.stderr or b"")
            raise
        (output / (prefix + ".stdout")).write_bytes(result.stdout)
        (output / (prefix + ".stderr")).write_bytes(result.stderr)
        report[prefix + "_returncode"] = result.returncode
        result.check_returncode()
        return result

    try:
        report["commit"] = git("rev-parse", "HEAD")
        report["source_dirty"] = bool(git("status", "--porcelain"))
        if report["source_dirty"]:
            raise ValueError("native observations require committed clean sources")
        native = platform.system() == "Windows"
        if not args.build_only and (not native or platform.machine().lower() not in {"amd64", "x86_64"}):
            raise ValueError("native observations require an x64 Windows host")
        paths = [str(Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin"), os.environ.get("PATH", "")]

        def tool(name: str) -> str:
            found = shutil.which(name, path=os.pathsep.join(paths))
            if not found:
                raise ValueError(f"required fixture tool unavailable: {name}")
            return found

        clang, link = tool("clang"), tool("lld-link")
        sources = [DEFINITION, CASES, FIXTURES / "WindowsSIMDStatusCases.def",
                   FIXTURES / "windows_simd.c", FIXTURES / "windows_simd.S",
                   Path(__file__).resolve()]
        report["source_sha256"] = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in sources}
        apis = re.findall(r"^NEVERD_WINDOWS_SIMD_API\((\w+)\)$", DEFINITION.read_text(), re.M)
        provider = output / "provider.def"
        provider.write_text("LIBRARY " + settings["Provider"] + "\nEXPORTS\n" +
                            "".join("  " + api + "\n" for api in apis), encoding="utf-8")
        timeout = values["BuildTimeoutSeconds"]
        run([clang, "--version"], "compiler", timeout)
        run([link, "--version"], "linker", timeout)
        run([link, "/lib", "/machine:x64", "/def:" + str(provider),
             "/out:" + str(output / "provider.lib")], "imports", timeout)
        objects = []
        for suffix in ("c", "S"):
            obj = output / ("simd-" + suffix + ".obj")
            flags = ["-std=c11", "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                     "-fno-vectorize", "-fno-slp-vectorize", "-O1", "-Wall", "-Wextra", "-Werror"] if suffix == "c" else []
            run([clang, "--target=" + settings["WindowsTarget" if native else "CrossTarget"],
                 *flags, "-c", str(FIXTURES / ("windows_simd." + suffix)), "-o", str(obj)],
                "compile-" + suffix, timeout)
            objects.append(str(obj))
        program = output / settings["Program"]
        run([link, "/nodefaultlib", "/subsystem:console", "/machine:x64", "/timestamp:0",
             "/entry:" + settings["Entry"], *objects, str(output / "provider.lib"),
             "/out:" + str(program)], "link", timeout)
        report["program_sha256"] = hashlib.sha256(program.read_bytes()).hexdigest()
        if args.build_only:
            report["status"] = "built_only"
        else:
            result = run([str(program)], "observe", values["TimeoutSeconds"])
            if result.stderr:
                raise ValueError("native observation emitted unexpected stderr")
            observations = parse_observations(result.stdout)
            (output / "observations.json").write_text(json.dumps(observations, indent=2) + "\n", encoding="utf-8")
            report["observations"] = len(observations)
            report["exception_codes"] = dict(Counter(
                f'{row["record"]["Code"]:08x}' for row in observations if row["record"]["Traps"]))
            report["native_windows_observed"] = True
            report["status"] = "observed"
        report["final_commit"] = git("rev-parse", "HEAD")
        report["final_source_dirty"] = bool(git("status", "--porcelain"))
        if report["final_commit"] != report["commit"] or report["final_source_dirty"]:
            raise ValueError("source changed during native observation")
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["native_windows_observed"] = False
        report["error"] = str(error)
    (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    if report["status"] == "failed":
        raise SystemExit(1)


if __name__ == "__main__":
    main()
