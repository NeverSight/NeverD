#!/usr/bin/env python3
"""Execute fixed o32/n64 FP calls before and after HighC/LLVMC recompilation.

Uses owned assembly, explicit C prototypes, and the pinned Unicorn reference.
Both byte orders, mixed entry parameters, FP overflow arguments and private
link spills are checked. All disposable binaries belong to the artifact owner.
"""

import argparse
from pathlib import Path
import re
import struct
import subprocess

from unicorn import mips_const as mips

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest


def observe(path, bits, little, scalar_bytes, cases):
    model = (
        mips.UC_CPU_MIPS64_I6400 if bits == 64 else mips.UC_CPU_MIPS32_MIPS32R6_GENERIC
    )
    uc, entry = roundtrip.emulator(path, bits, little, "kernel", cpu_model=model)
    result = []
    for value, bias in cases:
        for register in range(1, 32):
            uc.reg_write(getattr(mips, f"UC_MIPS_REG_{register}"), 0)
        for register in range(32):
            uc.reg_write(getattr(mips, f"UC_MIPS_REG_F{register}"), 0)
        uc.mem_write(roundtrip.STACK - 8192, bytes(16384))
        uc.reg_write(mips.UC_MIPS_REG_SP, roundtrip.STACK)
        uc.reg_write(mips.UC_MIPS_REG_RA, roundtrip.STOP)
        uc.reg_write(mips.UC_MIPS_REG_FCSR, 0)
        uc.reg_write(
            mips.UC_MIPS_REG_F12,
            struct.unpack(
                "<Q" if scalar_bytes == 8 else "<I",
                struct.pack("<d" if scalar_bytes == 8 else "<f", value),
            )[0],
        )
        uc.reg_write(
            getattr(mips, f"UC_MIPS_REG_{5 if bits == 64 or scalar_bytes == 4 else 6}"),
            bias & ((1 << bits) - 1),
        )
        saved = range(24, 32) if bits == 64 else range(20, 32, 2)
        for register in saved:
            uc.reg_write(
                getattr(mips, f"UC_MIPS_REG_F{register}"), 0x3FF0000000000000 + register
            )
        uc.emu_start(entry, roundtrip.STOP, count=10000)
        if uc.reg_read(mips.UC_MIPS_REG_PC) != roundtrip.STOP:
            raise ValueError("kernel did not return to its incoming link")
        if uc.reg_read(mips.UC_MIPS_REG_SP) != roundtrip.STACK:
            raise ValueError("kernel did not restore its stack")
        for register in saved:
            if (
                uc.reg_read(getattr(mips, f"UC_MIPS_REG_F{register}"))
                != 0x3FF0000000000000 + register
            ):
                raise ValueError("callee-saved FPR changed")
        result.append(
            (
                uc.reg_read(mips.UC_MIPS_REG_F0) & ((1 << (scalar_bytes * 8)) - 1),
                uc.reg_read(mips.UC_MIPS_REG_FCSR),
            )
        )
    return result


def narrow_arguments(bits, scalar_bytes, fmt, load, store):
    # Compare the whole machine word the caller provided for each C integer.
    # A single wrong sign or zero extension sets its own bit in the FP result.
    def constants(regs):
        a, b, c, d, e = regs
        return [
            f"addiu ${a},$zero,-7",
            f"ori ${b},$zero,249",
            f"addiu ${c},$zero,-257",
            f"ori ${d},$zero,65279",
            f"lui ${e},0x8000",
            f"ori ${e},${e},1",
        ]

    setup = constants(range(4, 9))
    check = []
    if bits == 64:
        setup.append(f"mov.{fmt} $f17,$f12")
        check.append(f"mov.{fmt} $f12,$f17")
    else:
        fp_offset = 24 if scalar_bytes == 8 else 20
        setup.extend(["sw $8,16($sp)", f"{store} $f12,{fp_offset}($sp)"])
        check.extend(["lw $8,16($sp)", f"{load} $f12,{fp_offset}($sp)"])
    check.extend(constants(range(10, 15)))
    check.append("or $9,$zero,$zero")
    for i in range(5):
        check.extend(
            [
                f"xor $15,${4 + i},${10 + i}",
                "sltu $15,$zero,$15",
                f"sll $15,$15,{i}",
                "or $9,$9,$15",
            ]
        )
    check.extend(["mtc1 $9,$f2", f"cvt.{fmt}.w $f2,$f2", f"add.{fmt} $f0,$f12,$f2"])
    return setup, "\n".join(check)


def validate_narrow_llvm(ir):
    expected = (
        ("i8", "signext"),
        ("i8", "zeroext"),
        ("i16", "signext"),
        ("i16", "zeroext"),
        ("i32", "signext"),
    )
    declarations, calls = set(), set()
    for line in ir.splitlines():
        if line.startswith("declare "):
            names = declarations
        elif " call " in line:
            names = calls
        else:
            continue
        match = re.search(r"@([\w.$-]+)\(([^()]*)\)", line)
        if match:
            params = [param.split() for param in match[2].split(",")]
            if len(params) == 6 and all(
                param and param[0] == kind and extension in param
                for param, (kind, extension) in zip(params[:5], expected, strict=True)
            ):
                names.add(match[1])
    if len(declarations & calls) != 1:
        raise ValueError(
            "the LLVM declaration and call must carry all five integer extensions"
        )


def expected_result(layout, bits, value, bias):
    total = value + bias
    if layout == "word-result":
        word = int(total)
        # The n64 caller adds the upper half of the returned machine word.
        return word + ((2 if word < 0 else 3) if bits == 64 else 0)
    if layout == "pointer-result":
        return (int(total) & 255) + (bits == 64)
    if layout == "void-result":
        # The caller reads the inexact flag set by the callee's conversion.
        return 4 if total != int(total) else 0
    if layout == "narrow-arguments":
        return 2 * total
    if layout == "after-integer":
        return 2 * (total + 7)
    return 4 * total


def run(args, artifacts):
    root = artifacts.work
    headers = root / "headers"
    headers.mkdir()
    (headers / "string.h").write_text(
        "#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n"
    )
    resource = subprocess.check_output(
        [args.clang, "-print-resource-dir"], text=True
    ).strip()
    cases = [
        (value, bias)
        for value in (-8.5, -0.0, 0.0, 1.25, 1048576.5)
        for bias in (-17, 0, 3, 1024)
    ]
    report = {"complete": False, "neverd_sha256": digest(args.neverd), "builds": []}
    artifacts.write("results.json", report)
    for bits in (32, 64):
        for little in (False, True):
            target = ("mips64" if bits == 64 else "mips") + ("el" if little else "")
            cpu = f"mips{bits}r6"
            emulation = f"elf{bits}{'l' if little else 'b'}tsmip"
            for scalar_bytes in (4, 8):
                for layout in (
                    "prefix",
                    "overflow",
                    "after-integer",
                    "narrow-arguments",
                    "word-result",
                    "pointer-result",
                    "void-result",
                ):
                    overflow = layout == "overflow"
                    after_integer = layout == "after-integer"
                    directory = root / (target + f"-f{scalar_bytes * 8}" + "-" + layout)
                    directory.mkdir()
                    log = directory / "command.log"

                    def command(argv):
                        roundtrip.run_command(artifacts, argv, log)

                    count = 9 if overflow else 1
                    c_type, fmt = (
                        ("double", "d") if scalar_bytes == 8 else ("float", "s")
                    )
                    load, store = (
                        ("ldc1", "sdc1") if scalar_bytes == 8 else ("lwc1", "swc1")
                    )
                    declarations = directory / "declarations.h"
                    result_type = {
                        "word-result": "long",
                        "pointer-result": "void *",
                        "void-result": "void",
                    }.get(layout, c_type)
                    if layout == "narrow-arguments":
                        params = (
                            "signed char s8, unsigned char u8, short s16, "
                            f"unsigned short u16, unsigned int u32, {c_type} value"
                        )
                    elif after_integer:
                        params = f"int tag, {c_type} value"
                    else:
                        params = ", ".join(f"{c_type} a{i}" for i in range(count))
                        if layout == "pointer-result":
                            params += ", void *base"
                    declarations.write_text(
                        f"{c_type} kernel({c_type} value, int bias);\n"
                        f"{result_type} fp_callee({params});\n"
                    )
                    d = "d" if bits == 64 else ""
                    slot = 88 if bits == 64 else 92
                    setup = []
                    if overflow:
                        if bits == 64:
                            setup = [f"mov.{fmt} $f{i},$f12" for i in range(13, 20)] + [
                                f"{store} $f12,0($sp)"
                            ]
                        else:
                            setup = [f"mov.{fmt} $f14,$f12"]
                            if scalar_bytes == 4:
                                setup += ["mfc1 $6,$f12", "mfc1 $7,$f12"]
                            setup += [
                                f"{store} $f12,{16 + i * scalar_bytes}($sp)"
                                for i in range(7 if scalar_bytes == 8 else 5)
                            ]
                    if after_integer:
                        setup = ["addiu $4,$zero,7"]
                        if bits == 64:
                            setup += [f"mov.{fmt} $f13,$f12"]
                            incoming = f"mov.{fmt} $f12,$f13"
                        elif scalar_bytes == 4:
                            setup += ["mfc1 $5,$f12"]
                            incoming = "mtc1 $5,$f12"
                        else:
                            low, high = (6, 7) if little else (7, 6)
                            setup += [f"mfc1 ${low},$f12", f"mfhc1 ${high},$f12"]
                            incoming = f"mtc1 ${low},$f12\nmthc1 ${high},$f12"
                    prefix = ".text\n.set noreorder\n"
                    callee = (
                        f"{load} $f2,"
                        + ("0" if bits == 64 else "64" if scalar_bytes == 8 else "32")
                        + f"($sp)\nadd.{fmt} $f0,$f12,$f2"
                        if overflow
                        else f"add.{fmt} $f0,$f12,$f12"
                    )
                    if after_integer:
                        callee = (
                            incoming
                            + f"\nmtc1 $4,$f2\ncvt.{fmt}.w $f2,$f2"
                            + f"\nadd.{fmt} $f0,$f12,$f2"
                        )
                    if layout == "narrow-arguments":
                        setup, callee = narrow_arguments(
                            bits, scalar_bytes, fmt, load, store
                        )
                    after_call = f"add.{fmt} $f0,$f0,$f0"
                    if layout in ("word-result", "pointer-result", "void-result"):
                        # A volatile f0 marker must never supply a GPR/void result.
                        callee = f"trunc.w.{fmt} $f2,$f12\nmfc1 $2,$f2\nmtc1 $zero,$f0"
                        after_call = f"mtc1 $2,$f0\ncvt.{fmt}.w $f0,$f0"
                        if layout == "word-result" and bits == 64:
                            callee += (
                                "\ndaddiu $8,$zero,3\ndsll32 $8,$8,0\ndaddu $2,$2,$8"
                            )
                            # A word ADDU requires sign-extended word inputs on n64.
                            # Keep the full return in a doubleword op; MTC1 reads its low word.
                            after_call = "dsrl32 $8,$2,0\ndaddu $2,$2,$8\n" + after_call
                        if layout == "pointer-result":
                            arg = 5 if bits == 64 or scalar_bytes == 4 else 6
                            setup = [
                                (
                                    f"dli ${arg},0x100600000"
                                    if bits == 64
                                    else f"lui ${arg},0x60"
                                )
                            ]
                            callee += f"\nandi $2,$2,255\n{d}addu $2,$2,${arg}"
                            after_call = (
                                "dsrl32 $8,$2,0\nandi $2,$2,255\naddu $2,$2,$8\n"
                                if bits == 64
                                else "andi $2,$2,255\n"
                            ) + after_call
                        elif layout == "void-result":
                            # Observe the conversion's inexact sticky flag across the call.
                            after_call = "cfc1 $2,$31\nandi $2,$2,4\n" + after_call
                    callee_text = (
                        prefix
                        + ".globl fp_callee\n.ent fp_callee\nfp_callee:\n"
                        + callee
                        + "\njr $ra\nnop\n.end fp_callee\n"
                    )
                    caller = (
                        prefix
                        + ".globl kernel\n.ent kernel\nkernel:\n"
                        + f"{d}addiu $sp,$sp,-96\n{'sd' if bits == 64 else 'sw'} $ra,{slot}($sp)\n"
                        + f"mtc1 ${5 if bits == 64 or scalar_bytes == 4 else 6},$f2\ncvt.{fmt}.w $f2,$f2\nadd.{fmt} $f12,$f12,$f2\n"
                        + "\n".join(setup)
                        + f"\nbalc fp_callee\n{after_call}\n"
                        + f"{'ld' if bits == 64 else 'lw'} $ra,{slot}($sp)\n{d}addiu $sp,$sp,96\njr $ra\nnop\n.end kernel\n"
                    )
                    assembly = directory / "original.s"
                    assembly.write_text(caller + callee_text)
                    obj, original = directory / "original.o", directory / "original.elf"
                    asm_flags = [
                        "-march=" + cpu,
                        "-mabi=" + str(bits),
                        "-mfp64",
                        "-mnan=2008",
                        "-EL" if little else "-EB",
                    ]
                    command([args.assembler, *asm_flags, assembly, "-o", obj])
                    command(
                        [
                            args.linker,
                            "-m",
                            emulation,
                            "-Ttext",
                            "0x10000",
                            "-e",
                            "kernel",
                            obj,
                            "-o",
                            original,
                        ]
                    )
                    wanted = observe(original, bits, little, scalar_bytes, cases)
                    for (value, bias), (actual, _) in zip(cases, wanted, strict=True):
                        expected = struct.unpack(
                            "<Q" if scalar_bytes == 8 else "<I",
                            struct.pack(
                                "<d" if scalar_bytes == 8 else "<f",
                                expected_result(layout, bits, value, bias),
                            ),
                        )[0]
                        if actual != expected:
                            raise ValueError(
                                "original ELF disagrees with independent arithmetic"
                            )
                    command(
                        [
                            args.neverd,
                            "types",
                            original,
                            "--declare",
                            declarations,
                            "--save",
                        ]
                    )
                    if layout == "narrow-arguments":
                        ir = directory / "kernel.ll"
                        command(
                            [
                                args.neverd,
                                "export",
                                original,
                                "--format",
                                "ir",
                                "--func",
                                "kernel",
                                "-o",
                                ir,
                            ]
                        )
                        validate_narrow_llvm(ir.read_text())
                        artifacts.discard(ir)
                    callee_source, callee_obj = (
                        directory / "callee.s",
                        directory / "callee.o",
                    )
                    callee_source.write_text(callee_text)
                    command(
                        [args.assembler, *asm_flags, callee_source, "-o", callee_obj]
                    )
                    for view in ("highc", "llvmc"):
                        source = directory / (view + ".c")
                        command(
                            [
                                args.neverd,
                                "decompile",
                                original,
                                "--func",
                                "kernel",
                                *(["--llvm"] if view == "llvmc" else []),
                                "-o",
                                source,
                            ]
                        )
                        artifacts.keep_source(source)
                        for opt in ("-O0", "-O2"):
                            rebuilt = directory / "rebuilt.elf"
                            command(
                                [
                                    args.clang,
                                    "--target=" + target + "-linux-gnu",
                                    "-march=" + cpu,
                                    "-mabi=" + str(bits),
                                    "-mfp64",
                                    "-mnan=2008",
                                    "-mno-abicalls",
                                    "-fno-pic",
                                    "-G0",
                                    "-ffreestanding",
                                    "-nostdinc",
                                    "-isystem",
                                    resource + "/include",
                                    "-I",
                                    headers,
                                    opt,
                                    "-c",
                                    source,
                                    "-o",
                                    obj,
                                ]
                            )
                            command(
                                [
                                    args.linker,
                                    "-m",
                                    emulation,
                                    "-Ttext",
                                    "0x30000",
                                    "-e",
                                    "kernel",
                                    obj,
                                    callee_obj,
                                    "-o",
                                    rebuilt,
                                ]
                            )
                            actual = observe(rebuilt, bits, little, scalar_bytes, cases)
                            if actual != wanted:
                                index = next(
                                    i
                                    for i, pair in enumerate(zip(actual, wanted))
                                    if pair[0] != pair[1]
                                )
                                raise ValueError(
                                    f"{directory.name} {view} {opt} {cases[index]}: {actual[index]} != {wanted[index]}"
                                )
                            report["builds"].append(
                                {
                                    "profile": directory.name,
                                    "view": view,
                                    "opt": opt,
                                    "cases": len(cases),
                                    "source_sha256": digest(source),
                                    "original_sha256": digest(original),
                                    "elf_sha256": digest(rebuilt),
                                }
                            )
                            artifacts.write("results.json", report)
                            artifacts.discard(obj, rebuilt)
                        artifacts.discard(source)
                    artifacts.discard(
                        original, assembly, callee_source, callee_obj, declarations, log
                    )
    report["complete"] = True
    artifacts.write("results.json", report)
    print(
        f"passed {len(report['builds'])} builds, {len(report['builds']) * len(cases)} executions"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "assembler", "linker", "clang", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with MIPSValidationArtifacts(
        args.output,
        "mips-floating-abi-roundtrip",
        [args.neverd, args.assembler, args.linker, args.clang],
    ) as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
