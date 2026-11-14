#!/usr/bin/env python3
"""Run original and regenerated microMIPS64r2 kernels on GNU sim."""

import argparse
import subprocess
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile

STORES = ("mips_merge_store_left_double", "mips_merge_store_right_double",
          "mips_merge_store_double")
DIVISIONS = ("mips_ddiv_lo", "mips_ddiv_hi", "mips_ddivu_lo", "mips_ddivu_hi")
SUCCESS = 211


def check_micro_functions(path, names, n32):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name(".symtab")
        flags = elf["e_flags"]
        if (elf.elfclass != (32 if n32 else 64) or bool(flags & 0x20) != n32
                or not flags & 0x02000000):
            raise ValueError(f"{path.name} has unexpected MIPS ABI or encoding")
        for name in names:
            matches = symbols.get_symbol_by_name(name)
            if not matches or not matches[0]["st_other"]["local"] & 4:
                raise ValueError(f"{path.name} has no tagged microMIPS function {name}")


def store_cases(text, data, little):
    order = "little" if little else "big"
    case = 0
    for name in STORES:
        for offset in range(8):
            for value in (0, 0xfedcba9876543210, 0xffffffffffffffff):
                case += 1
                initial = bytes((0x37 + 23 * i) & 255 for i in range(24))
                expected = bytearray(initial)
                encoded = value.to_bytes(8, order)
                if name == "mips_merge_store_double":
                    expected[8 + offset:16 + offset] = encoded
                elif (name == "mips_merge_store_left_double") != little:
                    expected[8 + offset:16] = encoded[:8 - offset]
                else:
                    expected[8:9 + offset] = encoded[7 - offset:]
                text.extend([f"dla $4,buffer{case}+{8 + offset}", f"dli $5,{value:#x}",
                             f"jal {name}", "nop", f"dli $8,{value:#x}",
                             f"bne $2,$8,fail{case}", "nop", f"dla $9,buffer{case}"])
                for i in range(0, 24, 8):
                    word = int.from_bytes(expected[i:i + 8], order)
                    text.extend([f"ld $10,{i}($9)", f"dli $8,{word:#x}",
                                 f"bne $10,$8,fail{case}", "nop"])
                text.extend([f"b pass{case}", "nop", f"fail{case}:", f"li $4,{case}",
                             "break 0x3ff", f"pass{case}:"])
                data.extend([f"buffer{case}:", ".byte " + ",".join(map(str, initial))])
    return case


def division_cases(text):
    mask = (1 << 64) - 1
    limit = 1 << 63
    inputs = [(0, -1), (10, 3), (-10, 3), (10, -3), (-10, -3),
              (-limit, -1), (-limit, 3), (-1, -limit), (limit - 1, -limit),
              (0x100000002, 0x100000001), (0x123456789abcdef, 0x12345), (-1, 1)]
    case = 0
    for name in DIVISIONS:
        for a, b in inputs:
            case += 1
            if "divu" in name:
                a, b = a & mask, b & mask
            q = abs(a) // abs(b)
            if (a < 0) != (b < 0):
                q = -q
            value = a - q * b if name.endswith("_hi") else q
            text.extend([f"dli $4,{a & mask:#x}", f"dli $5,{b & mask:#x}",
                         f"jal {name}", "nop", f"dli $8,{value & mask:#x}",
                         f"beq $2,$8,pass{case}", "nop", f"li $4,{case}",
                         "break 0x3ff", f"pass{case}:"])
    return case


def harness(little, division):
    text = [".text", ".set noreorder", ".set micromips", ".set insn32",
            ".globl _start", ".ent _start", "_start:", "dla $29,stack_end"]
    data = [".data", ".balign 16", ".space 32768", "stack_end:"]
    case = division_cases(text) if division else store_cases(text, data, little)
    text.extend([f"li $4,{SUCCESS}", "break 0x3ff", ".end _start"])
    return "\n".join(text + data) + "\n", case


def run(args, work, little, n32):
    order = "little" if little else "big"
    endian = "-EL" if little else "-EB"
    assemble = [args.assembler, "-mips64r2", "-n32" if n32 else "-64",
                "-msoft-float", "-mmicromips", endian]
    emulation = f"elf{32 if n32 else 64}{'l' if little else 'b'}tsmip" + ("n32" if n32 else "")
    link = [args.linker, "-m", emulation, "-N", "-Ttext", "0x80010000" if n32 else "0xffffffff80010000"]
    cc = [args.cc, "-march=mips64r2", "-mabi=n32" if n32 else "-mabi=64", "-msoft-float",
          "-mno-abicalls", "-minterlink-compressed",
          "-fno-pic", "-ffreestanding", "-fno-strict-aliasing", "-Werror=attributes",
          "-G0", endian]
    division = args.suite == "division"
    functions = DIVISIONS if division else STORES
    fixture = Path(__file__).with_name("doubleword-division.S" if division else "merge-store-word.S")
    (work / fixture.name).write_bytes(fixture.read_bytes())
    source = subprocess.check_output([*cc, "-DWIDE", "-E", "-P",
                                      "-x", "assembler-with-cpp", fixture.name], cwd=work)
    (work / "forms.s").write_bytes(b".text\n.set noreorder\n.set micromips\n.set insn32\n" + source)
    subprocess.run([*assemble, "-o", "forms.o", "forms.s"], cwd=work, check=True)
    subprocess.run([*link, "-e", functions[0], "-o", "forms.elf", "forms.o"], cwd=work, check=True)
    check_micro_functions(work / "forms.elf", functions, n32)
    driver, count = harness(little, division)
    (work / "driver.s").write_text(driver)
    subprocess.run([*assemble, "-o", "driver.o", "driver.s"], cwd=work, check=True)

    def execute(objects, label):
        subprocess.run([*link, "-e", "_start", "-o", "run.elf", "driver.o", *objects],
                       cwd=work, check=True)
        check_micro_functions(work / "run.elf", ("_start", *functions), n32)
        result = subprocess.run([args.simulator, "--endian", order, "run.elf"], cwd=work,
                                timeout=30)
        if result.returncode != SUCCESS:
            raise ValueError(f"{emulation} {label}: simulator exit {result.returncode}")
        print(f"{emulation} {label}: {count} cases passed", flush=True)

    execute(["forms.o"], "original")
    (work / "string.h").write_text("#include <stddef.h>\n"
                                  "void *memcpy(void *, const void *, size_t);\n"
                                  "void *memset(void *, int, size_t);\n")
    for backend in ("highc", "llvmc"):
        for name in functions:
            subprocess.run([args.neverd, "decompile", "--func", name,
                            *(["--llvm"] if backend == "llvmc" else []),
                            "-o", f"{name}.c", "forms.elf"], cwd=work, check=True)
            if not division and "volatile uint8_t" not in (work / f"{name}.c").read_text():
                raise ValueError(f"{backend} lost volatile byte stores in {name}")
        for opt in ("-O0", "-O2"):
            for name in functions:
                subprocess.run([*cc, "-I.", opt, "-c", f"{name}.c", "-o", f"{name}.o"],
                               cwd=work, check=True)
                check_micro_functions(work / f"{name}.o", (name,), n32)
            execute([f"{name}.o" for name in functions], f"{backend} {opt}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("stores", "division"), default="stores")
    for flag in ("neverd", "cc", "assembler", "linker", "simulator"):
        parser.add_argument("--" + flag, type=lambda s: Path(s).absolute(), required=True)
    args = parser.parse_args()
    for little in (False, True):
        for n32 in (False, True):
            with tempfile.TemporaryDirectory(prefix="neverd-micro64-") as directory:
                run(args, Path(directory), little, n32)
