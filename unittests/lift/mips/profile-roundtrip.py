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

DSP64_BOUNDARIES = {
    f"mips64boundary_{op.replace('.', '_')}_{str(count).replace('-', 'n')}_{pos}_{half}":
        (op, count, pos, half)
    for op, count, pos, half in (
        [(op, 1, 0, "value") for op in ("dextr.w", "dextr.l")]
        + [(op, count, 0, "value") for op in ("dextrv.w", "dextrv.l")
           for count in (0, 1, 32, 63)]
        + [(op, 31, pos, "value") for op in ("dextp", "dextpdp")
           for pos in (31, 95)]
        + [(op, 63, pos, "value") for op in ("dextpv", "dextpdpv")
           for pos in (63, 94, 127)]
        + [(op, -64, 0, half) for op in ("dshilo", "dshilov")
           for half in ("hi", "lo")]
    )
}


def kernels():
    for name, (op, count, pos, half) in DSP64_BOUNDARIES.items():
        setup = f"mthi $4\nmtlo $5\ndaddiu $6,$0,{count}\n"
        if op.startswith("dextp"):
            setup += f"ori $7,$0,{pos}\nwrdsp $7,1\n"
        operand = "$6" if "v" in op else str(count)
        operation = (f"{op} $ac0,{operand}\nmf{half} $2" if half != "value" else
                     f"{op} $2,$ac0,{operand}")
        yield name, "mips64r2", "dsp", setup + operation
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
    yield (
        "mips64msa", "mips64r5", "msa",
        "fill.d $w0,$4\ninsert.d $w0[1],$5\nsplati.d $w1,$w0[1]\n"
        "addv.d $w2,$w0,$w1\ncopy_s.d $2,$w2[0]",
    )
    yield (
        "mips64msafloat", "mips64r5", "msa",
        "fill.d $w0,$4\nfill.d $w1,$5\nfadd.d $w2,$w0,$w1\ncopy_s.d $2,$w2[0]",
    )
    for instruction in (
        "dpau.h.obl dpau.h.obr dpsu.h.obl dpsu.h.obr dpaq_s.w.qh dpsq_s.w.qh "
        "mulsaq_s.w.qh dpaq_sa.l.pw dpsq_sa.l.pw mulsaq_s.l.pw "
        "maq_sa.w.qhll maq_sa.w.qhlr maq_sa.w.qhrl maq_sa.w.qhrr "
        "maq_s.w.qhll maq_s.w.qhlr maq_s.w.qhrl maq_s.w.qhrr "
        "maq_s.l.pwl maq_s.l.pwr"
    ).split():
        yield (
            "mips64acc_" + instruction.replace(".", "_"), "mips64r2", "dsp",
            "mthi $0\nmtlo $0\n" + instruction + " $ac0,$4,$5\nmflo $2",
        )
    for instruction in ("dmaddu", "dmsubu"):
        for half in ("hi", "lo"):
            yield (
                f"mips64acc_{instruction}_{half}", "mips64r2", "dsp",
                f"mthi $4\nmtlo $5\n{instruction} $ac0,$4,$5\nmf{half} $2",
            )
    for instruction in ("dextr.w", "dextrv.w", "dextr.l", "dextrv.l",
                        "dextp", "dextpv", "dextpdp", "dextpdpv"):
        variable = "v" in instruction
        setup = "mthi $4\nmtlo $5\nori $6,$0,17\nori $7,$0,63\nwrdsp $7,1\n"
        yield (
            "mips64extract_" + instruction.replace(".", "_"), "mips64r2", "dsp",
            setup + instruction + " $2,$ac0," + ("$6" if variable else "17"),
        )
    for instruction in ("dshilo", "dshilov", "dmthlip"):
        for half in ("hi", "lo"):
            operation = (f"{instruction} $4,$ac0" if instruction == "dmthlip" else
                         f"{instruction} $ac0," + ("$6" if instruction == "dshilov" else "-17"))
            yield (
                f"mips64extract_{instruction}_{half}", "mips64r2", "dsp",
                "mthi $4\nmtlo $5\ndaddiu $6,$0,-17\n" + operation + f"\nmf{half} $2",
            )
    yield (
        "mips64extract_mixed", "mips64r2", "dsp",
        "mthi $4\nmtlo $5\nori $6,$0,63\nwrdsp $6,1\nori $7,$0,47\n"
        "extp $3,$ac0,15\ndextpv $2,$ac0,$7\nxor $2,$2,$3",
    )
    yield (
        "mips64extract_dinsv", "mips64r2", "dsp",
        "ori $6,$0,2207\nwrdsp $6,3\nor $2,$5,$0\ndinsv $2,$4",
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
        ".2byte 0x4ff9,0xca00,0xcbe1\nmove16 $16,$4\n.2byte 0x0c50,0x4402,0x4493",
    )
    for bits, multiply in ((32, "mul"), (64, "dmulu")):
        cpu = f"mips{bits}r6"
        d = "d" if bits == 64 else ""
        transfer = "dmtc1" if bits == 64 else "mtc1"
        move = "dmfc1" if bits == 64 else "mfc1"
        fmt = "d" if bits == 64 else "s"
        yield (
            cpu + "fp", cpu, "",
            f"{transfer} $4,$f0\n{transfer} $5,$f2\n"
            f"add.{fmt} $f4,$f0,$f2\n{move} $2,$f4",
        )
        yield (
            cpu + "fpcompare", cpu, "",
            f"{transfer} $4,$f0\n{transfer} $5,$f2\n"
            f"cmp.une.{fmt} $f4,$f0,$f2\n{move} $2,$f4",
        )
        yield (
            cpu + "fpround", cpu, "",
            f"{transfer} $4,$f0\nrint.{fmt} $f4,$f0\n{move} $2,$f4",
        )
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
    if name.startswith(("mips64extract_", "mips64boundary_")):
        values = (0, 1, 0x123456789abcdef0, (1 << 63) - 1, 1 << 63, (1 << 64) - 1)
        return [(a, b) for a in values for b in values]
    if name.startswith("mips64acc_"):
        vectors = [
            (a, b)
            for a in (0, 1, 2, 0x0001000200030004, 0xFFFF00010002FFFF)
            for b in (0, 5, 6, 0x0005000600070008, 0xFFFF00020001FFFF)
        ]
        if name.startswith(("mips64acc_dmaddu_", "mips64acc_dmsubu_")):
            vectors.extend([(0x10000, 0x10000), (0xffffffff, 0xffffffff),
                            (0xffffffff00000000, 0xffffffff),
                            (0xffffffffffffffff, 0xffffffffffffffff)])
        return vectors
    if "fp" in name:
        values = (
            (0, 0x80000000, 0x3FC00000, 0xBF800000, 0x7FC00000, 0x7F800001, 1)
            if bits == 32 else
            (0, 0x8000000000000000, 0x3FF8000000000000, 0xBFF0000000000000,
             0x7FF8000000000000, 0x7FF0000000000001, 1)
        )
        return [(a, b) for a in values for b in values]
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
        return mips.UC_CPU_MIPS64_I6400 if feature == "msa" else mips.UC_CPU_MIPS64_MIPS64DSPR2
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
    if not feature:
        status = uc.reg_read(mips.UC_MIPS_REG_CP0_STATUS) | 0x24000000
        uc.reg_write(mips.UC_MIPS_REG_CP0_STATUS, status)
    uc.mem_write(roundtrip.DATA, bytes((0x87 + 29 * i) & 255 for i in range(64)))
    observations = []
    for i, pair in enumerate(cases):
        if not feature:
            uc.reg_write(mips.UC_MIPS_REG_FCSR, 0xC0000 | (i & 3))
        if feature:
            uc.reg_write(
                mips.UC_MIPS_REG_A0, 0x01004005 if feature != "msa" and i % 2 else 0
            )
            execute(
                roundtrip.STOP + 0x300, [0x783E2059 if feature == "msa" else 0x7C81FCF8]
            )
        value = roundtrip.invoke(uc, entry, bits, little, pair)[0]
        control = uc.reg_read(mips.UC_MIPS_REG_FCSR) if not feature else None
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
        # The pinned I6400 implements MSA and n64, with the Release 6
        # return encoding. The original uses JALR zero,ra, valid in r5 and r6.
        if bits == 64 and feature == "msa":
            rebuild_cpu = "mips64r6"
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
                    + ("" if micro_r6 else "\njalr $0,$31\nnop" if bits == 64 and feature == "msa" else "\njr $ra\nnop" if feature else "\njrc $ra")
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
            if name in DSP64_BOUNDARIES:
                op, count, pos, half = DSP64_BOUNDARIES[name]
                mask = (1 << 64) - 1
                for i, ((a, b), actual) in enumerate(zip(cases, wanted, strict=True)):
                    acc = (a << 64) | b
                    control = 0x01004005 if i % 2 else 0
                    if op.startswith("dextr"):
                        value = (acc >> count) & mask
                        if op.endswith(".w"):
                            value &= 0xffffffff
                            if value & (1 << 31):
                                value |= mask ^ 0xffffffff
                    elif op.startswith("dextp"):
                        value = (acc >> (pos - count)) & ((1 << (count + 1)) - 1)
                        control = (control & ~0x407f) | (
                            (pos - count - 1) & 127 if "dp" in op else pos)
                    else:
                        shifted = acc >> count if count >= 0 else acc << -count
                        value = (shifted >> (64 if half == "hi" else 0)) & mask
                    if actual != (value, control):
                        raise ValueError(f"{name}: original {actual}, independent {(value, control)}")
            if name.startswith("mips64extract_"):
                mask = (1 << 64) - 1
                instruction = name.removeprefix("mips64extract_")
                for i, ((a, b), actual) in enumerate(zip(cases, wanted, strict=True)):
                    acc = (a << 64) | b
                    control = 0x01004005 if i % 2 else 0
                    if instruction.startswith("dextr"):
                        value = (acc >> 17) & mask
                        if instruction.endswith("_w"):
                            value &= 0xffffffff
                            if value & (1 << 31):
                                value |= mask ^ 0xffffffff
                        control = (control & ~127) | 63
                    elif instruction.startswith("dextp"):
                        value = (acc >> 46) & ((1 << 18) - 1)
                        control = (control & ~0x407f) | (45 if "dp" in instruction else 63)
                    elif instruction.startswith("dshilo"):
                        value = ((acc << 17) >> (64 if instruction.endswith("_hi") else 0)) & mask
                    elif instruction.startswith("dmthlip"):
                        value = b if instruction.endswith("_hi") else a
                        control += 64
                    elif instruction == "dinsv":
                        field = ((1 << 17) - 1) << 31
                        value = (b & ~field) | ((a << 31) & field)
                        control = (control & ~0x1fff) | 2207
                    else:
                        word_acc = ((a & 0xffffffff) << 32) | (b & 0xffffffff)
                        value = ((acc >> 16) & ((1 << 48) - 1)) ^ ((word_acc >> 48) & 0xffff)
                        control = (control & ~0x407f) | 63
                    if actual != (value, control):
                        raise ValueError(f"{name}: original {actual}, independent {(value, control)}")
            if name.startswith(("mips64acc_dmaddu_", "mips64acc_dmsubu_")):
                word = (1 << 32) - 1
                mask = (1 << 64) - 1
                for (a, b), (value, _) in zip(cases, wanted, strict=True):
                    dot = (a >> 32) * (b >> 32) + (a & word) * (b & word)
                    result = ((a << 64) | b) + (-dot if "dmsubu" in name else dot)
                    expected = (result >> (64 if name.endswith("_hi") else 0)) & mask
                    if value != expected:
                        raise ValueError(f"{name}: original returned {value:#x}, expected {expected:#x}")
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
