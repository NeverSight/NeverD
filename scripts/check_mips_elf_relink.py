#!/usr/bin/env python3
"""Build and run freestanding MIPS programs through the public relink CLI."""

import argparse
from dataclasses import dataclass, replace
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile

from mips_relink_linkage import (FIXTURES, SET_THREAD_AREA, build_linkage_objects,
                                 inspect_linkage, verify_linkage_objects)


ABIS = {
    "o32": ("mips", "gnu", 4001),
    "n32": ("mips64", "gnuabin32", 6058),
    "n64": ("mips64", "gnuabi64", 5058),
}
MODES = ("section", "inplace")
PRODUCT_VERSION = 13
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
GROWING_CALC = """
source_filename = "calc.c"
define i32 @calc(i32 %x) nounwind section ".text.calc" {
  %a = xor i32 %x, 42
  %b = shl i32 %a, 2
  %c = add i32 %b, %x
  %d = lshr i32 %c, 3
  %e = xor i32 %c, %d
  %result = and i32 %e, 63
  ret i32 %result
}
@neighbor = constant i32 12345
"""
MIPS16_ENTRY = """
declare void @run_calls() noreturn
define void @_start() noreturn nounwind section ".text.start" align 4 {
  notail call void @run_calls()
  unreachable
}
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
PROGRAM_HEADERS
SECTIONS {
  . = BASE_ADDRESS;
  .text : { *(.text.start) OLD_CALC *(.text) RESERVE } ENTRY_SEGMENT FILL
  NEW_CALC
  . = ALIGN(0x10000);
  .MIPS.abiflags : { *(.MIPS.abiflags) } DATA_SEGMENT
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
    compressed_entry: bool = False
    historical_isa: int = 0
    linkage: str = ""
    gp64_o32: bool = False
    dwarf_version: int = 0
    entry_alias: bool = False
    c_call: bool = False

    @property
    def high_address(self):
        return self.abi == "n64" and bool(self.shrinking or self.linkage or self.dwarf_version
                                          or self.entry_alias or self.c_call)

    @property
    def tls_model(self):
        return {"tls": "localexec", "tls-ie": "initialexec",
                "tls-bss": "localexec", "tls-bss-ie": "initialexec"}.get(self.linkage)

    @property
    def tls_blocks(self):
        return self.linkage in ("tls-bss", "tls-bss-ie")

    @property
    def soft_float(self):
        return self.linkage in ("pic-soft", "far-branches") or self.tls_blocks

    @property
    def name(self):
        suffix = f"{self.abi}-{'le' if self.little else 'be'}"
        if self.c_call:
            return f"c-call-{self.isa}-{suffix}"
        if self.entry_alias:
            return f"entry-alias-{self.isa}-{suffix}"
        if self.dwarf_version:
            return f"dwarf{self.dwarf_version}-{suffix}"
        if self.gp64_o32:
            suffix = "gp64-" + suffix
        if self.linkage:
            return f"{self.linkage}-{self.isa}-r{self.release}-{suffix}"
        if self.historical_isa:
            return f"mips{self.historical_isa}-{suffix}"
        name = f"{self.isa}-r{self.release}-{suffix}" if self.shrinking else suffix
        return name + "-compressed-entry" if self.compressed_entry else name

    @property
    def triple(self):
        arch, env, _ = ABIS[self.abi]
        return f"{arch}{'el' if self.little else ''}-unknown-linux-{env}"

    @property
    def exit_nr(self):
        return ABIS[self.abi][2]

    @property
    def release_isa_bits(self):
        return 64 if self.gp64_o32 or self.abi != "o32" else 32

    @property
    def cpu(self):
        if self.historical_isa:
            return f"mips{self.historical_isa}"
        base = f"mips{self.release_isa_bits}"
        return base if self.release == 1 else f"{base}r{self.release}"


def profiles(version=PRODUCT_VERSION):
    for abi in ABIS:
        for little in (False, True):
            yield Profile(abi, little)
            if version == 1:
                continue
            if version >= 12:
                encodings = ("classic", "micromips", "mips16") if abi == "o32" else ("classic", "micromips")
                for isa in encodings:
                    yield Profile(abi, little, isa, compressed_entry=isa != "classic", entry_alias=True)
                    if version >= 13:
                        yield Profile(abi, little, isa, compressed_entry=isa != "classic", c_call=True)
            if version >= 11:
                for dwarf_version in (4, 5):
                    yield Profile(abi, little, dwarf_version=dwarf_version)
            if version >= 9:
                yield Profile(abi, little, linkage="callback-table")
            if version >= 8:
                yield Profile(abi, little, linkage="large-segments")
            if version >= 7:
                yield Profile(abi, little, linkage="init-arrays")
            if version >= 4:
                first_level = 1 if abi == "o32" else 3
                last_level = 3 if version == 4 and abi == "o32" else 5
                for level in range(first_level, last_level + 1):
                    yield Profile(abi, little, shrinking=True, historical_isa=level)
            for profile in scalar_release_profiles(abi, little, version):
                yield profile
                if version >= 5 and abi == "o32":
                    yield replace(profile, gp64_o32=True)

            if version >= 4:
                workloads = ("pic-hard", "pic-soft", "small-data", "tls", "tls-ie")
                if version >= 6:
                    workloads += ("tls-bss", "tls-bss-ie")
                for release in (2, 6):
                    for isa in ("classic", "micromips"):
                        for linkage in workloads:
                            yield Profile(abi, little, isa, release, linkage=linkage)
                if abi == "o32":
                    for linkage in workloads:
                        yield Profile(abi, little, "mips16", linkage=linkage)
                    if version >= 6:
                        yield Profile(abi, little, "mips16", linkage="far-branches")
                yield Profile(abi, little, linkage="dsp")
                yield Profile(abi, little, release=5, linkage="msa")


def scalar_release_profiles(abi, little, version):
    for isa in ("classic", "micromips"):
        releases = (2, 6)
        if version >= 4:
            releases = (1, 2, 3, 5, 6) if isa == "classic" else (2, 3, 5, 6)
        for release in releases:
            profile = Profile(abi, little, isa, release, shrinking=True)
            yield profile
            if isa == "micromips" and version >= 3:
                yield replace(profile, compressed_entry=True)
    if abi == "o32":
        profile = Profile(abi, little, "mips16", shrinking=True)
        yield profile
        if version >= 3:
            yield replace(profile, compressed_entry=True)


def startup(profile):
    if profile.entry_alias or profile.c_call:
        return ((FIXTURES / "entry_exit.s").read_text()
                .replace("EXIT_ISA", ".set micromips" if profile.isa == "micromips" else "")
                .replace("EXIT_NR", str(profile.exit_nr)))
    source = STARTUP
    callee = "invoke" if profile.isa == "mips16" else "calc"
    for value in (0, 1, 7):
        arg = f"addiu $4, $zero, {value}"
        call = f"{arg}\n    balc {callee}" if profile.release == 6 else f"jal {callee}\n    {arg}"
        source = source.replace(f"CALL_{value}", call)
    # Cover the scratch registers used by the larger body in both versions.
    save = "move $1, $2\n    move $16, $1" if profile.shrinking else "move $16, $2"
    add = "addu $3, $16, $2\n    move $16, $3" if profile.shrinking else "addu $16, $16, $2"
    if profile.compressed_entry:
        if profile.isa == "mips16":
            source = source.replace("_start", "run_calls")
        else:
            source = ".set micromips\n" + source
            save = "addu $1, $2, $zero\n    addu $16, $1, $zero"
            add = "addu $3, $16, $2\n    addu $16, $3, $zero"
    return (source.replace("SAVE_FIRST", save).replace("SUM_FIRST_TWO", add)
            .replace("SET_NOAT", ".set noat" if profile.shrinking else "")
            .replace("EXIT_NR", str(profile.exit_nr)))


def calculator(profile, updated):
    if profile.entry_alias:
        return (FIXTURES / "entry_alias.ll").read_text().replace("STATUS", "37" if updated else "25")
    if profile.dwarf_version:
        return ((FIXTURES / "debug_calc.ll").read_text()
                .replace("DWARF_VERSION", str(profile.dwarf_version))
                .replace("SOURCE_LINE", "29" if updated else "14")
                .replace("ADJUST", "7" if updated else "3"))
    if not profile.shrinking:
        return CALC.replace("ADJUST", "7" if updated else "3")
    # A changing FDE size would legitimately change loaded bytes outside calc.
    ir = CALC.replace('section ".text.calc"', 'nounwind section ".text.calc"')
    if updated:
        return ir.replace("  %twice = shl i32 %x, 1\n", "").replace("%twice, ADJUST", "%x, 7")
    return ir.replace("shl i32 %x, 1", "mul i32 %x, 9").replace("ADJUST", "3")


def layout(profile, section, *, split_entry=False):
    base = 0x120000000 if profile.high_address else 0x400000
    headers = "PHDRS { startup PT_LOAD FLAGS(5); code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }"
    calc = (".calc.update ALIGN(0x10000) : { *(.text.calc) } :code" if split_entry else
            ".calc.update ALIGN(0x1000) : { *(.text.calc) }")
    script = LAYOUT
    if profile.linkage == "init-arrays":
        arrays = []
        for name in ("preinit_array", "init_array", "fini_array"):
            arrays.append(f"  .{name} : {{ __{name}_start = .; KEEP(*(SORT_BY_INIT_PRIORITY(.{name}.*))) "
                          f"KEEP(*(.{name})) __{name}_end = .; }}")
        script = script.replace("  .data :", "\n".join(arrays) + "\n  .data :")
    elif profile.tls_blocks:
        script = script.replace("  .data :", """  .tdata ALIGN(256) : { *(.tdata*) }
  __tls_image = ADDR(.tdata);
  .tbss ALIGN(256) (NOLOAD) : { *(.tbss*) }
  .data :""")
    elif profile.tls_model:
        script = script.replace("  .data :", "  .tdata : { *(.tdata*) }\n  __tls_image = ADDR(.tdata);\n  .data :")
    return (script.replace("BASE_ADDRESS", hex(base + (0x100000 if section else 0)))
            .replace("PROGRAM_HEADERS", headers if split_entry else "")
            .replace("ENTRY_SEGMENT", ":startup" if split_entry else "")
            .replace("DATA_SEGMENT", ":data" if split_entry else "")
            .replace("RESERVE", ". = ADDR(.text) + 0x100;" if profile.shrinking else "")
            .replace("FILL", "=0" if profile.shrinking else "")
            .replace("OLD_CALC", "" if section else "*(.text.calc)")
            .replace("NEW_CALC", calc if section else ""))


def write(path, body):
    path.write_text(body, encoding="utf-8")


def encode(path, data):
    write(path, json.dumps(data, indent=2, sort_keys=True) + "\n")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, *, error=None, expected=0, source=None, cwd=None, env=None):
    result = subprocess.run(args, input=source, text=True, capture_output=True, timeout=60, cwd=cwd, env=env)
    if error is None:
        passed = result.returncode == expected
    else:
        passed = result.returncode != 0 and error in result.stdout + result.stderr
    if not passed:
        raise RuntimeError(f"{args} returned {result.returncode}, expected "
                           f"{error or expected}\n{result.stdout}\n{result.stderr}")


def compile_arithmetic_object(args, build, profile, filename, ir, *, pic=False):
    target = [f"-mcpu={profile.cpu}", f"-target-abi={profile.abi}"]
    features = [] if pic else ["+noabicalls"]
    if profile.isa != "classic":
        features.append("+" + profile.isa)
    run([args.llc, "-filetype=obj", f"-mtriple={profile.triple}", *target, "-mattr=" + ",".join(features),
         "-relocation-model=pic" if pic else "-relocation-model=static", "-o", build / filename, "-"], source=ir)


def assemble_startup(args, build, profile):
    target = [f"-mcpu={profile.cpu}", f"-target-abi={profile.abi}"]
    run([args.mc, "-filetype=obj", f"-triple={profile.triple}", *target,
         "-mattr=+noabicalls", "-o", build / "start.o", "-"], source=startup(profile))


def build_c_call_objects(args, build, profile, replacement_clang=None):
    sources = FIXTURES / "c_abi"
    abi = {"o32": "32", "n32": "n32", "n64": "64"}[profile.abi]
    flags = ["--target=" + profile.triple, "-march=" + profile.cpu, "-mabi=" + abi,
             "-mno-abicalls", "-fno-pic", "-G0", "-O2", "-ffreestanding", "-fno-stack-protector",
             "-fno-ident", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables"]
    if profile.isa != "classic":
        flags.append("-mips16" if profile.isa == "mips16" else "-mmicromips")
    assemble_startup(args, build, profile)
    run([args.clang, *flags, "-c", sources / "start.c", "-o", build / "caller.o"])
    run([args.clang, *flags, "-DREVISION=7", "-c", sources / "calc.c", "-o", build / "calc.o"])
    run([replacement_clang or args.clang, *flags, "-DREVISION=5", "-c", sources / "calc.c",
         "-o", build / "updated.o"])
    return ["caller.o", "start.o", "calc.o"]


def build_arithmetic_objects(args, build, profile):
    assemble_startup(args, build, profile)
    compile_arithmetic_object(args, build, profile, "calc.o", calculator(profile, False))
    compile_arithmetic_object(args, build, profile, "updated.o", calculator(profile, True))
    if profile.entry_alias:
        return ["start.o", "calc.o"]
    if not profile.dwarf_version:
        compile_arithmetic_object(args, build, profile, "grown.o", GROWING_CALC)
    objects = ["start.o"]
    if profile.isa == "mips16":
        compile_arithmetic_object(args, build, profile, "invoke.o", MIPS16_CALLER)
        objects.append("invoke.o")
        if profile.compressed_entry:
            compile_arithmetic_object(args, build, profile, "entry.o", MIPS16_ENTRY)
            objects.append("entry.o")
    objects.append("calc.o")
    return objects


def check_mixed_float_abis(args, builds, products):
    for abi in ABIS:
        for little in (False, True):
            hard = Profile(abi, little, linkage="pic-hard").name
            soft = Profile(abi, little, linkage="pic-soft").name
            build = builds / hard
            manifest = json.loads((build / "manifest.json").read_text())
            shutil.copyfile(builds / soft / "updated.o", build / "soft.o")
            path = build / "mixed.json"
            encode(path, {**manifest, "replacement": "soft.o"})
            previous = build / "prior-output.elf"
            previous.write_bytes(b"existing product\n")
            for mode in MODES:
                run([args.neverd, "patch", products / hard / "original.elf", f"--relink={path}",
                     f"--linker-path={args.lld}", f"--mode={mode}", "-o", previous],
                    error="incompatible with target floating point ABI")
                if previous.read_bytes() != b"existing product\n":
                    raise ValueError(f"{hard}: incompatible {mode} replacement changed the prior output")


def build_upgrade_objects(old, new, build, out, profile, partial, archive_tool=None):
    inputs = build / "inputs" if partial else build
    inputs.mkdir(exist_ok=True)
    assemble_startup(old, inputs, profile)
    if not partial:
        compile_arithmetic_object(old, inputs, profile, "calc.o", calculator(profile, False))
        compile_arithmetic_object(new, inputs, profile, "updated.o", calculator(profile, True))
        return ["start.o", "calc.o"]

    callers = [inputs / "start.o"]
    helper = (FIXTURES / "group_helper.ll").read_text()
    for role, choice in (("left", "0"), ("right", "3")):
        filename = role + ".o"
        ir = helper.replace("SIDE", role).replace("CHOICE", choice)
        compile_arithmetic_object(old, inputs, profile, filename, ir)
        callers.append(inputs / filename)
    calc = (FIXTURES / "group_calc.ll").read_text()
    versions = ((old, "calc.o", "updated.o", "1"), (new, "updated.o", "calc.o", "5"))
    for tools, name, _, adjust in versions:
        compile_arithmetic_object(tools, inputs, profile, name, calc.replace("ADJUST", adjust), pic=partial == "pic")
    if archive_tool:
        unused = (FIXTURES / "archive_unused.ll").read_text()
        compile_arithmetic_object(old, inputs, profile, "unused.o", unused)
    for tools, name, alternate, _ in versions:
        selected = inputs / name
        if archive_tool:
            # Both versions define calc. LLD should pull the first definition
            # and leave the unreferenced member with its unresolved call behind.
            explicit = inputs / (name + ".explicit")
            run([tools.lld, "-r", "-o", explicit, *callers, selected])
            archive = inputs / (name + ".a")
            run([archive_tool, "--format=gnu", "rcsD", archive, selected, inputs / alternate, inputs / "unused.o"])
            selected = archive
        run([tools.lld, "-r", "-o", build / name, *callers, selected])
        if archive_tool and digest(build / name) != digest(explicit):
            raise ValueError(f"{profile.name}: archive member selection changed {name}")
        shutil.copyfile(build / name, out / name)
    return ["calc.o"]


def check_partial_link_order(neverd, old_lld, new_lld, manifest, out):
    build = manifest.parent
    inputs = build / "inputs"
    other = build / "other-order"
    other.mkdir()
    # Use the same original objects and LLD, but select the other weak definition.
    run([old_lld, "-r", "-o", other / "calc.o", inputs / "start.o", inputs / "right.o",
         inputs / "left.o", inputs / "calc.o"])
    original = other / "original.elf"
    run([old_lld, "-static", "-T", build / "base.ld", "-o", original, other / "calc.o"])
    for mode in MODES:
        target = out / f"{mode}.elf"
        previous = digest(target)
        run([neverd, "patch", original, f"--relink={manifest}", f"--mode={mode}",
             f"--original-linker-path={old_lld}", f"--linker-path={new_lld}", "-o", target],
            error="do not reproduce")
        if digest(target) != previous:
            raise ValueError(f"{build.name}: mismatched input order changed the {mode} output")


def c_call_upgrade_profiles():
    # r15 can compile the C caller on classic and microMIPS in both byte orders.
    return [p for p in profiles() if p.c_call and p.isa != "mips16"]


def qualify_upgrade(args):
    """Reproduce a prior fork release's ELF and link the replacement with the current one."""
    def tools_in(directory):
        suffix = ".exe" if (directory / "ld.lld.exe").is_file() else ""
        return argparse.Namespace(clang=directory / ("clang" + suffix), mc=directory / ("llvm-mc" + suffix),
                                  llc=directory / ("llc" + suffix), lld=directory / ("ld.lld" + suffix))

    old, new = tools_in(args.original_tools.resolve()), tools_in(args.tools.resolve())
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    results = {}
    cases = c_call_upgrade_profiles() if args.c_calls else profiles(1)
    with tempfile.TemporaryDirectory(prefix="mips upgrade ") as temp:
        for profile in cases:
            build = Path(temp) / profile.name
            out = root / profile.name
            build.mkdir()
            out.mkdir(exist_ok=True)
            if args.c_calls:
                objects = build_c_call_objects(old, build, profile, replacement_clang=new.clang)
            else:
                objects = build_upgrade_objects(old, new, build, out, profile, args.partial_link, args.archive_tool)
            for separate, name in ((False, "base.ld"), (True, "section.ld")):
                write(build / name, layout(profile, separate))
            original = out / "original.elf"
            run([old.lld, "-static", "-T", build / "base.ld", "-o", original, *(build / p for p in objects)])
            manifest = build / "manifest.json"
            encode(manifest, dict(objects=objects, original="calc.o", replacement="updated.o",
                                  function="calc", layout="base.ld", section=".calc.update", sectionLayout="section.ld"))
            staged = build / "updated" / "calc.o"
            staged.parent.mkdir()
            shutil.copyfile(build / "updated.o", staged)
            for mode in MODES:
                target = out / f"{mode}.elf"
                target.write_bytes(b"prior product\n")
                command = [args.neverd, "patch", original, f"--relink={manifest}", f"--mode={mode}",
                           f"--linker-path={new.lld}", "-o", target]
                # The original linker version is part of the complete file.
                run(command, error="do not reproduce")
                if target.read_bytes() != b"prior product\n":
                    raise ValueError(f"{profile.name}: failed upgrade changed {mode} output")
                run([*command, f"--original-linker-path={old.lld}"])
                script = build / ("section.ld" if mode == "section" else "base.ld")
                reference = build / "reference.elf"
                run([new.lld, "-static", "-T", script, "-o", reference,
                     *(staged if p == "calc.o" else build / p for p in objects)])
                if reference.read_bytes() != target.read_bytes():
                    raise ValueError(f"{profile.name}: {mode} differs from the new linker's output")
            if args.partial_link:
                check_partial_link_order(args.neverd, old.lld, new.lld, manifest, out)
            if not results:
                target = out / "inplace.elf"
                previous = digest(target)
                base = [args.neverd, "patch", original, "-o", target]
                relink = [f"--relink={manifest}", f"--linker-path={new.lld}", "--mode=inplace"]
                missing = "neverd-test-missing-original-linker"
                for flags, error in (
                    ([f"--original-linker-path={old.lld}"], "--original-linker-path requires --relink"),
                    ([*relink, "--original-linker-path="], "--original-linker-path requires an ELF linker"),
                    ([*relink, f"--original-linker-path={missing}"], missing),
                ):
                    run([*base, *flags], error=error)
                    if digest(target) != previous:
                        raise ValueError(f"{profile.name}: rejected linker option changed the prior output")
            results[profile.name] = {case: digest(out / f"{case}.elf") for case in ("original", *MODES)}
    # Existing upgrade archives use the original six-profile format.
    products = ({"version": PRODUCT_VERSION, "suite": "c-call-upgrade", "profiles": results, "objects": {}}
                if args.c_calls else results)
    encode(root / "products.json", products)
    print(f"Relinked {len(results)} original-release programs with the new linker in both modes")


def qualify_second_updates(args, build, out, profile, manifest, patch):
    # The first update replaced calc.o under its original basename. Reproduce
    # that entire executable before changing the function a second time.
    current = build / "first" / "calc.o"
    current.parent.mkdir()
    shutil.copyfile(build / "updated.o", current)
    third = "third.o"
    compile_arithmetic_object(args, build, profile, third, CALC.replace("ADJUST", "11"))
    objects = [current.relative_to(build).as_posix() if name == manifest["original"] else name
               for name in manifest["objects"]]
    newest = [build / (third if name == manifest["original"] else name) for name in manifest["objects"]]
    layouts = {"inplace": ("base.ld", ".text"), "section": ("section.ld", ".calc.update")}
    references = {}
    for script, _ in layouts.values():
        reference = build / "reference.elf"
        run([args.lld, "-static", "-T", build / script, "-o", reference, *newest])
        references[script] = digest(reference)
    results = {}
    for first in MODES:
        previous = out / f"{first}.elf"
        old_script = layouts[first][0]
        moved_script, moved_section = layouts["inplace" if first == "section" else "section"]
        second_manifest = {**manifest, "objects": objects, "original": current.relative_to(build).as_posix(),
                           "replacement": third, "layout": old_script,
                           "sectionLayout": moved_script, "section": moved_section}
        for mode in MODES:
            key = f"after-{first}-{mode}"
            output = out / f"{key}.elf"
            encode(build / "manifest.json", second_manifest)
            patch(mode, output, input_elf=previous)
            expected = references[old_script if mode == "inplace" else moved_script]
            if digest(output) != expected:
                raise ValueError(f"{profile.name}: {key} differs from the independent second link")
            encode(build / "manifest.json", {**second_manifest, "objects": manifest["objects"],
                                             "original": manifest["original"]})
            patch(mode, output, input_elf=previous, error="do not reproduce the input ELF")
            if digest(output) != expected:
                raise ValueError(f"{profile.name}: stale link inputs replaced the {key} output")
            results[key] = expected
    return results


def qualify_named_inputs(args, build, out, profile, expected):
    start, selected, replacement = "startup/unit.o", "../objects/unit.o", "rebuilt/unit.o"
    lld = Path(shutil.which(str(args.lld)) or args.lld).absolute()
    neverd = Path(shutil.which(str(args.neverd)) or args.neverd).absolute()
    with tempfile.TemporaryDirectory(dir=out) as replay:
        directory = Path(replay) / "project"
        directory.mkdir()
        for name, source in ((start, "start.o"), (selected, "calc.o"), (replacement, "updated.o")):
            (directory / name).parent.mkdir()
            shutil.copyfile(build / source, directory / name)
        for mode, script in (("inplace", "base.ld"), ("section", "section.ld")):
            body = layout(profile, mode == "section")
            write(directory / script, body.replace("*(.text.start)", f'"{start}"(.text.start)')
                  .replace("*(.text.calc)", f'"{selected}"(.text.calc)'))
        manifest = directory / "manifest.json"
        encode(manifest, dict(objects=[start, selected], original=selected, replacement=replacement,
                              function="calc", layout="base.ld", sectionLayout="section.ld", section=".calc.update"))
        # Link from the recorded directory with the actual object arguments.
        # The updated object occupies the original argument in each direct link.
        reference = directory / "reference.elf"
        for case, source in (("original", "calc.o"), ("inplace", "updated.o"), ("section", "updated.o")):
            shutil.copyfile(build / source, directory / selected)
            script = "section.ld" if case == "section" else "base.ld"
            run([lld, "-static", "-T", script, "-o", reference, start, selected], cwd=directory)
            if digest(reference) != expected[case]:
                raise ValueError(f"{profile.name}: {case} direct link with named inputs differs")
        shutil.copyfile(build / "calc.o", directory / selected)
        inputs = [directory / name for name in (start, selected, replacement, "base.ld", "section.ld", "manifest.json")]
        for path in inputs:
            path.chmod(0o444)
        def input_state():
            return [(digest(path), stat.S_IMODE(path.stat().st_mode)) for path in inputs]

        before = input_state()
        (directory / "temporary").mkdir()
        env = dict(os.environ, TMPDIR="temporary", TMP="temporary", TEMP="temporary")
        same = directory / "更新 app.elf"
        for mode in MODES:
            shutil.copyfile(out / "original.elf", same)
            same.chmod(0o751)
            permissions = stat.S_IMODE(same.stat().st_mode)
            for error in (None, "do not reproduce"):
                run([neverd, "patch", same.name, "--relink=manifest.json", f"--linker-path={lld}",
                     f"--mode={mode}", "-o", same.name], error=error, cwd=directory, env=env)
                if digest(same) != expected[mode] or stat.S_IMODE(same.stat().st_mode) != permissions:
                    raise ValueError(f"{profile.name}: {mode} same-path bytes or permissions changed")
            same.replace(out / f"{mode}.elf")
        if input_state() != before:
            raise ValueError(f"{profile.name}: relink changed a read-only build input")


def qualify(args):
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    # Existing IR-only tool installations can still produce and replay v12 products.
    version = PRODUCT_VERSION if args.clang else 12
    results, object_hashes = {}, {}
    with tempfile.TemporaryDirectory(prefix="mips relink ") as temp:
        for profile in profiles(version):
            name = profile.name
            out = root / name
            out.mkdir(exist_ok=True)
            build = Path(temp) / name
            build.mkdir()
            if profile.c_call:
                objects = build_c_call_objects(args, build, profile)
            elif profile.linkage:
                objects = build_linkage_objects(args, build, profile, run)
            else:
                objects = build_arithmetic_objects(args, build, profile)
            write(build / "base.ld", layout(profile, False))
            section_layout = layout(profile, True)
            write(build / "section.ld", section_layout)
            write(build / "wrong-entry.ld", section_layout.replace("ENTRY(_start)", "ENTRY(calc)"))
            original = out / "original.elf"
            run([args.lld, "-static", "-T", build / "base.ld", "-o", original,
                 *(build / obj for obj in objects)])
            manifest = dict(objects=objects, original="calc.o",
                            replacement="updated.o", function="calc", layout="base.ld",
                            section=".calc.update", sectionLayout="section.ld")
            manifest_path = build / "manifest.json"
            encode(manifest_path, manifest)
            options = [f"--relink={manifest_path}", f"--linker-path={args.lld}"]

            def patch(mode, output, *, input_elf=original, error=None):
                run([args.neverd, "patch", input_elf, *options, f"--mode={mode}", "-o", output],
                    error=error)

            for mode in MODES:
                patch(mode, out / f"{mode}.elf")
                if (profile.c_call or profile.entry_alias or profile.dwarf_version
                        or profile.linkage in ("init-arrays", "large-segments", "callback-table")):
                    reference = build / "reference.elf"
                    script = build / ("section.ld" if mode == "section" else "base.ld")
                    run([args.lld, "-static", "-T", script, "-o", reference,
                         *(build / ("updated.o" if obj == manifest["original"] else obj) for obj in objects)])
                    if digest(reference) != digest(out / f"{mode}.elf"):
                        raise ValueError(f"{name}: {mode} output differs from the direct link")
            results[name] = {case: digest(out / f"{case}.elf") for case in ("original", *MODES)}
            if profile.c_call or profile.dwarf_version or profile.entry_alias:
                continue
            if profile.linkage:
                object_hashes[name] = {}
                for obj in ("caller.o", "calc.o"):
                    shutil.copyfile(build / obj, out / obj)
                    object_hashes[name][obj] = digest(out / obj)
                continue
            split = layout(profile, True, split_entry=True)
            write(build / "grown.ld", split)
            encode(manifest_path, {**manifest, "replacement": "grown.o", "sectionLayout": "grown.ld"})
            patch("section", out / "section-grown.elf")
            results[name]["section-grown"] = digest(out / "section-grown.elf")
            if profile == Profile(profile.abi, profile.little):
                results[name].update(qualify_second_updates(args, build, out, profile, manifest, patch))

            sentinel = build / "prior output"
            sentinel.write_bytes(b"existing product\n")

            def reject(mode, diagnostic, *, input_elf=original, **changes):
                encode(manifest_path, {**manifest, **changes})
                patch(mode, sentinel, input_elf=input_elf, error=diagnostic)
                if sentinel.read_bytes() != b"existing product\n":
                    raise RuntimeError(f"{name}: failed link published an output")

            reject("inplace", "does not fit", replacement="grown.o")
            entry_error = "program entry is outside file-backed executable load segments"
            write(build / "readonly-entry.ld",
                  split.replace("startup PT_LOAD FLAGS(5)", "startup PT_LOAD FLAGS(4)"))
            write(build / "noload-entry.ld", split.replace(".text :", ".text (NOLOAD) :"))
            for script in ("readonly-entry.ld", "noload-entry.ld"):
                reject("section", entry_error, sectionLayout=script)
            if profile.shrinking:
                continue

            # The manifest path must still apply the ELF loader's ISA admission.
            invalid_isa = bytearray(original.read_bytes())
            offset = 48 if profile.abi == "n64" else 36
            order = "little" if profile.little else "big"
            flags = int.from_bytes(invalid_isa[offset:offset + 4], order)
            invalid_isa[offset:offset + 4] = (flags | 0x00010000).to_bytes(4, order)  # EF_MIPS_MACH
            invalid_input = build / "vendor-isa.elf"
            invalid_input.write_bytes(invalid_isa)
            for mode in MODES:
                reject(mode, "vendor/ASE profile is not supported", input_elf=invalid_input)

            # Check an unusable input as well as the candidate section layout.
            bad_input = build / "readonly-entry.elf"
            run([args.lld, "-static", "-T", build / "readonly-entry.ld", "-o", bad_input,
                 *(build / obj for obj in objects)])
            for mode in MODES:
                reject(mode, entry_error, input_elf=bad_input, layout="readonly-entry.ld")

            # Deliver the named-input, same-path results to the guest runner.
            qualify_named_inputs(args, build, out, profile, results[name])

            reject("section", "do not reproduce", objects=["start.o", "updated.o"],
                   original="updated.o")
            reject("section", "missing defined global function", function="absent")
            reject("section", "separate section", sectionLayout="base.ld")
            reject("section", "separate section", section=".wrong")
            reject("section", "changed the program entry", sectionLayout="wrong-entry.ld")
            reject("inplace", "missing defined global function", replacement="start.o")
            reject("inplace", "exactly one entry", original="absent.o")
            opposite = replace(profile, little=not profile.little)
            compile_arithmetic_object(args, build, opposite, "wrong-endian.o", calculator(profile, True))
            reject("section", "ELF linker exited", replacement="wrong-endian.o")
            compile_arithmetic_object(args, build, profile, "adjacent.o", CALC.replace("ADJUST", "7").replace("12345", "9"))
            reject("inplace", "other loaded bytes", replacement="adjacent.o")
            encode(manifest_path, manifest)
            run([args.neverd, "patch", original, *options, "--from-object=updated.o", "-o", sentinel],
                error="cannot be combined")
        check_mixed_float_abis(args, Path(temp), root)
    encode(root / "products.json", {"version": version, "profiles": results, "objects": object_hashes})
    print(f"Relinked {len(results)} profiles in both modes")


def verify_elf_profile(image, profile, file_type):
    # This runs on the execution host. Native producers only need Python's stdlib.
    abi = profile.abi
    flags = image["e_flags"]
    if profile.historical_isa:
        arch = profile.historical_isa - 1
        level, revision = profile.historical_isa, 0
    else:
        arch = {1: 5, 2: 7, 3: 7, 5: 7, 6: 9}[profile.release] + (profile.release_isa_bits == 64)
        level, revision = profile.release_isa_bits, profile.release
    extension = {"classic": 0, "micromips": 0x02000000, "mips16": 0x04000000}[profile.isa]
    if (image["e_type"] != file_type or image["e_machine"] != "EM_MIPS"
            or image.little_endian != profile.little or image.elfclass != (64 if abi == "n64" else 32)
            or flags >> 28 != arch or flags & 0x06000000 != extension
            or bool(flags & 0x20) != (abi == "n32")
            or flags & 0xf000 != (0x1000 if abi == "o32" else 0)):
        raise ValueError(f"{profile.name}: unexpected ELF header or ABI flags {flags:#x}")
    abi_record = image.get_section_by_name(".MIPS.abiflags")
    if abi_record is None or abi_record.data()[2:4] != bytes((level, revision)):
        raise ValueError(f"{profile.name}: expected ISA level {level}, revision {revision} in .MIPS.abiflags")
    if abi == "o32" and (profile.historical_isa >= 3 or profile.gp64_o32):
        if not flags & 0x100 or abi_record.data()[4] != 2:
            raise ValueError(f"{profile.name}: expected the fork's wide-GPR o32 ELF flags")


def inspect_elf(image, profile, *, require_pic_marker=False):
    verify_elf_profile(image, profile, "ET_EXEC")
    if bool(image["e_entry"] & 1) != profile.compressed_entry:
        raise ValueError(f"{profile.name}: incorrect program entry ISA at {image['e_entry']:#x}")

    symtab = image.get_section_by_name(".symtab")
    (index, symbol), = ((i, s) for i, s in enumerate(symtab.iter_symbols()) if s.name == "calc")
    # MIPS16 uses the whole high nibble. Other functions can carry STO_MIPS_PIC
    # alongside their encoding, for example after a relocatable link.
    other_offset = 5 if image.elfclass == 64 else 13
    table = symtab.data()
    other = table[index * symtab["sh_entsize"] + other_offset]
    if other & 0xf0 == 0xf0:
        isa = "mips16"
    elif other & 0x80:
        isa = "micromips"
    else:
        isa = "classic"
    if isa != profile.isa:
        raise ValueError(f"{profile.name}: incorrect calc ISA marker {other:#x}")
    if require_pic_marker and not other & 0x20:
        raise ValueError(f"{profile.name}: missing STO_MIPS_PIC on the partially linked calc")
    section = image.get_section(symbol["st_shndx"])
    va, size = symbol["st_value"], symbol["st_size"]
    if profile.entry_alias:
        (index, alias), = ((i, s) for i, s in enumerate(symtab.iter_symbols()) if s.name == "_start")
        if (alias["st_info"]["bind"] != "STB_GLOBAL" or alias["st_info"]["type"] != "STT_FUNC"
                or any(alias[key] != symbol[key] for key in ("st_value", "st_size", "st_shndx"))
                or table[index * symtab["sh_entsize"] + other_offset] != other
                or image["e_entry"] & ~1 != va):
            raise ValueError(f"{profile.name}: ELF entry and _start must both designate calc")
    if (symbol["st_info"]["type"] != "STT_FUNC" or not section["sh_flags"] & 4 or size <= 0
            or not section["sh_addr"] <= va <= section["sh_addr"] + section["sh_size"] - size
            or (profile.high_address and va < 1 << 32)):
        raise ValueError(f"{profile.name}: invalid calc range {va:#x}+{size:#x} in {section.name}")
    if profile.linkage:
        inspect_linkage(image, profile, symbol, section)
    return dict(va=va, offset=section["sh_offset"] + va - section["sh_addr"], size=size, section=section.name)


def verify_dwarf(image, version, placement, line):
    from elftools.dwarf.descriptions import describe_form_class

    dwarf = image.get_dwarf_info()
    unit, = dwarf.iter_CUs()
    if unit["version"] != version or unit["address_size"] != image.elfclass // 8:
        raise ValueError("Unexpected MIPS DWARF version or address size")
    subprogram, = (die for die in unit.iter_DIEs() if die.tag == "DW_TAG_subprogram")
    attrs = subprogram.attributes
    start = attrs["DW_AT_low_pc"].value
    high = attrs["DW_AT_high_pc"]
    end = high.value + (start if describe_form_class(high.form) == "constant" else 0)
    if (attrs["DW_AT_name"].value != b"calc" or attrs["DW_AT_decl_line"].value != line
            or (start, end) != (placement["va"], placement["va"] + placement["size"])):
        raise ValueError(f"DWARF subprogram does not match calc at {placement['va']:#x}")
    program = dwarf.line_program_for_CU(unit)
    states = [e.state for e in program.get_entries() if e.state is not None]
    if (len(states) < 2 or states[0].address != start or not states[-1].end_sequence
            or states[-1].address != end):
        raise ValueError("DWARF line table does not span calc")
    for state in states[:-1]:
        # DWARF 5 numbers files from zero. Earlier line tables start at one.
        index = state.file if version == 5 else state.file - 1
        if (state.end_sequence or state.line != line or not start <= state.address < end
                or not 0 <= index < len(program.header.file_entry)
                or program.header.file_entry[index].name != b"calc.c"):
            raise ValueError(f"DWARF line table does not identify calc.c line {line}")


def execute_image(path, image, profile, expected):
    needs_fpu = profile.release < 6 and profile.linkage and not profile.soft_float
    if profile.isa == "micromips" and (profile.abi != "o32" or profile.gp64_o32 or needs_fpu):
        from mips_elf_runtime import run_micromips64
        pointers = None
        if profile.tls_blocks:
            blocks, = image.get_section_by_name(".symtab").get_symbol_by_name("tls_blocks")
            first = blocks["st_value"] + 0x7000
            pointers = (first, first + 768) * 2
        run_micromips64(image, profile.release, profile.exit_nr, expected,
                        instruction_limit=2048 if profile.linkage else 256,
                        set_thread_area=SET_THREAD_AREA[profile.abi] if profile.tls_model else None,
                        thread_pointers=pointers)
        return
    arch = {"o32": "mips", "n32": "mipsn32", "n64": "mips64"}[profile.abi]
    qemu = f"qemu-{arch}{'el' if profile.little else ''}"
    cpu = None
    if profile.linkage == "dsp":
        cpu = "74Kf" if profile.abi == "o32" else "mips64dspr2"
    elif profile.linkage == "msa":
        cpu = "P5600" if profile.abi == "o32" else "Loongson-3A4000"
    elif profile.release == 6:
        cpu = "mips32r6-generic" if profile.abi == "o32" else "I6400"
    elif profile.isa == "micromips":
        cpu = "M14Kc"
    elif profile.isa == "mips16":
        cpu = "74Kf"
    path.chmod(0o755)
    run([qemu, *(["-cpu", cpu] if cpu else []), path], expected=expected)


def verify_partial_object(path, profile):
    from elftools.elf.elffile import ELFFile

    with path.open("rb") as stream:
        image = ELFFile(stream)
        verify_elf_profile(image, profile, "ET_REL")
        symbols = image.get_section_by_name(".symtab")
        if symbols is None:
            raise ValueError(f"{path}: missing combined object symbol table")
        for name, section in (("_start", ".text.start"), ("calc", ".text.calc")):
            found = symbols.get_symbol_by_name(name) or []
            if (len(found) != 1 or found[0]["st_info"]["type"] != "STT_FUNC"
                    or found[0]["st_info"]["bind"] != "STB_GLOBAL" or found[0]["st_size"] == 0
                    or found[0]["st_shndx"] != image.get_section_index(section)):
                raise ValueError(f"{path}: expected a definition of {name} in {section}")
        relocs = image.get_section_by_name(".rel.text.start" if profile.abi == "o32" else ".rela.text.start")
        if relocs is None or relocs["sh_info"] != image.get_section_index(".text.start"):
            raise ValueError(f"{path}: missing startup relocations")
        targets = image.get_section(relocs["sh_link"])
        startup = image.get_section(relocs["sh_info"]).data()
        order = "little" if image.little_endian else "big"
        calls = {}
        for reloc in relocs.iter_relocations():
            offset = reloc["r_offset"]
            instruction = int.from_bytes(startup[offset:offset + 4], order)
            operations = (reloc["r_info_type"], reloc.entry.get("r_info_type2", 0),
                          reloc.entry.get("r_info_type3", 0))
            calls[offset] = (operations, targets.get_symbol(reloc["r_info_sym"]).name,
                             reloc.entry.get("r_addend", 0), instruction)
        # Each startup call is a JAL to calc with R_MIPS_26 and a zero addend.
        if (relocs.num_relocations() != 3 or len(calls) != 3
                or set(calls.values()) != {((4, 0, 0), "calc", 0, 0x0c000000)}):
            raise ValueError(f"{path}: unexpected startup relocations {calls}")
        for name in ("group_choice", "group_increment"):
            found = symbols.get_symbol_by_name(name) or []
            if (len(found) != 1 or found[0]["st_info"]["bind"] != "STB_WEAK"
                    or not isinstance(found[0]["st_shndx"], int)):
                raise ValueError(f"{path}: expected one defined weak {name}")
        groups = [section for section in image.iter_sections() if section["sh_type"] == "SHT_GROUP"]
        if len(groups) != 1:
            raise ValueError(f"{path}: expected one group for group_increment")
        group = groups[0]
        signature = image.get_section(group["sh_link"]).get_symbol(group["sh_info"])
        data = group.data()
        words = [int.from_bytes(data[i:i + 4], order) for i in range(0, len(data), 4)]
        if signature.name != "group_increment" or words[:1] != [1] or signature["st_shndx"] not in words[1:]:
            raise ValueError(f"{path}: incomplete group_increment COMDAT group")
    return digest(path)


def execute(args):
    from elftools.elf.elffile import ELFFile

    root = args.root.resolve()
    manifest = json.loads((root / "products.json").read_text())
    if "version" in manifest:
        version = manifest["version"]
        if version not in range(2, PRODUCT_VERSION + 1):
            raise ValueError("Unknown MIPS relink products version")
        hashes = manifest["profiles"]
    else:
        # Archived original-format manifests contain the six classic R2 cases.
        version = 1
        hashes = manifest
    cases = c_call_upgrade_profiles() if manifest.get("suite") == "c-call-upgrade" else list(profiles(version))
    if set(hashes) != {p.name for p in cases}:
        raise ValueError("MIPS relink products do not match the expected profile set")
    if args.partial_link:
        partial = {}
        for profile in cases:
            folder = root / profile.name
            objects = {name: verify_partial_object(folder / name, profile) for name in ("calc.o", "updated.o")}
            if objects["calc.o"] == objects["updated.o"]:
                raise ValueError(f"{profile.name}: expected distinct original and updated objects")
            partial[profile.name] = objects
        encode(root / "partial-inputs.json", partial)
    if version >= 4:
        object_hashes = manifest["objects"]
        if set(object_hashes) != {p.name for p in cases if p.linkage}:
            raise ValueError("MIPS relink object receipts do not match the expected linkage profiles")
    results, placements = {}, {}
    for profile in cases:
        row, ranges = {}, {}
        original, updated = (81, 29) if profile.shrinking else (25, 37)
        if profile.c_call:
            original, updated = 43, 89
        elif profile.linkage:
            original, updated = 41, 73
            verify_linkage_objects(root / profile.name, profile, object_hashes[profile.name], digest)
        expected_results = {"original": original, "section": updated, "inplace": updated}
        if version >= 3 and not (profile.c_call or profile.linkage or profile.dwarf_version or profile.entry_alias):
            # The larger body returns 61, 56 and 44 for the three startup calls.
            expected_results["section-grown"] = 161
        second_updates = version >= 10 and profile == Profile(profile.abi, profile.little)
        if second_updates:
            # The three calls to 2*x + 11 return 11, 13 and 25.
            expected_results.update({f"after-{first}-{mode}": 49 for first in MODES for mode in MODES})
        if hashes[profile.name].keys() != expected_results.keys():
            raise ValueError(f"{profile.name}: unexpected set of relink products")
        for case, expected in expected_results.items():
            path = root / profile.name / f"{case}.elf"
            if digest(path) != hashes[profile.name][case]:
                raise ValueError(f"{path}: SHA-256 mismatch")
            with path.open("rb") as source:
                image = ELFFile(source)
                ranges[case] = inspect_elf(image, profile, require_pic_marker=args.partial_link == "pic")
                if profile.dwarf_version:
                    verify_dwarf(image, profile.dwarf_version, ranges[case], 14 if case == "original" else 29)
                execute_image(path, image, profile, expected)
            row[case] = expected
        old, inplace = ranges["original"], ranges["inplace"]
        sizes = ranges["section"]["size"], inplace["size"]
        sizes_match = (max(sizes) < old["size"] if profile.shrinking
                       else sizes == (old["size"], old["size"]))
        if (inplace["va"] != old["va"] or inplace["offset"] != old["offset"]
                or not sizes_match):
            raise ValueError(f"{profile.name}: incorrect function placement {ranges}")
        for case in (case for case in ranges if case.startswith("section")):
            section = ranges[case]
            if (section["section"] != ".calc.update"
                    or not (section["va"] >= old["va"] + old["size"]
                            or old["va"] >= section["va"] + section["size"])
                    or (case == "section-grown" and section["size"] <= old["size"])):
                raise ValueError(f"{profile.name}: incorrect {case} placement {section} for {old}")
        if second_updates:
            for first in MODES:
                moved = old if first == "section" else ranges["section"]
                if ranges[f"after-{first}-inplace"] != ranges[first] or ranges[f"after-{first}-section"] != moved:
                    raise ValueError(f"{profile.name}: incorrect second placement after {first}")
        results[profile.name], placements[profile.name] = row, ranges
        if profile.name == "micromips-r2-n64-be":
            from mips_elf_runtime import check_non_executable_entry
            check_non_executable_entry(root / profile.name / "section.elf",
                                       profile.release, profile.exit_nr, updated)
    encode(root / "execution.json", results)
    encode(root / "placements.json", placements)
    print(f"Executed {sum(map(len, results.values()))} complete ELF files across {len(results)} profiles")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    patch = commands.add_parser("qualify")
    patch.add_argument("--root", required=True, type=Path)
    for tool in ("neverd", "mc", "llc", "lld"):
        patch.add_argument("--" + tool, required=True)
    patch.add_argument("--clang", help="also compile the C aggregate and variadic calls with this fork Clang")
    upgrade = commands.add_parser("upgrade")
    upgrade.add_argument("--root", required=True, type=Path)
    upgrade.add_argument("--neverd", required=True)
    upgrade.add_argument("--original-tools", required=True, type=Path)
    upgrade.add_argument("--tools", required=True, type=Path)
    upgrade.add_argument("--c-calls", action="store_true",
                         help="relink r15 C caller objects with a callee compiled by the current fork")
    upgrade.add_argument("--archive-tool", type=Path,
                         help="use fork llvm-ar to check lazy member selection during a partial link")
    runtime = commands.add_parser("execute")
    runtime.add_argument("--root", required=True, type=Path)
    for command in (upgrade, runtime):
        command.add_argument("--partial-link", nargs="?", const="static", choices=("static", "pic"),
                             help="combine the startup and weak definitions with a static or PIC callee")
    args = parser.parse_args()
    if args.command == "upgrade" and args.archive_tool and not args.partial_link:
        parser.error("--archive-tool requires --partial-link")
    if args.command == "upgrade" and args.c_calls and args.partial_link:
        parser.error("--c-calls and --partial-link select different input sets")
    {"qualify": qualify, "upgrade": qualify_upgrade, "execute": execute}[args.command](args)
