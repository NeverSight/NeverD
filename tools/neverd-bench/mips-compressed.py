#!/usr/bin/env python3
"""Compare NeverD LowIR and SLEIGH P-code on the same compressed MIPS functions."""

import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

from elftools.elf.elffile import ELFFile
import pypcode


FUNCTIONS = {
    "micromips": (
        "mips_add",
        "mips_load",
        "mips_store",
        "mips_stack",
        "mips_branch_capture",
    ),
    "mips16": (
        "m16_add",
        "m16_unsigned",
        "m16_logic",
        "m16_slot",
        "m16_memory",
        "m16_stack",
    ),
}
FIXTURES = Path(__file__).resolve().parents[2] / "unittests/lift/mips"


def assemble(assembler, family, little, scratch):
    obj = scratch / "fixture.o"
    subprocess.run(
        [
            assembler,
            "-mips32r2",
            "-32",
            "-EL" if little else "-EB",
            "-o",
            obj,
            FIXTURES / (family + ".S"),
        ],
        check=True,
    )
    with obj.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name(".symtab")
        parts = {}
        for name in FUNCTIONS[family]:
            [symbol] = symbols.get_symbol_by_name(name)
            section = elf.get_section(symbol["st_shndx"])
            start = symbol["st_value"] - section["sh_addr"]
            parts[name] = section.data()[start : start + symbol["st_size"]]
    return parts


def sleigh_spans(ctx, code):
    base = 0x10000
    decoded = [
        (i.addr.offset, i.length) for i in ctx.disassemble(code, base).instructions
    ]
    ops = ctx.translate(code, base).ops
    marks = [
        (v.offset, v.size)
        for op in ops
        if op.opcode == pypcode.OpCode.IMARK
        for v in op.inputs
    ]
    complete = (
        decoded == marks
        and decoded
        and decoded[0][0] == base
        and sum(size for _, size in decoded) == len(code)
        and decoded[-1][0] + decoded[-1][1] == base + len(code)
    )
    return complete, decoded, marks, ops


def compare(args, scratch, family, little):
    profile = family + ("el" if little else "")
    parts = assemble(args.assembler, family, little, scratch)
    if args.save_inputs:
        (args.save_inputs / (profile + "-unchanged.bin")).write_bytes(
            b"".join(parts.values())
        )
    mode = "micro" if family == "micromips" else "default"
    language = f"MIPS:{'LE' if little else 'BE'}:32:{mode}"
    ctx = pypcode.Context(language)
    ctx.setVariableDefault("ISA_MODE", 1)
    ctx.setVariableDefault("RELP", int(family == "mips16"))
    included, excluded = [], {}
    for name, fragment in parts.items():
        complete, decoded, marks, _ = sleigh_spans(ctx, fragment)
        if complete:
            included.append(name)
        else:
            excluded[name] = {"disassembly": decoded, "pcode_source_spans": marks}
    # Amortize the Python/native boundary. Both translators receive the same
    # buffer and addresses. Report functions with incompatible physical spans.
    code = b"".join(parts[name] for name in included)
    batch = code * 128
    raw = scratch / "code.bin"
    raw.write_bytes(batch)
    complete, _, marks, ops = sleigh_spans(ctx, batch)
    if not complete:
        raise ValueError(f"{profile}: incomplete SLEIGH batch coverage")

    def neverd(repeats):
        return json.loads(
            subprocess.check_output([args.bench, profile, "lift", raw, str(repeats)])
        )

    warm = neverd(1)
    if warm["instructions_per_repeat"] != len(marks):
        raise ValueError(f"{profile}: physical instruction counts differ")
    samples = {"neverd": [], "sleigh": []}
    for pair in range(args.samples):
        for translator in (
            ("neverd", "sleigh") if pair % 2 == 0 else ("sleigh", "neverd")
        ):
            if translator == "neverd":
                result = neverd(args.repeats)
                if result["digest"] != warm["digest"]:
                    raise ValueError(f"{profile}: LowIR digest changed")
                seconds = result["cpu_seconds"]
            else:
                start = time.process_time()
                for _ in range(args.repeats):
                    ctx.translate(batch, 0x10000)
                seconds = time.process_time() - start
            samples[translator].append(seconds)
    count = len(marks) * args.repeats
    return {
        "language": language,
        "sha256": hashlib.sha256(code).hexdigest(),
        "functions": included,
        "excluded": excluded,
        "instructions_per_sample": count,
        "neverd_low_ops_per_batch": warm["operations_per_repeat"],
        "sleigh_pcode_ops_per_batch": len(ops)
        - sum(op.opcode == pypcode.OpCode.IMARK for op in ops),
        "samples_cpu_seconds": samples,
        "median_instructions_per_cpu_second": {
            key: round(count / statistics.median(values))
            for key, values in samples.items()
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assembler", type=Path, required=True)
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--repeats", type=int, default=128)
    parser.add_argument(
        "--save-inputs",
        type=Path,
        help="create a new directory for the paired driver's input bytes",
    )
    args = parser.parse_args()
    if min(args.samples, args.repeats) < 1:
        parser.error("samples and repeats must be positive")
    if args.save_inputs:
        args.save_inputs.mkdir()
    with tempfile.TemporaryDirectory(prefix="neverd-mips-compressed-") as directory:
        results = {
            family
            + ("el" if little else ""): compare(args, Path(directory), family, little)
            for family in FUNCTIONS
            for little in (False, True)
        }
    print(
        json.dumps(
            {
                "pypcode": pypcode.__version__,
                "samples": args.samples,
                "repeats": args.repeats,
                "profiles": results,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
