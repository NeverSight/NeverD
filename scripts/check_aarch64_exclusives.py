#!/usr/bin/env python3
"""Build and verify original exclusive instruction loops on native Windows ARM64."""
from __future__ import annotations

import argparse
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
DEFINITION = FIXTURES / "AArch64ExclusiveOracle.def"
CASES = FIXTURES.parent / "aarch64" / "AArch64ExclusiveCases.def"


def validate_observations(data: bytes, cases: list[str]) -> list[dict]:
    if not cases or len(set(cases)) != len(cases) or len(data) != len(cases) * 4:
        raise ValueError("incomplete original ARM64 exclusive observations")
    values = struct.unpack("<" + "I" * len(cases), data)
    if any(value != 1 for value in values):
        raise ValueError("an original ARM64 exclusive instruction check failed")
    return [{"case": case, "passed": True} for case in cases]


def oracle_values() -> dict[str, int]:
    return {name: int(value.removesuffix("ULL"), 0) for name, value in re.findall(
        r"NEVERD_EXCLUSIVE_ORACLE_(?:VALUE|WIDE)\((\w+), (\w+)\)", DEFINITION.read_text(encoding="utf-8"))}


def validate_alignment_observations(data: bytes, cases: list[tuple[str, int, int]],
                                    fields: list[str], initial: int, updated: int,
                                    status: int) -> list[dict]:
    values = oracle_values()
    record = struct.Struct("<" + "Q" * len(fields))
    required = [(index, name, width, count, offset, mode, scenario)
                for index, (name, width, count) in enumerate(cases)
                for offset in range(1, width * count)
                for mode in range(values["ModeCount"])
                for scenario in range(values["MemoryScenarioCount"])]
    if not fields or len(set(fields)) != len(fields) or len(data) != len(required) * record.size:
        raise ValueError("incomplete original ARM64 alignment observations")
    observations = []
    original = struct.pack("<QQQQ", initial, updated, initial, updated)
    maximum = values["MaxParameters"]
    for expected, words in zip(required, record.iter_unpack(data), strict=True):
        index, name, width, count, offset, mode, scenario = expected
        row = dict(zip(fields, words))
        split = scenario >= values["SplitReadOnlyMemory"]
        base_offset = values["WindowAlignment"] - width * count if split else 0
        store_offset = base_offset + offset
        load_offset = base_offset if mode == values["AlignedLoadStore"] else store_offset
        if ((row["CaseIndex"], row["Offset"], row["Mode"], row["MemoryScenario"])
                != (index, offset, mode, scenario)
                or not row["MemoryBase"] or row["MemoryBase"] % values["WindowAlignment"]
                or row["Faults"] not in (0, 1) or row["FaultStage"] not in (0, 1)
                or not row["LoadPC"] or not row["StorePC"]
                or row["LoadPC"] == row["StorePC"]
                or row["LoadPC"] % values["InstructionBytes"]
                or row["StorePC"] % values["InstructionBytes"]
                or row["ParameterCount"] > maximum
                or any(row[f"Parameter{i}"] for i in range(row["ParameterCount"], maximum))):
            raise ValueError("inconsistent original ARM64 alignment identity")
        fault = bool(row["Faults"])
        load_fault = fault and not row["FaultStage"]
        store_fault = fault and bool(row["FaultStage"])
        if fault:
            pc = row["StorePC"] if store_fault else row["LoadPC"]
            low, high = (updated, initial) if store_fault else (initial, updated)
            if (not row["Code"] or row["FaultPC"] != pc or row["ExceptionPC"] != pc
                    or row["ContextPC"] != pc or row["ContextLow"] != low
                    or row["ContextHigh"] != high or row["ContextStatus"] != status
                    or row["ContextAddress"] != row["MemoryBase"] +
                        (store_offset if store_fault else load_offset)
                    or (load_fault and mode == values["StoreOnly"])
                    or (store_fault and mode == values["LoadOnly"])):
                raise ValueError("inconsistent original ARM64 fault context")
        elif any(row[field] for field in ["Code", "Flags", "ParameterCount", "FaultPC",
                                          "ExceptionPC", "ContextPC", "ContextLow",
                                          "ContextHigh", "ContextStatus", "ContextAddress", "FaultStage"]):
            raise ValueError("exception metadata without an original ARM64 fault")
        loaded = [initial, updated]
        if mode != values["StoreOnly"] and not load_fault:
            if (scenario == values["NoAccessMemory"]
                    or (scenario == values["SplitNoAccessMemory"]
                        and mode != values["AlignedLoadStore"])):
                raise ValueError("an original ARM64 load bypassed inaccessible RAM")
            for part in range(count):
                at = load_offset + part * width
                loaded[part] = int.from_bytes(original[at:at + width], "little")
        if [row["LoadedLow"], row["LoadedHigh"]] != loaded:
            raise ValueError("incorrect original ARM64 exclusive load")
        store = mode != values["LoadOnly"] and not load_fault
        returned = [updated, initial] if store else loaded
        result = row["ReturnStatus"]
        if ([row["ReturnLow"], row["ReturnHigh"]] != returned
                or (result != status if fault or not store else result not in (0, 1))):
            raise ValueError("incorrect original ARM64 exclusive return state")
        after = bytearray(original)
        if store:
            source = updated.to_bytes(8, "little")[:width]
            if count == 2:
                source += initial.to_bytes(8, "little")[:width]
        if store and not fault and result == 0:
            if scenario != values["ReadWriteMemory"]:
                raise ValueError("an original ARM64 store bypassed unwritable RAM")
            after[store_offset:store_offset + len(source)] = source
        actual = struct.pack("<QQQQ", *(row[f"After{i}"] for i in range(values["MemoryWordCount"])))
        # A kernel fixup could commit a prefix before its later access fails.
        # Preserve that observation, but never accept writes outside the
        # instruction's writable operand bytes or values it did not supply.
        prefix = (max(0, values["WindowAlignment"] - store_offset) if split
                  else len(source) if scenario == values["ReadWriteMemory"] and store else 0)
        allowed_prefix = False
        if store_fault:
            for length in range(prefix + 1):
                partial = bytearray(original)
                partial[store_offset:store_offset + length] = source[:length]
                allowed_prefix |= actual == partial
        if actual != after and not allowed_prefix:
            raise ValueError("incorrect original ARM64 exclusive store footprint")
        observations.append({"case": name, "record": row})
    return observations


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    definition = DEFINITION.read_text(encoding="utf-8")
    settings = dict(re.findall(r'NEVERD_EXCLUSIVE_ORACLE_TEXT\((\w+), "([^"]+)"\)', definition))
    values = dict(re.findall(r"NEVERD_EXCLUSIVE_ORACLE_(?:VALUE|WIDE)\((\w+), (\w+)\)", definition))
    apis = re.findall(r"NEVERD_EXCLUSIVE_ORACLE_API\((\w+)\)", definition)
    case_text = CASES.read_text(encoding="utf-8")
    cases = re.findall(r"^NEVERD_EXCLUSIVE_CASE\((\w+),[^\n]+, (\d+), (\d+)\)$", case_text, re.M)
    cases = [(name, int(width), int(count)) for name, width, count in cases]
    seeds = dict(re.findall(r"NEVERD_EXCLUSIVE_VALUE\((\w+), (\w+)\)", case_text))
    fields = re.findall(r"NEVERD_EXCLUSIVE_ORACLE_FIELD\((\w+)\)", definition)
    sources = [DEFINITION, CASES, FIXTURES / "aarch64_exclusive.inc",
               FIXTURES / "aarch64_exclusive_oracle.c", Path(__file__).resolve()]
    report = {"native_observed": False, "status": "failed", "commands": [],
              "host": {"system": platform.system(), "machine": platform.machine()},
              "sources": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in sources}}

    def tool(name: str) -> str:
        found = shutil.which(name)
        if found:
            return found
        candidate = Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin" / (name + ".exe")
        if candidate.is_file():
            return str(candidate)
        raise RuntimeError(f"required fixture tool unavailable: {name}")

    def run(command: list[str], name: str, timeout: int = 60) -> subprocess.CompletedProcess:
        report["commands"].append(command)
        result = subprocess.run(command, cwd=output, capture_output=True, timeout=timeout, check=False)
        (output / (name + ".stdout")).write_bytes(result.stdout)
        (output / (name + ".stderr")).write_bytes(result.stderr)
        report[name + "_returncode"] = result.returncode
        result.check_returncode()
        return result

    try:
        report["commit"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        report["source_dirty"] = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True).strip())
        clang, link = tool("clang"), tool("lld-link")
        provider = output / "provider.def"
        provider.write_text("LIBRARY " + settings["Provider"] + "\nEXPORTS\n" +
                            "".join("  " + api + "\n" for api in apis), encoding="utf-8")
        library, obj = output / "provider.lib", output / "exclusive.obj"
        run([link, "/lib", "/machine:" + settings["Machine"], "/def:" + str(provider),
             "/out:" + str(library)], "imports")
        run([clang, "--target=" + settings["Target"], "-std=c11", "-O1", "-ffreestanding",
             "-fno-builtin", "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
             "-c", str(FIXTURES / "aarch64_exclusive_oracle.c"), "-o", str(obj)], "compile")
        program = output / settings["Program"]
        run([link, "/nodefaultlib", "/subsystem:console", "/machine:" + settings["Machine"],
             "/timestamp:0", "/entry:" + settings["Entry"], str(obj), str(library),
             "/out:" + str(program)], "link")
        report["binary_sha256"] = hashlib.sha256(program.read_bytes()).hexdigest()
        if args.build_only:
            report["status"] = "built_only"
            return
        if report["source_dirty"]:
            raise RuntimeError("native evidence requires committed clean sources")
        if platform.system() != "Windows" or platform.machine().lower() not in ("arm64", "aarch64"):
            raise RuntimeError("native exclusive evidence requires a Windows ARM64 host")
        result = run([str(program)], "native", int(values["NativeTimeoutSeconds"]))
        if result.stderr:
            raise ValueError("unexpected native exclusive diagnostic output")
        size = len(cases) * 4
        report["observations"] = validate_observations(result.stdout[:size], [name for name, _, _ in cases])
        report["alignment_observations"] = validate_alignment_observations(
            result.stdout[size:], cases, fields, int(seeds["Initial"].removesuffix("ULL"), 0),
            int(seeds["Updated"].removesuffix("ULL"), 0), oracle_values()["StatusSeed"])
        report["native_observed"] = True
        report["status"] = "passed"
    finally:
        (output / "observations.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report[key] for key in ("status", "native_observed", "host")}))


if __name__ == "__main__":
    main()
