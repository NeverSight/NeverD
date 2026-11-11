#!/usr/bin/env python3
"""Recompile and execute HighC/LLVMC from linked MIPS64 DSP, micro ASE and R6 ELFs."""

import argparse
from pathlib import Path
import runpy
import subprocess

from unicorn import mips_const as mips

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest

ASE = runpy.run_path(str(Path(__file__).with_name("ase-roundtrip.py")))


def kernels():
    for name, cpu in (("mips64", "mips64r2"), ("micromips", "mips32r2")):
        yield name + "dsp", cpu, "dsp", "subu.qb $2,$4,$5"
        yield name + "dspr2", cpu, "dspr2", "adduh_r.qb $2,$4,$5"
    yield "mips64dspindex", "mips64r2", "dsp", "ldx $2,$5($4)"
    yield "mips64dspcompare", "mips64r2", "dsp", "cmp.lt.ph $4,$5\nrddsp $2,63"
    yield (
        "micromipsmsa",
        "mips32r5",
        "msa",
        "fill.w $w0,$4\nfill.w $w1,$5\naddv.w $w2,$w0,$w1\ncopy_s.w $2,$w2[0]",
    )
    # The R6 generic core executes the original microMIPS ELF directly.
    yield "micromips32r6", "mips32r6", "", "mul $2,$4,$5\nbitswap $2,$2\njrc16 $ra"
    yield (
        "micromips32r6branch",
        "mips32r6",
        "",
        "or $2,$4,$0\n.2byte 0x74a4,2\n.2byte 0x5040,7\njrc16 $ra",
    )  # BEQC over ORI
    # ADDIUSP -16, SWSP16 s0/ra into successive words, MOVE16 s0,a0 and v0,s0,
    # LWM16 s0,ra,0(sp), JRCADDIUSP 16.
    yield (
        "micromips32r6frame",
        "mips32r6",
        "",
        ".2byte 0x4ff9,0xca00,0xcbe1,0x0e04,0x0c50,0x4402,0x4493",
    )
    for bits, multiply in ((32, "mul"), (64, "dmulu")):
        cpu = f"mips{bits}r6"
        d = "d" if bits == 64 else ""
        yield cpu, cpu, "", f"{multiply} $2,$4,$5\n{d}bitswap $2,$2"
        yield cpu + "div", cpu, "", f"{d}div $2,$4,$5"
        yield cpu + "branch", cpu, "", "or $2,$4,$0\nbgec $4,$5,1f\nori $2,$0,7\n1:"
        yield cpu + "nal", cpu, "", "or $3,$31,$0\nnal\nor $2,$31,$0\nor $31,$3,$0"


def inputs(name, bits):
    pairs = [
        (a, b)
        for a in (0, 1, -1, 0x11223344, 0x8000000000000001)
        for b in (0, 7, -1, 0xA5A533FF)
    ]
    if name.endswith("div"):
        return [(a, b) for a, b in pairs if b] + [
            (-(1 << (bits - 1)), -1),
            (-17, 3),
            (17, -3),
        ]
    if name.endswith("index"):
        return [
            (roundtrip.DATA + base, offset - base)
            for base in (0, 8, 16, 24)
            for offset in (0, 8, 16, 24)
        ]
    return pairs


def reference_model(bits, feature):
    if not feature:
        return (
            mips.UC_CPU_MIPS64_I6400
            if bits == 64
            else mips.UC_CPU_MIPS32_MIPS32R6_GENERIC
        )
    if bits == 64:
        return mips.UC_CPU_MIPS64_MIPS64DSPR2
    return mips.UC_CPU_MIPS32_P5600 if feature == "msa" else mips.UC_CPU_MIPS32_74KF


def observe(elf, bits, little, feature, cases):
    uc, entry = roundtrip.emulator(
        elf, bits, little, "kernel", cpu_model=reference_model(bits, feature)
    )
    order = "little" if little else "big"

    def execute(address, words):
        uc.mem_write(address, b"".join(w.to_bytes(4, order) for w in words))
        uc.emu_start(address, address + len(words) * 4, count=len(words))

    if feature:
        execute(
            roundtrip.STOP + 0x200,
            ASE["ENABLE_MSA" if feature == "msa" else "ENABLE_DSP"],
        )
    uc.mem_write(roundtrip.DATA, bytes((0x87 + 29 * i) & 255 for i in range(64)))
    observations = []
    for i, pair in enumerate(cases):
        if feature:
            uc.reg_write(
                mips.UC_MIPS_REG_A0, 0x01004005 if feature != "msa" and i % 2 else 0
            )
            execute(
                roundtrip.STOP + 0x300, [0x783E2059 if feature == "msa" else 0x7C81FCF8]
            )
        value = roundtrip.invoke(uc, entry, bits, little, pair)[0]
        control = None
        if feature:
            execute(
                roundtrip.STOP + 0x400, [0x787E0A19 if feature == "msa" else 0x7C3F44B8]
            )
            control = uc.reg_read(mips.UC_MIPS_REG_8) & 0xFFFFFFFF
        observations.append((value, control))
    return observations


def verify_micro(
    args, artifacts, obj, rebuilt, command, log, feature, little, cases, wanted
):
    # The ASE-capable Unicorn cores only execute classic code. The emitted C
    # retains its microMIPS function attribute, including mixed-mode helpers.
    directory = obj.parent
    source, driver = directory / "driver.s", directory / "driver.o"
    asm = [
        ".text",
        ".set noreorder",
        ".set micromips",
        ".set insn32",
        ".globl _start",
        ".ent _start",
        "_start:",
        "la $sp,stack_top",
    ]
    for i, ((a, b), (value, control)) in enumerate(zip(cases, wanted, strict=True)):
        if feature in ("dsp", "dspr2"):
            asm.extend([f"li $4,{0x01004005 if i % 2 else 0}", "wrdsp $4,63"])
        asm.extend(
            [
                f"li $4,{a & 0xFFFFFFFF}",
                f"li $5,{b & 0xFFFFFFFF}",
                "jal kernel",
                "nop",
                f"li $8,{value}",
                f"bne $2,$8,failed_{i}",
                "nop",
            ]
        )
        if feature in ("dsp", "dspr2"):
            asm.extend(
                ["rddsp $9,63", f"li $8,{control}", f"bne $9,$8,failed_{i}", "nop"]
            )
    success = 211
    asm.extend([f"li $4,{success}", "break 0x3ff"])
    for i in range(len(cases)):
        asm.extend([f"failed_{i}:", f"li $4,{i + 1}", "break 0x3ff"])
    asm.extend(
        [".end _start", ".bss", ".balign 16", ".space 8192", "stack_top:", ".space 32"]
    )
    source.write_text("\n".join(asm) + "\n")
    cpu = "mips32r5" if feature == "msa" else "mips32r2"
    command(
        [
            args.assembler,
            "-" + cpu,
            "-32",
            "-EL" if little else "-EB",
            *(["-m" + feature] if feature else []),
            *(["-mfp64"] if feature == "msa" else []),
            "-o",
            driver,
            source,
        ]
    )
    command(
        [
            args.linker,
            "-m",
            "elf32" + ("l" if little else "b") + "tsmip",
            "-N",
            "-Ttext",
            "0x80010000",
            "-e",
            "_start",
            "-o",
            rebuilt,
            driver,
            obj,
        ]
    )
    with artifacts.process(
        [args.simulator, "--endian", "little" if little else "big", rebuilt], log
    ) as (process, _):
        process.wait(timeout=30)
    if process.returncode != success:
        index = process.returncode - 1
        case = cases[index] if 0 <= index < len(cases) else None
        raise ValueError(
            f"{directory.name} simulator exit {process.returncode}, input {case}: {log.read_text()[-1024:]}"
        )
    artifacts.discard(source, driver)


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
    report = {"complete": False, "builds": [], "neverd_sha256": digest(args.neverd)}
    for name, cpu, feature, body in kernels():
        if args.profile and name not in args.profile:
            continue
        bits = 64 if "64" in cpu else 32
        micro = name.startswith("micro")
        micro_r6 = micro and cpu.endswith("r6")
        # Clang 22's microR6 backend emits legacy LUI and JRC16 in its
        # prologue/epilogue. Keep the original R6 ISA and tagged entry, then
        # execute the unedited C on the GNU sim microMIPS32r2/o32 target.
        rebuild_cpu = "mips32r2" if micro_r6 else cpu
        cases = inputs(name, bits)
        extra = (["-m" + feature] if feature else []) + (
            ["-mfp64"] if feature == "msa" else []
        )
        for little in (False, True):
            target = ("mips64" if bits == 64 else "mips") + ("el" if little else "")
            emulation = f"elf{bits}{'l' if little else 'b'}tsmip"
            directory = root / (name + ("el" if little else ""))
            directory.mkdir()
            log = directory / "command.log"
            obj = directory / "kernel.o"

            def command(argv):
                roundtrip.run_command(artifacts, argv, log)

            def assemble(path, compressed):
                source = directory / "kernel.s"
                source.write_text(
                    ".text\n.set noreorder\n.set noat\n"
                    + (
                        ".set micromips\n"
                        if micro_r6
                        else ".set micromips\n.set insn32\n" if compressed else ""
                    )
                    + ".globl kernel\n.type kernel,@function\n.ent kernel\nkernel:\n"
                    + body
                    + ("" if micro_r6 else "\njr $ra\nnop" if feature else "\njrc $ra")
                    + "\n.end kernel\n.size kernel,.-kernel\n"
                )
                if micro_r6:
                    # GNU as 2.47 does not emit microMIPS Release 6.
                    command(
                        [
                            args.clang,
                            "-target",
                            target + "-linux-gnu",
                            "-march=" + cpu,
                            "-mabi=o32",
                            "-mmicromips",
                            "-mno-abicalls",
                            "-fno-pic",
                            "-c",
                            source,
                            "-o",
                            obj,
                        ]
                    )
                else:
                    command(
                        [
                            args.assembler,
                            "-" + cpu,
                            "-EL" if little else "-EB",
                            "-" + str(bits),
                            *extra,
                            "-o",
                            obj,
                            source,
                        ]
                    )
                command(
                    [
                        args.linker,
                        "-m",
                        emulation,
                        "-Ttext",
                        "0x10000",
                        "-e",
                        "kernel",
                        "-o",
                        path,
                        obj,
                    ]
                )
                artifacts.discard(source, obj)

            original = directory / "original.elf"
            assemble(original, micro)
            oracle = original
            if micro and not micro_r6:
                # The reference cores with DSP/MSA expose the classic encoding.
                # NeverD receives the separately assembled physical micro ELF.
                oracle = directory / "classic.elf"
                assemble(oracle, False)
            wanted = observe(oracle, bits, little, feature, cases)
            for view in ("highc", "llvmc"):
                source = directory / (view + ".c")
                command(
                    [
                        args.neverd,
                        "decompile",
                        "--func",
                        "kernel",
                        *(["--llvm"] if view == "llvmc" else []),
                        original,
                        "-o",
                        source,
                    ]
                )
                try:
                    for opt in ("-O0", "-O2"):
                        rebuilt = directory / "rebuilt.elf"
                        command(
                            [
                                args.clang,
                                f"--target={target}-linux-gnu",
                                "-march=" + rebuild_cpu,
                                f"-mabi={bits}",
                                "-mno-abicalls",
                                "-fno-pic",
                                "-G0",
                                "-ffreestanding",
                                "-nostdinc",
                                "-isystem",
                                resource + "/include",
                                "-I",
                                headers,
                                *extra,
                                opt,
                                "-c",
                                source,
                                "-o",
                                obj,
                            ]
                        )
                        if micro:
                            verify_micro(
                                args,
                                artifacts,
                                obj,
                                rebuilt,
                                command,
                                log,
                                feature,
                                little,
                                cases,
                                wanted,
                            )
                        else:
                            command(
                                [
                                    args.linker,
                                    "-m",
                                    emulation,
                                    "-Ttext",
                                    "0x30000",
                                    "-e",
                                    "kernel",
                                    "-o",
                                    rebuilt,
                                    obj,
                                ]
                            )
                            got = observe(rebuilt, bits, little, feature, cases)
                            for pair, actual, expected in zip(
                                cases, got, wanted, strict=True
                            ):
                                if actual != expected:
                                    raise ValueError(
                                        f"{directory.name} {view} {opt} {pair}: {actual} != {expected}"
                                    )
                        report["builds"].append(
                            {
                                "profile": directory.name,
                                "source_isa": cpu,
                                "rebuilt_isa": rebuild_cpu,
                                "view": view,
                                "opt": opt,
                                "cases": len(cases),
                                "source_sha256": digest(source),
                                "elf_sha256": digest(rebuilt),
                            }
                        )
                        artifacts.discard(obj, rebuilt)
                except Exception:
                    artifacts.keep_source(source)
                    raise
                artifacts.discard(source)
            artifacts.discard(original, log)
            if micro and not micro_r6:
                artifacts.discard(oracle)
            artifacts.write("results.json", report)
    report["complete"] = True
    artifacts.write("results.json", report)
    print(
        len(report["builds"]),
        "source builds,",
        sum(row["cases"] for row in report["builds"]),
        "executions passed",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("assembler", "linker", "neverd", "clang", "output"):
        parser.add_argument(
            "--" + option, type=lambda s: Path(s).absolute(), required=True
        )
    parser.add_argument("--simulator", type=lambda s: Path(s).absolute())
    parser.add_argument("--profile", action="append", choices=[k[0] for k in kernels()])
    args = parser.parse_args()
    if not args.simulator and (
        not args.profile or any(p.startswith("micro") for p in args.profile)
    ):
        parser.error("--simulator is required to execute rebuilt microMIPS kernels")
    with MIPSValidationArtifacts(
        args.output,
        "mips-profile-roundtrip",
        (
            args.assembler,
            args.linker,
            args.neverd,
            args.clang,
            *([args.simulator] if args.simulator else []),
        ),
    ) as artifacts:
        run(args, artifacts)
