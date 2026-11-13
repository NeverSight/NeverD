#!/usr/bin/env python3
"""Run original and regenerated microMIPS64r2 merge stores on GNU sim."""

import argparse
import subprocess
import tempfile
from pathlib import Path

from elftools.elf.elffile import ELFFile

FUNCTIONS = ("mips_merge_store_left_double", "mips_merge_store_right_double",
             "mips_merge_store_double")
SUCCESS = 211


def check_micro_functions(path, names):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name(".symtab")
        if elf.elfclass != 64 or not elf["e_flags"] & 0x02000000:
            raise ValueError(f"{path.name} has no n64 microMIPS image")
        for name in names:
            matches = symbols.get_symbol_by_name(name)
            if not matches or not matches[0]["st_other"]["local"] & 4:
                raise ValueError(f"{path.name} has no tagged microMIPS function {name}")


def harness(little):
    text = [".text", ".set noreorder", ".set micromips", ".set insn32",
            ".globl _start", ".ent _start", "_start:", "dla $29,stack_end"]
    data = [".data", ".balign 16", ".space 32768", "stack_end:"]
    order = "little" if little else "big"
    case = 0
    for name in FUNCTIONS:
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
    text.extend([f"li $4,{SUCCESS}", "break 0x3ff", ".end _start"])
    return "\n".join(text + data) + "\n", case


def run(args, work, little):
    order = "little" if little else "big"
    endian = "-EL" if little else "-EB"
    assemble = [args.assembler, "-mips64r2", "-64", "-mmicromips", endian]
    link = [args.linker, "-m", f"elf64{'l' if little else 'b'}tsmip", "-N",
            "-Ttext", "0xffffffff80010000"]
    cc = [args.cc, "-march=mips64r2", "-mabi=64", "-msoft-float", "-mno-abicalls",
          "-fno-pic", "-ffreestanding", "-fno-strict-aliasing", "-Werror=attributes",
          "-G0", endian]
    fixture = Path(__file__).with_name("merge-store-word.S")
    (work / fixture.name).write_bytes(fixture.read_bytes())
    source = subprocess.check_output([*cc, "-DWIDE", "-E", "-P",
                                      "-x", "assembler-with-cpp", fixture.name], cwd=work)
    (work / "forms.s").write_bytes(b".text\n.set noreorder\n.set micromips\n.set insn32\n" + source)
    subprocess.run([*assemble, "-o", "forms.o", "forms.s"], cwd=work, check=True)
    subprocess.run([*link, "-e", FUNCTIONS[0], "-o", "forms.elf", "forms.o"], cwd=work, check=True)
    check_micro_functions(work / "forms.elf", FUNCTIONS)
    driver, count = harness(little)
    (work / "driver.s").write_text(driver)
    subprocess.run([*assemble, "-o", "driver.o", "driver.s"], cwd=work, check=True)

    def execute(objects, label):
        subprocess.run([*link, "-e", "_start", "-o", "run.elf", "driver.o", *objects],
                       cwd=work, check=True)
        check_micro_functions(work / "run.elf", ("_start", *FUNCTIONS))
        result = subprocess.run([args.simulator, "--endian", order, "run.elf"], cwd=work,
                                timeout=30)
        if result.returncode != SUCCESS:
            raise ValueError(f"{order} {label}: simulator exit {result.returncode}")
        print(f"{order} {label}: {count} cases passed", flush=True)

    execute(["forms.o"], "original")
    (work / "string.h").write_text("#include <stddef.h>\n"
                                  "void *memcpy(void *, const void *, size_t);\n"
                                  "void *memset(void *, int, size_t);\n")
    for backend in ("highc", "llvmc"):
        for name in FUNCTIONS:
            subprocess.run([args.neverd, "decompile", "--func", name,
                            *(["--llvm"] if backend == "llvmc" else []),
                            "-o", f"{name}.c", "forms.elf"], cwd=work, check=True)
        for opt in ("-O0", "-O2"):
            for name in FUNCTIONS:
                subprocess.run([*cc, "-I.", opt, "-c", f"{name}.c", "-o", f"{name}.o"],
                               cwd=work, check=True)
                check_micro_functions(work / f"{name}.o", (name,))
            execute([f"{name}.o" for name in FUNCTIONS], f"{backend} {opt}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("neverd", "cc", "assembler", "linker", "simulator"):
        parser.add_argument("--" + flag, type=lambda s: Path(s).absolute(), required=True)
    args = parser.parse_args()
    for little in (False, True):
        with tempfile.TemporaryDirectory(prefix="neverd-micro64-stores-") as directory:
            run(args, Path(directory), little)
