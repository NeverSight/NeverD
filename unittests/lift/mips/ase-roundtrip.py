#!/usr/bin/env python3
"""Compare extracted MIPS ASE leaves and their HighC/LLVMC rebuilds on 74K/P5600."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from functools import partial
from pathlib import Path
import re
import subprocess
import sys

import roundtrip
from unicorn import UcError, UC_ERR_EXCEPTION
from unicorn import mips_const as mips
from unicorn.unicorn_py3.unicorn import uclib

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts, digest

# Mnemonics are assembled independently of the NeverD encoding table.
UNARY = (
    "fclass ffint_s ffint_u flog2 frint frcp frsqrt fsqrt ftint_s ftint_u "
    "ftrunc_s ftrunc_u fexupl fexupr ffql ffqr"
).split()
BINARY = (
    "fadd fsub fmul fdiv fexp2 fmax fmax_a fmin fmin_a fcaf fceq fcle fclt fcne "
    "fcor fcueq fcule fcult fcun fcune fsaf fseq fsle fslt fsne fsor fsueq "
    "fsule fsult fsun fsune fexdo ftq fmadd fmsub"
).split()
VECTOR_INPUTS = (
    (0x3F800000, 0x40000000, 0x40400000),
    (0x80000000, 0x7F800000, 0),
    (1, 0x7FC12345, 0x7F800001),
    (0x3FC00000, 0x80000001, 0xFF7FFFFF),
)
DSP_INPUTS = (
    (0x80000000, 0x80000000, 0x80008000),
    (0xFFFFFFFF, 0x80000001, 0x7FFF8000),
    (0x7FFFFFFF, 0xFFFFFFFF, 0x7FFFFFFF),
)
VECTOR_SETUP = """
  fill.w $w0,$a1
  insert.w $w0[1],$a2
  insert.w $w0[2],$a3
  xor $t0,$a1,$a3
  insert.w $w0[3],$t0
  fill.w $w1,$a3
  insert.w $w1[1],$a1
  insert.w $w1[2],$a2
  fill.w $w2,$a2
  insert.w $w2[3],$a1
"""
ENABLE_MSA = (
    0x40088005,
    0x3C090800,
    0x01094025,
    0x40888005,
    0xC0,
    0x40086000,
    0x3C092400,
    0x01094025,
    0x40886000,
    0xC0,
)
ENABLE_DSP = (0x40086000, 0x3C090100, 0x01094025, 0x40886000, 0xC0)


def leaves(group):
    if group == "dsp":
        for op in (
            "extr.w",
            "extr_r.w",
            "extr_rs.w",
            "extr_s.h",
            "extp",
            "extpdp",
            "extrv.w",
            "extrv_r.w",
            "extrv_rs.w",
            "extrv_s.h",
            "extpv",
            "extpdpv",
            "dpaq_sa.l.w",
            "dpsq_sa.l.w",
            "maq_sa.w.phl",
            "maq_sa.w.phr",
            "maq_s.w.phl",
            "maq_s.w.phr",
        ):
            setup = "mthi $a1,$ac2\nmtlo $a2,$ac2\n"
            if op.startswith("ext"):
                operand = "$a3" if "v" in op else "7"
                body = f"{op} $v0,$ac2,{operand}"
            else:
                body = f"{op} $ac2,$a2,$a3\nmfhi $v0,$ac2\nmflo $t0,$ac2\nsw $t0,0($a0)"
            yield op.replace(".", "_"), setup + body
        yield "discard_failed_extpdp", "wrdsp $zero,63\nextpdp $zero,$ac2,7\nli $v0,7"
        yield (
            "unused_failed_extpdp",
            setup + "wrdsp $zero,63\nextpdp $t0,$ac2,7\nli $v0,7",
        )
        yield "discard_success_extpdp", setup + "extpdp $zero,$ac2,7\nli $v0,7"
        yield "discard_extr", setup + "extr.w $zero,$ac2,0\nli $v0,7"
        return
    for op in UNARY + BINARY:
        for width in ("h", "w") if op in ("ftq", "fexdo") else ("w", "d"):
            tail = "" if op in UNARY else ",$w1"
            yield op + "_" + width, vector(f"{op}.{width} $w2,$w0{tail}")
    for width, count in zip("bhwd", (16, 8, 4, 2)):
        for op in (
            "ilvev",
            "ilvod",
            "ilvl",
            "ilvr",
            "pckev",
            "pckod",
            "vshf",
            "sld",
            "sldi",
            "splat",
            "splati",
            "insve",
            "shf",
        ):
            if op == "shf" and width == "d":
                continue
            if op in ("splat", "sld"):
                args = "$w0[$a3]"
            elif op in ("splati", "sldi"):
                args = f"$w0[{count - 1}]"
            elif op == "insve":
                yield op + "_" + width, vector(f"insve.{width} $w2[{count - 1}],$w0[0]")
                continue
            else:
                args = "$w0,57" if op == "shf" else "$w0,$w1"
            yield op + "_" + width, vector(f"{op}.{width} $w2,{args}")
    # The return and memory are independent of WD. MSACSR and enabled traps
    # must survive optimization even if no later instruction reads that vector.
    yield "dead_fdiv_w", VECTOR_SETUP + "fdiv.w $w2,$w0,$w1\nli $v0,7"
    for index in range(32):
        if index < 8:
            body = f"ctcmsa ${index},$a1\ncfcmsa $v0,${index}"
        else:
            body = f".word {0x783E2819 | index << 6:#x}\n.word {0x787E0099 | index << 11:#x}"
        yield f"ctrl_{index}", body


def vector(instruction):
    return (
        VECTOR_SETUP
        + instruction
        + "\n"
        + "\n".join(f"copy_s.w $t0,$w2[{i}]\nsw $t0,{4 * i}($a0)" for i in range(4))
        + "\nli $v0,7"
    )


def observe(elf, name, little, group, args, csr):
    is_msa = group == "msa"
    cpu, entry = roundtrip.emulator(
        elf,
        32,
        little,
        name,
        cpu_model=(mips.UC_CPU_MIPS32_P5600 if is_msa else mips.UC_CPU_MIPS32_74KF),
    )
    order = "little" if little else "big"

    def instructions(address, words):
        cpu.mem_write(address, b"".join(x.to_bytes(4, order) for x in words))
        cpu.emu_start(address, address + len(words) * 4, count=len(words))

    cpu.reg_write(mips.UC_MIPS_REG_A0, csr)
    instructions(
        roundtrip.STOP + 0x200,
        (ENABLE_MSA if is_msa else ENABLE_DSP)
        + ((0x783E2059,) if is_msa else (0x7C81FCF8,)),
    )
    saved = {i: 0x3A560000 + i for i in (*range(16, 24), 30)}
    try:
        value, memory = roundtrip.invoke(
            cpu, entry, 32, little, (roundtrip.DATA, *args), initial_registers=saved
        )
        if cpu.reg_read(mips.UC_MIPS_REG_SP) != roundtrip.STACK or any(
            cpu.reg_read(mips.UC_MIPS_REG_0 + i) != word for i, word in saved.items()
        ):
            raise AssertionError("generated leaf did not preserve the o32 caller")
    except UcError as error:
        if error.errno != UC_ERR_EXCEPTION or not is_msa:
            raise
        value, memory = "exception", bytes(cpu.mem_read(roundtrip.DATA, 16)).hex()
    instructions(roundtrip.STOP + 0x300, (0x787E0A19 if is_msa else 0x7C3F44B8,))
    control = cpu.reg_read(mips.UC_MIPS_REG_T0) & 0xFFFFFFFF
    if value == "exception" and not (control >> 12 & ((control >> 7 & 31) | 32)):
        raise AssertionError(
            f"unexpected guest exception at {name} with MSACSR={control:#x}"
        )
    return value, memory, control


def run(args, artifacts):
    cases = [
        (args.group + "_" + n, body)
        for n, body in leaves(args.group)
        if re.search(args.match, n)
    ]
    if not cases:
        raise ValueError("no matching ASE fixture")
    fixture = artifacts.work / "ase.S"
    fixture.write_text(
        ".set noreorder\n.text\n"
        + "".join(
            f".globl {n}\n.type {n},@function\n.ent {n}\n{n}:\n{body}\njr $ra\nnop\n.end {n}\n.size {n},.-{n}\n"
            for n, body in cases
        )
    )
    headers = artifacts.work / "headers"
    headers.mkdir()
    (headers / "string.h").write_text(
        "#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\n"
        "void *memset(void *,int,size_t);\n"
    )
    resource = subprocess.check_output(
        [args.clang, "-print-resource-dir"], text=True
    ).strip()
    report = {
        "neverd_sha256": digest(args.neverd),
        "fixture_sha256": digest(fixture),
        "unicorn_sha256": digest(Path(uclib._name)),
        "cases": [],
    }
    for little in (False, True):
        profile = "mipsel" if little else "mips"
        msa = args.group == "msa"
        flags = [
            f"--target={profile}-linux-gnu",
            "-march=mips32r5" if msa else "-march=mips32r2",
            "-mabi=32",
            "-mno-abicalls",
            "-fno-pic",
            "-G0",
            "-ffreestanding",
            *(["-mmsa", "-mfp64", "-mhard-float"] if msa else ["-mdsp"]),
        ]
        link = [args.ld, "-m", f"elf32{'l' if little else 'b'}tsmip", "-static"]
        original = artifacts.work / f"{profile}.elf"
        obj, log = original.with_suffix(".o"), original.with_suffix(".log")
        command = partial(roundtrip.run_command, artifacts)
        command([args.clang, *flags, "-c", fixture, "-o", obj], log)
        command(
            [*link, "-Ttext", "0x20000", "-e", cases[0][0], obj, "-o", original], log
        )
        artifacts.discard(obj, log)

        def test(name):
            if msa:
                values = VECTOR_INPUTS
                if name.endswith("_d"):
                    values += (
                        (1, 0x7FF00000, 0),
                        (0, 0x7FF80000, 1),
                        (0, 0x7FF00000, 0x80000000),
                        (0x40000000, 0x3FF00000, 0x3FE00000),
                    )
                floating = name.startswith(("msa_f", "msa_dead"))
                states = (
                    (0, 1, 2, 3, 0x01000000, 0x40000, 0x400, 0x800)
                    if floating
                    else (0, 0x01000057)
                )
                inputs = [(v, c) for v in values for c in states]
            else:
                inputs = [(v, 0x0520403F) for v in DSP_INPUTS]
            before = [
                observe(original, name, little, args.group, v, c) for v, c in inputs
            ]
            records = []
            for view in args.views:
                stem = artifacts.work / f"{profile}-{name}-{view}"
                source, obj, elf, log = (
                    stem.with_suffix(ext) for ext in (".c", ".o", ".elf", ".log")
                )
                try:
                    command(
                        [
                            args.neverd,
                            "decompile",
                            "--func",
                            name,
                            *(["--llvm"] if view == "llvmc" else []),
                            "-o",
                            source,
                            original,
                        ],
                        log,
                    )
                    if view == "highc" and name.startswith("msa_ctrl_"):
                        selector = name.rsplit("_", 1)[1]
                        emitted = set(
                            re.findall(
                                r"neverd_mips_c[ft]cmsa_(\d+)\b", source.read_text()
                            )
                        )
                        if emitted != {selector}:
                            raise AssertionError(
                                f"{name} emitted control helpers for {emitted}"
                            )
                    for opt in ("-O0", "-O2"):
                        command(
                            [
                                args.clang,
                                *flags,
                                opt,
                                "-nostdinc",
                                "-isystem",
                                resource + "/include",
                                "-I",
                                headers,
                                "-c",
                                source,
                                "-o",
                                obj,
                            ],
                            log,
                        )
                        command(
                            [*link, "-Ttext", "0x30000", "-e", name, obj, "-o", elf],
                            log,
                        )
                        for (values, state), expected in zip(inputs, before):
                            got = observe(elf, name, little, args.group, values, state)
                            if got != expected:
                                raise RuntimeError(
                                    f"{name} {view} {opt} {values=} {state=:x}: {got} != {expected}"
                                )
                        records.append(
                            [
                                profile,
                                name,
                                view,
                                opt,
                                len(inputs),
                                digest(source),
                                digest(elf),
                            ]
                        )
                        artifacts.discard(obj, elf)
                except Exception:
                    if source.is_file():
                        artifacts.keep_source(source)
                    raise
                artifacts.discard(source, log)
            return records

        with ThreadPoolExecutor(args.jobs) as pool:
            for records in pool.map(test, (n for n, _ in cases)):
                report["cases"].extend(records)
        artifacts.discard(original)
        artifacts.write("results.json", report)
    report["complete"] = True
    artifacts.write("results.json", report)
    print(
        f"{len(report['cases'])} source builds, {sum(r[4] for r in report['cases'])} executions passed"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "ld", "output"):
        parser.add_argument(
            "--" + name, type=lambda s: Path(s).absolute(), required=True
        )
    parser.add_argument(
        "--views", nargs="+", choices=("highc", "llvmc"), default=("highc", "llvmc")
    )
    parser.add_argument("--group", choices=("msa", "dsp"), required=True)
    parser.add_argument("--match", default=".")
    parser.add_argument("--jobs", type=int, default=1)
    args = parser.parse_args()
    with MIPSValidationArtifacts(
        args.output, "mips-ase-roundtrip", (args.neverd, args.clang, args.ld)
    ) as artifacts:
        run(args, artifacts)
