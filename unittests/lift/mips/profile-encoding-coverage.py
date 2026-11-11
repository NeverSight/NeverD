#!/usr/bin/env python3
"""Qualify linked DSP/MSA and Release 6 machine code with GNU as, ld and NeverD."""

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile


R6 = (
    ("", "pause"),
    ("$2", "di ei"),
    ("$2,$12,0", "mfc0 mtc0 mfhc0 mthc0"),
    ("$2,$4,3", "rdhwr"),
    ("3,-8($4)", "cache pref"),
    ("$2,-8($4)", "ll sc"),
    ("$2,$3,($4)", "llwp scwp"),
    ("", "nal"),
    ("$2,$4,$5", "selnez seleqz mul muh mulu muhu div mod divu modu"),
    ("$2,$4", "clz clo bitswap"),
    ("$2,$4,$5,2", "lsa align"),
    ("$2,$4,5", "aui"),
    ("$2,16", "addiupc lwpc"),
    ("$2,5", "auipc aluipc"),
    ("1f", "bc balc"),
    (
        "$4,1f",
        "beqzc bnezc beqzalc bnezalc bgtzc blezc bgezc bltzc blezalc bgezalc bgtzalc bltzalc",
    ),
    ("$4,$5,1f", "beqc bnec bltc bgec bltuc bgeuc"),
    ("$5,$4,1f", "bovc bnvc"),
    ("$4,4", "jic jialc"),
)
R6_DOUBLE = (
    ("$2,$14,0", "dmfc0 dmtc0"),
    ("$2,248($4)", "lld scd"),
    ("$2,$3,($4)", "lldp scdp"),
    ("$2,$4,$5", "dmul dmuh dmulu dmuhu ddiv dmod ddivu dmodu"),
    ("$2,$4", "dclo dclz dbitswap"),
    ("$2,$4,$5,3", "dlsa dalign"),
    ("$2,$4,5", "daui"),
    ("$2,$2,5", "dahi dati"),
    ("$2,16", "ldpc lwupc"),
)


MICRO64 = (
    ("$2,$4,$5", "daddu dsubu dsllv dsrlv dsrav drotrv"),
    ("$2,$4,-53", "daddiu"),
    ("$2,$4,5", "dsll dsrl dsra drotr dsll32 dsrl32 dsra32 drotr32"),
    ("$2,$4", "dclz dclo dsbh dshd"),
    ("$2,$4,5,7", "dext dins"),
    ("$2,$4,5,40", "dextm dinsm"),
    ("$2,$4,35,7", "dextu dinsu"),
    ("$2,-16($4)", "ld sd lwu ldl ldr"),
)


def corpora():
    ase = runpy.run_path(str(Path(__file__).with_name("ase-encoding-coverage.py")))
    for feature in ("dsp", "dspr2", "msa"):
        insns = [
            f"{name}{'.' + width if width else ''} {args.replace('.+8', '1f')}"
            for args, widths, names in ase[feature.upper()]
            for name in names.split()
            for width in widths or ("",)
        ]
        for prefix in ("mips", "micromips"):
            yield (
                prefix + feature,
                "mips32r5" if feature == "msa" else "mips32r2",
                feature,
                insns,
            )
        if feature != "msa":
            yield (
                "mips64" + feature,
                "mips64r2",
                feature,
                insns + (["ldx $2,$5($4)"] if feature == "dsp" else []),
            )
            yield "micromips64" + feature, "mips64r2", feature, insns
    wide_scalar = [
        f"{name} {args}" for args, names in MICRO64 for name in names.split()
    ]
    for profile in ("mips64", "micromips64"):
        yield profile, "mips64r2", "", wide_scalar
    for bits in (32, 64):
        profile = f"mips{bits}r6"
        yield (
            profile,
            profile,
            "",
            [
                f"{name} {operands}"
                for operands, names in R6 + (R6_DOUBLE if bits == 64 else ())
                for name in names.split()
            ],
        )


def assemble(tools, directory, profile, cpu, feature, instructions, little):
    source, obj, elf, raw = (
        directory / n for n in ("fixture.s", "fixture.o", "fixture.elf", "code.bin")
    )
    bits = 64 if "64" in cpu else 32
    source.write_text(
        ".text\n.set noreorder\n.set noat\n"
        + (".set micromips\n.set insn32\n" if profile.startswith("micro") else "")
        + ".globl entry\n.type entry,@function\nentry:\n"
        + "".join(f"{line}\nnop\n1:\n" for line in instructions)
        + ".size entry,.-entry\n"
    )
    options = ["-m" + feature] if feature else []
    if cpu.endswith("r6"):
        options.append("-mxpa")  # High CP0 transfers carry the GNU XPA flag.
    if feature == "msa":
        options.append("-mfp64")
    subprocess.run(
        [
            tools.assembler,
            "-EL" if little else "-EB",
            "-" + cpu,
            "-" + str(bits),
            *options,
            "-o",
            obj,
            source,
        ],
        check=True,
    )
    emulation = f"elf{bits}{'l' if little else 'b'}tsmip"
    subprocess.run(
        [
            tools.linker,
            "-m",
            emulation,
            "-Ttext",
            "0x10000",
            "-e",
            "entry",
            "-o",
            elf,
            obj,
        ],
        check=True,
    )
    with elf.open("rb") as stream:
        image = ELFFile(stream)
        [entry] = image.get_section_by_name(".symtab").get_symbol_by_name("entry")
        length = entry["st_size"]
        if length != len(instructions) * 8:
            raise ValueError(f"{profile}: assembler expanded or dropped an instruction")
        data = image.get_section_by_name(".text").data()[:length]
    if any(data[offset : offset + 4] != bytes(4) for offset in range(4, length, 8)):
        raise ValueError(f"{profile}: unexpected branch slot or padding")
    raw.write_bytes(data)
    return raw


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("assembler", "linker", "bench"):
        parser.add_argument("--" + option, type=Path, required=True)
    tools = parser.parse_args()
    records = {}
    with tempfile.TemporaryDirectory(prefix="neverd-mips-profiles-") as scratch:
        for profile, cpu, feature, instructions in corpora():
            for little in (False, True):
                name = profile + ("el" if little else "")
                raw = assemble(
                    tools, Path(scratch), profile, cpu, feature, instructions, little
                )
                result = json.loads(
                    subprocess.check_output([tools.bench, name, "lift", raw, "1"])
                )
                if result["instructions_per_repeat"] != len(instructions) * 2:
                    raise ValueError(f"{name}: incomplete physical coverage")
                records[name] = dict(
                    forms=len(instructions),
                    digest=result["digest"],
                    operations=result["operations_per_repeat"],
                    sha256=hashlib.sha256(raw.read_bytes()).hexdigest(),
                )
                for reference in ([profile] if little else []) + (
                    [name.removeprefix("micro")] if profile.startswith("micro") else []
                ):
                    expected = records[reference]["digest"]
                    if profile == "micromips64dsp" and reference.startswith("mips64dsp"):
                        # The classic catalog also has LDX. Compare the common
                        # instructions at the same addresses in both profiles.
                        shared = assemble(
                            tools, Path(scratch), "mips64dsp", cpu,
                            feature, instructions, little
                        )
                        expected = json.loads(subprocess.check_output(
                            [tools.bench, reference, "lift", shared, "1"]
                        ))["digest"]
                    if result["digest"] != expected:
                        raise ValueError(f"{name} LowIR differs from {reference}")
    print(json.dumps(records, indent=2))


if __name__ == "__main__":
    main()
