#!/usr/bin/env python3
"""Build and run freestanding MIPS programs through the public relink CLI."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


ABIS = {
    "o32": ("mips", "gnu", "mips32r2", 4001),
    "n32": ("mips64", "gnuabin32", "mips64r2", 6058),
    "n64": ("mips64", "gnuabi64", "mips64r2", 5058),
}
MODES = ("section", "inplace")
STARTUP = """
    .set noreorder
    .section .text.start,"ax",@progbits
    .globl _start
    .ent _start
_start:
    jal calc
    addiu $4, $zero, 0
    move $16, $2
    jal calc
    addiu $4, $zero, 1
    addu $16, $16, $2
    jal calc
    addiu $4, $zero, 7
    addu $4, $16, $2
    addiu $2, $zero, EXIT_NR
    syscall
    .end _start
"""
CALC = """
source_filename = "calc.c"
define i32 @calc(i32 %x) section ".text.calc" {
  %twice = shl i32 %x, 1
  %result = add i32 %twice, ADJUST
  ret i32 %result
}
@neighbor = constant i32 12345
"""
LAYOUT = """
ENTRY(_start)
SECTIONS {
  . = 0x400000;
  .text : { *(.text.start) OLD_CALC *(.text) }
  NEW_CALC
  . = ALIGN(0x10000);
  .MIPS.abiflags : { *(.MIPS.abiflags) }
  .MIPS.options : { *(.MIPS.options) }
  .reginfo : { *(.reginfo) }
  .rodata : { *(.rodata*) }
  . = ALIGN(0x10000);
  .data : { *(.data*) }
  .got : { *(.got*) }
  .bss : { *(.bss*) }
}
"""


def profiles():
    for abi, (arch, env, cpu, syscall) in ABIS.items():
        qemu = {"o32": "mips", "n32": "mipsn32", "n64": "mips64"}[abi]
        for endian, suffix in (("be", ""), ("le", "el")):
            yield (f"{abi}-{endian}", f"{arch}{suffix}-unknown-linux-{env}",
                   abi, cpu, syscall, f"qemu-{qemu}{suffix}")


def write(path, body):
    path.write_text(body, encoding="utf-8")


def encode(path, data):
    write(path, json.dumps(data, indent=2, sort_keys=True) + "\n")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, *, error=None, expected=0, source=None):
    result = subprocess.run(args, input=source, text=True, capture_output=True)
    if error is None:
        passed = result.returncode == expected
    else:
        passed = result.returncode != 0 and error in result.stdout + result.stderr
    if not passed:
        raise RuntimeError(f"{args} returned {result.returncode}, expected "
                           f"{error or expected}\n{result.stdout}\n{result.stderr}")


def qualify(args):
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    results = {}
    with tempfile.TemporaryDirectory(prefix="mips relink ") as temp:
        for name, triple, abi, cpu, syscall, _ in profiles():
            out = root / name
            out.mkdir(exist_ok=True)
            build = Path(temp) / name
            build.mkdir()
            target = [f"-mcpu={cpu}", f"-target-abi={abi}", "-mattr=+noabicalls"]
            run([args.mc, "-filetype=obj", f"-triple={triple}", *target, "-o",
                 build / "start.o", "-"], source=STARTUP.replace("EXIT_NR", str(syscall)))

            def compile_calc(filename, ir):
                run([args.llc, "-filetype=obj", f"-mtriple={triple}", *target,
                     "-relocation-model=static", "-o", build / filename, "-"], source=ir)

            compile_calc("calc.o", CALC.replace("ADJUST", "3"))
            compile_calc("updated.o", CALC.replace("ADJUST", "7"))
            write(build / "base.ld", LAYOUT.replace("OLD_CALC", "*(.text.calc)")
                  .replace("NEW_CALC", ""))
            write(build / "section.ld", LAYOUT.replace("OLD_CALC", "").replace(
                "NEW_CALC", ".calc.update ALIGN(0x1000) : { *(.text.calc) }"))
            original = out / "original.elf"
            run([args.lld, "-static", "-T", build / "base.ld", "-o", original,
                 build / "start.o", build / "calc.o"])
            manifest = dict(objects=["start.o", "calc.o"], original="calc.o",
                            replacement="updated.o", function="calc", layout="base.ld",
                            section=".calc.update", sectionLayout="section.ld")
            manifest_path = build / "manifest.json"
            encode(manifest_path, manifest)
            command = [args.neverd, "patch", original, f"--relink={manifest_path}",
                       f"--linker-path={args.lld}"]
            for mode in MODES:
                run([*command, f"--mode={mode}", "-o", out / f"{mode}.elf"])
            # Exercise atomic replacement of the input pathname as well.
            same = build / "same.elf"
            shutil.copyfile(original, same)
            run([*command[:2], same, *command[3:], "--mode=inplace", "-o", same])
            if digest(same) != digest(out / "inplace.elf"):
                raise RuntimeError(f"{name}: inconsistent same-path result")

            sentinel = build / "prior output"
            sentinel.write_bytes(b"existing product\n")

            def reject(mode, diagnostic, **changes):
                encode(manifest_path, {**manifest, **changes})
                run([*command, f"--mode={mode}", "-o", sentinel], error=diagnostic)
                if sentinel.read_bytes() != b"existing product\n":
                    raise RuntimeError(f"{name}: failed link published an output")

            reject("section", "do not reproduce", objects=["start.o", "updated.o"],
                   original="updated.o")
            reject("section", "missing defined global function", function="absent")
            reject("section", "separate section", sectionLayout="base.ld")
            reject("section", "separate section", section=".wrong")
            reject("inplace", "missing defined global function", replacement="start.o")
            reject("inplace", "exactly one entry", original="absent.o")
            opposite = next(t for n, t, a, *_ in profiles() if a == abi and n != name)
            run([args.llc, "-filetype=obj", f"-mtriple={opposite}", *target,
                 "-relocation-model=static", "-o", build / "wrong-endian.o", "-"],
                source=CALC.replace("ADJUST", "7"))
            reject("section", "ELF linker exited", replacement="wrong-endian.o")
            compile_calc("adjacent.o", CALC.replace("ADJUST", "7").replace("12345", "9"))
            reject("inplace", "other loaded bytes", replacement="adjacent.o")
            large = CALC.replace("%twice = shl i32 %x, 1",
                                 "%a = mul i32 %x, 314159\n  %twice = xor i32 %a, 271828")
            compile_calc("large.o", large.replace("ADJUST", "7"))
            reject("inplace", "does not fit", replacement="large.o")
            encode(manifest_path, manifest)
            run([*command, "--from-object=updated.o", "-o", sentinel],
                error="cannot be combined")
            results[name] = {case: digest(out / f"{case}.elf")
                             for case in ("original", *MODES)}
    encode(root / "products.json", results)
    print(f"Relinked {len(results)} profiles in both modes")


def execute(args):
    root = args.root.resolve()
    hashes = json.loads((root / "products.json").read_text())
    results = {}
    for name, _, abi, _, _, qemu in profiles():
        row = {}
        for case, expected in (("original", 25), ("section", 37), ("inplace", 37)):
            path = root / name / f"{case}.elf"
            if digest(path) != hashes[name][case]:
                raise RuntimeError(f"{path}: SHA-256 mismatch")
            # Verify the emulator matches the actual ELF class, ABI and byte order.
            header = path.read_bytes()[:64]
            endian = ">" if name.endswith("-be") else "<"
            fl_off = 48 if abi == "n64" else 36
            flags = struct.unpack_from(endian + "I", header, fl_off)[0]
            if (header[:4] != b"\x7fELF" or header[4] != (2 if abi == "n64" else 1)
                    or header[5] != (2 if endian == ">" else 1)
                    or struct.unpack_from(endian + "HH", header, 16) != (2, 8)
                    or bool(flags & 0x20) != (abi == "n32")
                    or flags & 0xf000 != (0x1000 if abi == "o32" else 0)):
                raise RuntimeError(f"{path}: incorrect MIPS ABI")
            path.chmod(0o755)
            run([qemu, path], expected=expected)
            row[case] = expected
        results[name] = row
    encode(root / "execution.json", results)
    print(f"Executed {3 * len(results)} complete ELF files: original=25, section=inplace=37")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    patch = commands.add_parser("qualify")
    patch.add_argument("--root", required=True, type=Path)
    for tool in ("neverd", "mc", "llc", "lld"):
        patch.add_argument("--" + tool, required=True)
    runtime = commands.add_parser("execute")
    runtime.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    (qualify if args.command == "qualify" else execute)(args)
