#!/usr/bin/env python3
"""Collect original LSE results and complete Windows ARM64 exception records."""
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
DEFINITION = FIXTURES / "AArch64AtomicOracle.def"
CASES = FIXTURES.parent / "aarch64" / "AArch64AtomicCases.def"


def contract():
    definition, cases = DEFINITION.read_text(encoding="utf-8"), CASES.read_text(encoding="utf-8")
    values = {name: int(value.removesuffix("ULL"), 0) for name, value in re.findall(
        r"NEVERD_ATOMIC_(?:ORACLE_)?VALUE\((\w+), (\w+)\)", definition + cases)}
    fields = re.findall(r"NEVERD_ATOMIC_ORACLE_FIELD\((\w+)\)", definition)
    instructions = [(name, kind, int(width), int(count)) for name, kind, width, count in re.findall(
        r"NEVERD_ATOMIC_CASE\((\w+),\s*(\w+),\s*(\d+),\s*(\d+),", cases)]
    scenarios = [(name, int(offset), values[protection], int(split), int(mismatch))
                 for name, offset, protection, split, mismatch in re.findall(
                     r"NEVERD_ATOMIC_ORACLE_SCENARIO\((\w+), (\d+), (\w+), (\d+), (\d+)\)", definition)]
    return values, fields, instructions, scenarios


def required_records(instructions, scenarios, granule):
    for index, case in enumerate(instructions):
        _, kind, width, count = case
        size = width * count
        for scenario, settings in enumerate(scenarios):
            _, offset, _, _, mismatch = settings
            if ((offset == 1 and size == granule) or (offset == 2 and size == 1)
                    or (mismatch and not kind.startswith("Compare"))):
                continue
            yield index, scenario, case, settings, granule - size + 1 if offset == 2 else offset


def initial_state(values, case, settings, offset, base):
    _, kind, width, count = case
    original = struct.pack("<QQQQ", values["Initial"], values["Operand"],
                           values["Initial"], values["Operand"])
    mask = (1 << (width * values["ByteBits"])) - 1
    old = [int.from_bytes(original[offset + n * width:offset + (n + 1) * width], "little")
           for n in range(count)]
    registers = [values["Operand"], values["InitialStatus"], values["Operand"],
                 values["Initial"], base + offset, values["InitialFlags"]]
    if kind.startswith("Compare"):
        for n in range(count):
            registers[n] = old[n] | (values["InitialStatus"] & ~mask)
        registers[0] ^= settings[4]
    return original, mask, old, registers


def successful_state(original, mask, old, registers, case, offset):
    _, kind, width, count = case
    source = registers[0] & mask
    sign = (mask + 1) // 2
    signed = lambda value: value if value < sign else value - mask - 1
    if kind.startswith("Compare"):
        match = all(old[n] == registers[n] & mask for n in range(count))
        updated = [registers[n + 2] & mask if match else old[n] for n in range(count)]
    else:
        operations = {
            "Add": lambda a, b: a + b, "Clear": lambda a, b: a & ~b,
            "Xor": lambda a, b: a ^ b, "Set": lambda a, b: a | b,
            "Swap": lambda a, b: b,
            "SignedMax": lambda a, b: max(signed(a), signed(b)),
            "SignedMin": lambda a, b: min(signed(a), signed(b)),
            "UnsignedMax": max, "UnsignedMin": min,
        }
        updated = [operations[kind](old[0], source) & mask]
    result = list(registers)
    start = 0 if kind.startswith("Compare") else 2
    result[start:start + count] = old
    after = bytearray(original)
    for n in range(count):
        after[offset + n * width:offset + (n + 1) * width] = updated[n].to_bytes(width, "little")
    return bytes(after), result


def validate_observations(data: bytes, specification=None) -> list[dict]:
    values, fields, instructions, scenarios = specification or contract()
    required = list(required_records(instructions, scenarios, values["Granule"]))
    record = struct.Struct("<" + "Q" * len(fields))
    if (not required or len(fields) != len(set(fields))
            or len(data) != record.size * len(required)):
        raise ValueError("incomplete original ARM64 atomic observations")
    observed = []
    for item, words in zip(required, record.iter_unpack(data), strict=True):
        index, scenario, case, settings, offset = item
        row = dict(zip(fields, words))
        if ((row["CaseIndex"], row["Scenario"]) != (index, scenario)
                or not row["MemoryBase"] or row["MemoryBase"] % values["Granule"]
                or not row["AtomicPC"] or row["AtomicPC"] % values["InstructionBytes"]
                or row["Faults"] not in (0, 1) or row["ParameterCount"] > values["MaxParameters"]
                or any(row[f"Parameter{i}"] for i in range(row["ParameterCount"], values["MaxParameters"]))):
            raise ValueError("inconsistent original ARM64 atomic identity")
        original, mask, old, before = initial_state(values, case, settings, offset, row["MemoryBase"])
        if row["Faults"]:
            if (row["Code"] not in (values["AccessViolation"], values["AlignmentStatus"])
                    or row["Flags"] or row["ExceptionPC"] != row["AtomicPC"]
                    or row["ContextPC"] != row["AtomicPC"]
                    or [row[f"Context{i}"] for i in range(5)] + [row["ContextFlags"]] != before
                    or settings[1] == 0 and settings[2] == values["PageReadWrite"]):
                raise ValueError("inconsistent original ARM64 atomic fault context")
            if row["Code"] == values["AccessViolation"]:
                write = settings[2] != values["PageNoAccess"]
                if (row["ParameterCount"] != 2 or row["Parameter0"] != write
                        or not before[4] <= row["Parameter1"] < before[4] + case[2] * case[3]):
                    raise ValueError("inconsistent original ARM64 atomic access violation")
            elif row["ParameterCount"]:
                raise ValueError("unexpected ARM64 atomic alignment parameters")
            after, returned = original, before
        else:
            exception_fields = ["Code", "Flags", "ParameterCount", "ExceptionPC", "ContextPC",
                                "ContextFlags"] + [f"Context{i}" for i in range(5)]
            if (any(row[field] for field in exception_fields)
                    or settings[2] != values["PageReadWrite"]):
                raise ValueError("invalid successful ARM64 atomic observation")
            after, returned = successful_state(original, mask, old, before, case, offset)
        if ([row[f"Return{i}"] for i in range(5)] + [row["ReturnFlags"]] != returned
                or struct.pack("<QQQQ", *(row[f"After{i}"] for i in range(4))) != after):
            raise ValueError("incorrect original ARM64 atomic result or memory footprint")
        observed.append({"case": case[0], "scenario": settings[0], "record": row})
    return observed


def group_digests(observations: list[dict]) -> list[tuple[str, int, str]]:
    """Remove only placement, retaining ordering, operands, faults and RAM."""
    values, fields, cases, _ = contract()
    kinds = {name: kind for name, kind, *_ in cases}
    groups = {}
    for observation in observations:
        kind = kinds[observation["case"]]
        row = dict(observation["record"])
        row["Return4"] -= row["MemoryBase"]
        if row["Faults"]:
            row["Context4"] -= row["MemoryBase"]
        if row["Code"] == values["AccessViolation"]:
            row["Parameter1"] -= row["MemoryBase"]
        for field in ("MemoryBase", "AtomicPC", "ExceptionPC", "ContextPC"):
            row[field] = 0
        count, digest = groups.setdefault(kind, [0, hashlib.sha256()])
        digest.update(struct.pack("<" + "Q" * len(fields), *(row[field] for field in fields)))
        groups[kind][0] = count + 1
    return [(kind, count, digest.hexdigest()) for kind, (count, digest) in groups.items()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    definition = DEFINITION.read_text(encoding="utf-8")
    settings = dict(re.findall(r'NEVERD_ATOMIC_ORACLE_TEXT\((\w+), "([^"]+)"\)', definition))
    apis = re.findall(r"NEVERD_ATOMIC_ORACLE_API\((\w+)\)", definition)
    values, _, _, _ = contract()
    sources = [DEFINITION, CASES, FIXTURES / "aarch64_atomic.inc",
               FIXTURES / "aarch64_atomic_oracle.c", Path(__file__).resolve()]
    report = {"native_observed": False, "status": "failed", "commands": [],
              "host": {"system": platform.system(), "machine": platform.machine()},
              "sources": {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in sources}}

    def tool(name):
        found = shutil.which(name)
        if found:
            return found
        candidate = Path(os.environ.get("ProgramFiles", "")) / "LLVM/bin" / (name + ".exe")
        if candidate.is_file():
            return str(candidate)
        raise RuntimeError(f"required fixture tool unavailable: {name}")

    def run(command, name, timeout=60):
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
        library, obj = output / "provider.lib", output / "atomic.obj"
        run([link, "/lib", "/machine:" + settings["Machine"], "/def:" + str(provider),
             "/out:" + str(library)], "imports")
        run([clang, "--target=" + settings["Target"], "-std=c11", "-O1", "-ffreestanding",
             "-fno-builtin", "-fno-stack-protector", "-fno-vectorize", "-fno-slp-vectorize",
             "-c", str(FIXTURES / "aarch64_atomic_oracle.c"), "-o", str(obj)], "compile")
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
            raise RuntimeError("native atomic evidence requires a Windows ARM64 host")
        result = run([str(program)], "native", values["NativeTimeoutSeconds"])
        if result.stderr:
            raise ValueError("unexpected native atomic diagnostic output")
        report["observations"] = validate_observations(result.stdout)
        report["group_digests"] = group_digests(report["observations"])
        report["native_observed"] = True
        report["status"] = "passed"
    finally:
        (output / "observations.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({key: report[key] for key in ("status", "native_observed", "host")}))


if __name__ == "__main__":
    main()
