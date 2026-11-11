#!/usr/bin/env python3
"""Cross-check each micro ASE lift and run the physical DSP forms on GNU sim."""

import argparse
import json
from pathlib import Path
import runpy
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import mips_const as mips

import roundtrip

COVERAGE = runpy.run_path(str(Path(__file__).with_name("profile-encoding-coverage.py")))
ENABLE_DSP = (0x40086000, 0x3C090100, 0x01094025, 0x40886000, 0xC0)
SEED_AC2 = (0x7C81FCF8, 0x00A01011, 0x00C01013)
READ_STATE = (0x7C3F44B8, 0x00404810, 0x00405012)
SAMPLE = bytes((17 + 29 * i) & 255 for i in range(16))
SUCCESS = 211


def case_state(instruction, variant):
    a0, a1 = [(0x80018003, 0x7FFF8000), (0x7FFF0007, 0x80FF7F03)][variant]
    control = [0x0100018C, 0x02000194][variant]
    mnemonic = instruction.split()[0]
    indexed = mnemonic in {"lbux", "lhx", "lwx"}
    if indexed:
        a0, a1 = roundtrip.DATA, 4
    if mnemonic == "bposge32":
        control = (control & ~63) | (31 + variant)
    return a0, a1, control, indexed


def oracle(elf, instructions, little, variant):
    uc, entry = roundtrip.emulator(
        elf, 32, little, "entry", cpu_model=mips.UC_CPU_MIPS32_74KF
    )
    uc.mem_write(roundtrip.DATA, SAMPLE)

    def execute(offset, words):
        address = roundtrip.STOP + offset
        order = "little" if little else "big"
        uc.mem_write(address, b"".join(w.to_bytes(4, order) for w in words))
        uc.emu_start(address, address + 4 * len(words), count=len(words))

    execute(0x200, ENABLE_DSP)
    rows = []
    for i, instruction in enumerate(instructions):
        a0, a1, control, _ = case_state(instruction, variant)
        for reg, value in [(4, control), (5, 0x80007FFF), (6, 0x80FF8000)]:
            uc.reg_write(mips.UC_MIPS_REG_0 + reg, value)
        execute(0x300, SEED_AC2)
        for reg, value in [(2, 0x89ABCDEF), (4, a0), (5, a1), (14, 1)]:
            uc.reg_write(mips.UC_MIPS_REG_0 + reg, value)
        if instruction.startswith("bposge32 "):
            # Distinguish taking the branch from falling through its NOP slot.
            execute(0x500, (0x041C0002, 0, 0x340E0000))
        else:
            pc = entry + 8 * i
            uc.emu_start(pc, pc + 4, count=1)
        execute(0x400, READ_STATE)
        rows.append(
            [uc.reg_read(mips.UC_MIPS_REG_0 + reg) for reg in (2, 5, 8, 9, 10, 14)]
        )
    return rows


def run_micro(args, work, feature, instructions, rows, little, variant, bits=32):
    source, obj, elf = (work / n for n in ("probe.s", "probe.o", "probe.elf"))
    asm = [
        ".text",
        ".set noreorder",
        ".set micromips",
        ".set insn32",
        ".globl _start",
        ".ent _start",
        "_start:",
    ]
    for i, (insn, expected) in enumerate(zip(instructions, rows, strict=True)):
        a0, a1, control, indexed = case_state(insn, variant)
        asm.extend(
            [
                f"li $4,{control}",
                "wrdsp $4,63",
                "li $5,0x80007fff",
                "mthi $5,$ac2",
                "li $6,0x80ff8000",
                "mtlo $6,$ac2",
                "li $2,0x89abcdef",
                (
                    ("dla $4,sample" if bits == 64 else "la $4,sample")
                    if indexed
                    else f"li $4,{a0}"
                ),
                f"li $5,{a1}",
                "li $14,1",
            ]
        )
        if insn.startswith("bposge32 "):
            asm.extend(["bposge32 1f", "nop", "ori $14,$0,0", "1:"])
        else:
            asm.append(insn)
        asm.extend(["rddsp $8,63", "mfhi $9,$ac2", "mflo $10,$ac2"])
        for reg, value in zip((2, 5, 8, 9, 10, 14), expected, strict=True):
            asm.extend(
                [
                    f"{'dli' if bits == 64 else 'li'} $11,{value}",
                    f"bne ${reg},$11,failed_{i}",
                    "nop",
                ]
            )
    asm.extend([f"li $4,{SUCCESS}", "break 0x3ff"])
    for i in range(len(instructions)):
        asm.extend([f"failed_{i}:", f"li $4,{i + 1}", "break 0x3ff"])
    asm.extend(
        [
            ".end _start",
            ".data",
            ".balign 8",
            "sample:",
            ".byte " + ",".join(map(str, SAMPLE)),
        ]
    )
    source.write_text("\n".join(asm) + "\n")
    subprocess.run(
        [
            args.assembler,
            f"-mips{bits}r2",
            f"-{bits}",
            "-m" + feature,
            "-EL" if little else "-EB",
            "-o",
            obj,
            source,
        ],
        check=True,
    )
    subprocess.run(
        [
            args.linker,
            "-m",
            f"elf{bits}" + ("l" if little else "b") + "tsmip",
            "-N",
            "-Ttext",
            "0xffffffff80010000" if bits == 64 else "0x80010000",
            "-e",
            "_start",
            "-o",
            elf,
            obj,
        ],
        check=True,
    )
    with elf.open("rb") as stream:
        image = ELFFile(stream)
        if not (image["e_flags"] & 0x02000000 and image["e_entry"] & 1):
            raise ValueError("GNU probe has no microMIPS entry point")
    result = subprocess.run(
        [args.simulator, "--endian", "little" if little else "big", elf],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if result.returncode != SUCCESS:
        index = result.returncode - 1
        mismatch = (
            instructions[index]
            if not result.stderr and 0 <= index < len(instructions)
            else ""
        )
        raise ValueError(
            f"{feature} {'LE' if little else 'BE'} input {variant}: "
            f"simulator exit {result.returncode} {mismatch} {result.stderr}"
        )


def compare_lifts(args, work, raw_classic, raw_micro, profile, little, instructions):
    from_classic = raw_classic.read_bytes()
    from_micro = raw_micro.read_bytes()
    pair = work / "instruction-and-slot.bin"
    name = profile + ("el" if little else "")
    for i, insn in enumerate(instructions):
        lifted = []
        for cpu, stream in ((name, from_classic), ("micro" + name, from_micro)):
            pair.write_bytes(stream[8 * i : 8 * i + 8])
            result = json.loads(
                subprocess.check_output([args.bench, cpu, "lift", pair, "1"])
            )
            if result["instructions_per_repeat"] != 2:
                raise ValueError(f"{cpu} {insn}: incomplete instruction pair")
            lifted.append((result["operations_per_repeat"], result["digest"]))
        if lifted[0] != lifted[1]:
            raise ValueError(f"{name} {insn}: LowIR differs between classic and micro")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("assembler", "linker", "simulator", "bench"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--bits", type=int, choices=(32, 64), default=32)
    args = parser.parse_args()
    results = {"lowir_form_pairs": {}, "physical_dsp_executions": {}}
    with tempfile.TemporaryDirectory(prefix="neverd-micro-dsp-") as directory:
        work = Path(directory)
        classic, micro = work / "classic", work / "micro"
        classic.mkdir()
        micro.mkdir()
        for profile, cpu, feature, instructions in COVERAGE["corpora"]():
            if args.bits == 64:
                if profile not in {"micromips64dsp", "micromips64dspr2"}:
                    continue
                profile = profile.removeprefix("micro")
            elif profile not in {"mipsdsp", "mipsdspr2", "mipsmsa"}:
                continue
            for little in (False, True):
                raw = COVERAGE["assemble"](
                    args, classic, profile, cpu, feature, instructions, little
                )
                compressed = COVERAGE["assemble"](
                    args, micro, "micro" + profile, cpu, feature, instructions, little
                )
                compare_lifts(
                    args, work, raw, compressed, profile, little, instructions
                )
                name = "micro" + profile + ("el" if little else "")
                results["lowir_form_pairs"][name] = len(instructions)
                if feature == "msa":
                    continue
                reference = raw.with_name("fixture.elf")
                if args.bits == 64:
                    # These DSP 3.02 instructions consume word views of AC halves.
                    # The historical MIPS64DSPR2 core implements older 128-bit
                    # accumulators and an eight-bit CCOND. Use the 74Kf for the
                    # word results, then verify the full GPR-width sign extensions
                    # by executing the physical microMIPS64 ELF in GNU sim.
                    word = work / "word"
                    word.mkdir(exist_ok=True)
                    reference = COVERAGE["assemble"](
                        args,
                        word,
                        "mips" + feature,
                        "mips32r2",
                        feature,
                        instructions,
                        little,
                    ).with_name("fixture.elf")
                for variant in range(2):
                    expected = oracle(reference, instructions, little, variant)
                    if args.bits == 64:
                        expected = [
                            [
                                v | 0xFFFFFFFF00000000 if v & 0x80000000 else v
                                for v in row
                            ]
                            for row in expected
                        ]
                    run_micro(
                        args,
                        work,
                        feature,
                        instructions,
                        expected,
                        little,
                        variant,
                        args.bits,
                    )
                results["physical_dsp_executions"][name] = len(instructions) * 2
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
