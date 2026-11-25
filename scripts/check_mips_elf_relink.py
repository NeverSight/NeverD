#!/usr/bin/env python3
"""Build and run freestanding MIPS programs through the public relink CLI."""

import argparse
from dataclasses import dataclass, replace
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ABIS = {
    "o32": ("mips", "gnu", 4001),
    "n32": ("mips64", "gnuabin32", 6058),
    "n64": ("mips64", "gnuabi64", 5058),
}
MODES = ("section", "inplace")
STARTUP = """
    .set noreorder
    SET_NOAT
    .section .text.start,"ax",@progbits
    .globl _start
    .ent _start
_start:
    CALL_0
    SAVE_FIRST
    CALL_1
    SUM_FIRST_TWO
    CALL_7
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
MIPS16_CALLER = """
declare i32 @calc(i32)
define i32 @invoke(i32 %x) nounwind section ".text.start" align 4 {
  %result = notail call i32 @calc(i32 %x)
  ret i32 %result
}
"""
LAYOUT = """
ENTRY(_start)
SECTIONS {
  . = BASE_ADDRESS;
  .text : { *(.text.start) OLD_CALC *(.text) RESERVE } FILL
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


@dataclass(frozen=True)
class Profile:
    abi: str
    little: bool
    isa: str = "classic"
    release: int = 2
    shrinking: bool = False

    @property
    def name(self):
        suffix = f"{self.abi}-{'le' if self.little else 'be'}"
        return f"{self.isa}-r{self.release}-{suffix}" if self.shrinking else suffix

    @property
    def triple(self):
        arch, env, _ = ABIS[self.abi]
        return f"{arch}{'el' if self.little else ''}-unknown-linux-{env}"

    @property
    def exit_nr(self):
        return ABIS[self.abi][2]

    @property
    def cpu(self):
        return f"mips{32 if self.abi == 'o32' else 64}r{self.release}"


def profiles():
    for abi in ABIS:
        for little in (False, True):
            yield Profile(abi, little)
            for isa in ("classic", "micromips"):
                for release in (2, 6):
                    yield Profile(abi, little, isa, release, shrinking=True)
            if abi == "o32":
                yield Profile(abi, little, "mips16", shrinking=True)


def startup(profile):
    source = STARTUP
    callee = "invoke" if profile.isa == "mips16" else "calc"
    for value in (0, 1, 7):
        arg = f"addiu $4, $zero, {value}"
        call = f"{arg}\n    balc {callee}" if profile.release == 6 else f"jal {callee}\n    {arg}"
        source = source.replace(f"CALL_{value}", call)
    # Cover the scratch registers used by the larger body in both versions.
    save = "move $1, $2\n    move $16, $1" if profile.shrinking else "move $16, $2"
    add = "addu $3, $16, $2\n    move $16, $3" if profile.shrinking else "addu $16, $16, $2"
    return (source.replace("SAVE_FIRST", save).replace("SUM_FIRST_TWO", add)
            .replace("SET_NOAT", ".set noat" if profile.shrinking else "")
            .replace("EXIT_NR", str(profile.exit_nr)))


def calculator(profile, updated):
    if not profile.shrinking:
        return CALC.replace("ADJUST", "7" if updated else "3")
    # A changing FDE size would legitimately change loaded bytes outside calc.
    ir = CALC.replace('section ".text.calc"', 'nounwind section ".text.calc"')
    if updated:
        return ir.replace("  %twice = shl i32 %x, 1\n", "").replace("%twice, ADJUST", "%x, 7")
    return ir.replace("shl i32 %x, 1", "mul i32 %x, 9").replace("ADJUST", "3")


def layout(profile, section):
    base = 0x120000000 if profile.shrinking and profile.abi == "n64" else 0x400000
    return (LAYOUT.replace("BASE_ADDRESS", hex(base + (0x100000 if section else 0)))
            .replace("RESERVE", ". = ADDR(.text) + 0x100;" if profile.shrinking else "")
            .replace("FILL", "=0" if profile.shrinking else "")
            .replace("OLD_CALC", "" if section else "*(.text.calc)")
            .replace("NEW_CALC", ".calc.update ALIGN(0x1000) : { *(.text.calc) }" if section else ""))


def write(path, body):
    path.write_text(body, encoding="utf-8")


def encode(path, data):
    write(path, json.dumps(data, indent=2, sort_keys=True) + "\n")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, *, error=None, expected=0, source=None):
    result = subprocess.run(args, input=source, text=True, capture_output=True, timeout=60)
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
        for profile in profiles():
            name = profile.name
            out = root / name
            out.mkdir(exist_ok=True)
            build = Path(temp) / name
            build.mkdir()
            target = [f"-mcpu={profile.cpu}", f"-target-abi={profile.abi}"]
            features = "+noabicalls" + (f",+{profile.isa}" if profile.isa != "classic" else "")
            run([args.mc, "-filetype=obj", f"-triple={profile.triple}", *target,
                 "-mattr=+noabicalls", "-o", build / "start.o", "-"], source=startup(profile))

            def compile_object(filename, ir, triple=profile.triple):
                run([args.llc, "-filetype=obj", f"-mtriple={triple}", *target, f"-mattr={features}",
                     "-relocation-model=static", "-o", build / filename, "-"], source=ir)

            compile_object("calc.o", calculator(profile, False))
            compile_object("updated.o", calculator(profile, True))
            write(build / "base.ld", layout(profile, False))
            section_layout = layout(profile, True)
            write(build / "section.ld", section_layout)
            write(build / "wrong-entry.ld", section_layout.replace("ENTRY(_start)", "ENTRY(calc)"))
            objects = ["start.o"]
            if profile.isa == "mips16":
                compile_object("invoke.o", MIPS16_CALLER)
                objects.append("invoke.o")
            objects.append("calc.o")
            original = out / "original.elf"
            run([args.lld, "-static", "-T", build / "base.ld", "-o", original,
                 *(build / obj for obj in objects)])
            manifest = dict(objects=objects, original="calc.o",
                            replacement="updated.o", function="calc", layout="base.ld",
                            section=".calc.update", sectionLayout="section.ld")
            manifest_path = build / "manifest.json"
            encode(manifest_path, manifest)
            command = [args.neverd, "patch", original, f"--relink={manifest_path}",
                       f"--linker-path={args.lld}"]
            for mode in MODES:
                run([*command, f"--mode={mode}", "-o", out / f"{mode}.elf"])
            results[name] = {case: digest(out / f"{case}.elf") for case in ("original", *MODES)}
            if profile.shrinking:
                continue

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
            reject("section", "changed the program entry", sectionLayout="wrong-entry.ld")
            reject("inplace", "missing defined global function", replacement="start.o")
            reject("inplace", "exactly one entry", original="absent.o")
            opposite = replace(profile, little=not profile.little)
            compile_object("wrong-endian.o", calculator(profile, True), opposite.triple)
            reject("section", "ELF linker exited", replacement="wrong-endian.o")
            compile_object("adjacent.o", CALC.replace("ADJUST", "7").replace("12345", "9"))
            reject("inplace", "other loaded bytes", replacement="adjacent.o")
            large = CALC.replace("%twice = shl i32 %x, 1",
                                 "%a = mul i32 %x, 314159\n  %twice = xor i32 %a, 271828")
            compile_object("large.o", large.replace("ADJUST", "7"))
            reject("inplace", "does not fit", replacement="large.o")
            encode(manifest_path, manifest)
            run([*command, "--from-object=updated.o", "-o", sentinel],
                error="cannot be combined")
    encode(root / "products.json", {"version": 2, "profiles": results})
    print(f"Relinked {len(results)} profiles in both modes")


def inspect_elf(image, profile):
    # This runs on the execution host. Native producers only need Python's stdlib.
    abi = profile.abi
    flags = image["e_flags"]
    arch = (7 if profile.release == 2 else 9) + (abi != "o32")
    extension = {"classic": 0, "micromips": 0x02000000, "mips16": 0x04000000}[profile.isa]
    if (image["e_type"] != "ET_EXEC" or image["e_machine"] != "EM_MIPS"
            or image.little_endian != profile.little or image.elfclass != (64 if abi == "n64" else 32)
            or flags >> 28 != arch or flags & 0x06000000 != extension
            or bool(flags & 0x20) != (abi == "n32")
            or flags & 0xf000 != (0x1000 if abi == "o32" else 0)):
        raise ValueError(f"{profile.name}: unexpected ELF header or ABI flags {flags:#x}")

    symtab = image.get_section_by_name(".symtab")
    (index, symbol), = ((i, s) for i, s in enumerate(symtab.iter_symbols()) if s.name == "calc")
    # pyelftools exposes the generic ELF visibility fields. MIPS uses the full high nibble.
    other = symtab.data()[index * symtab["sh_entsize"] + (5 if image.elfclass == 64 else 13)]
    if other & 0xf0 != {"classic": 0, "micromips": 0x80, "mips16": 0xf0}[profile.isa]:
        raise ValueError(f"{profile.name}: incorrect calc ISA marker {other:#x}")
    section = image.get_section(symbol["st_shndx"])
    va, size = symbol["st_value"], symbol["st_size"]
    if (symbol["st_info"]["type"] != "STT_FUNC" or not section["sh_flags"] & 4 or size <= 0
            or not section["sh_addr"] <= va <= section["sh_addr"] + section["sh_size"] - size
            or (profile.shrinking and abi == "n64" and va < 1 << 32)):
        raise ValueError(f"{profile.name}: invalid calc range {va:#x}+{size:#x} in {section.name}")
    return dict(va=va, offset=section["sh_offset"] + va - section["sh_addr"], size=size, section=section.name)


def execute_image(path, image, profile, expected):
    if profile.isa == "micromips" and profile.abi != "o32":
        from mips_elf_runtime import run_micromips64
        run_micromips64(image, profile.release, profile.exit_nr, expected)
        return
    arch = {"o32": "mips", "n32": "mipsn32", "n64": "mips64"}[profile.abi]
    qemu = f"qemu-{arch}{'el' if profile.little else ''}"
    cpu = None
    if profile.release == 6:
        cpu = "mips32r6-generic" if profile.abi == "o32" else "I6400"
    elif profile.isa == "micromips":
        cpu = "M14Kc"
    elif profile.isa == "mips16":
        cpu = "74Kf"
    path.chmod(0o755)
    run([qemu, *(["-cpu", cpu] if cpu else []), path], expected=expected)


def execute(args):
    from elftools.elf.elffile import ELFFile

    root = args.root.resolve()
    manifest = json.loads((root / "products.json").read_text())
    cases = list(profiles())
    if "version" in manifest:
        if manifest["version"] != 2:
            raise ValueError("Unknown MIPS relink products version")
        hashes = manifest["profiles"]
    else:
        # Archived original-format manifests contain the six classic R2 cases.
        cases = [p for p in cases if not p.shrinking]
        hashes = manifest
    if set(hashes) != {p.name for p in cases}:
        raise ValueError("MIPS relink products do not match the expected profile set")
    results, placements = {}, {}
    for profile in cases:
        row, ranges = {}, {}
        original, updated = (81, 29) if profile.shrinking else (25, 37)
        for case, expected in (("original", original), ("section", updated), ("inplace", updated)):
            path = root / profile.name / f"{case}.elf"
            if digest(path) != hashes[profile.name][case]:
                raise ValueError(f"{path}: SHA-256 mismatch")
            with path.open("rb") as source:
                image = ELFFile(source)
                ranges[case] = inspect_elf(image, profile)
                execute_image(path, image, profile, expected)
            row[case] = expected
        old, section, inplace = (ranges[case] for case in ("original", *MODES))
        if (inplace["va"] != old["va"] or inplace["offset"] != old["offset"]
                or section["section"] != ".calc.update"
                or not (section["va"] >= old["va"] + old["size"] or old["va"] >= section["va"] + section["size"])
                or (profile.shrinking and not (section["size"] < old["size"] and inplace["size"] < old["size"]))):
            raise ValueError(f"{profile.name}: incorrect function placement {ranges}")
        results[profile.name], placements[profile.name] = row, ranges
    encode(root / "execution.json", results)
    encode(root / "placements.json", placements)
    print(f"Executed {3 * len(results)} complete ELF files across {len(results)} profiles")


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
