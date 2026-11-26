#!/usr/bin/env python3
"""Execute compiled Power ISA 3.1 fixed-point and RISC-V Zba/Zbs fixtures in QEMU."""

import argparse
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory

from elftools.elf.elffile import ELFFile

from image_storage import llvmc_image_storage
from roundtrip import compile_elf, compiler_flags

HERE = Path(__file__).resolve().parent
CPUS = {"powerpc64": ("ppc64", "power10"), "powerpc64le": ("ppc64le", "power10"),
        "riscv32": ("riscv32", "rv32,zba=true,zbs=true"),
        "riscv64": ("riscv64", "rv64,zba=true,zbs=true")}


def image_base_option(linker):
    version = subprocess.check_output([linker, "--version"], text=True)
    return "-Ttext-segment" if "GNU ld" in version else "--image-base"


def project(args, image, function, route):
    source = image.with_name(function + (".ll" if route == "llvm" else ".c"))
    cmd = ["lift"] if route == "llvm" else ["decompile", "--func", function]
    if route == "llvmc":
        cmd.append("--llvm")
    subprocess.run([args.neverd, *cmd, image, "-o", source], check=True)
    return source


def check(args, work, profile):
    rv = profile.startswith("riscv")
    bits = profile[-2:]
    flags = [f"-march=rv{bits}i_zba_zbs"] if rv else ["-mcpu=pwr10"]
    asm = HERE / ("riscv-bitmanip.S" if rv else "ppc64-prefixed.S")
    qemu, cpu = CPUS[profile]
    runner = [args.qemu_dir / f"qemu-{qemu}", "-cpu", cpu]
    placement = image_base_option(args.ld)
    # The RISC-V source ELF has IALIGN=32. Build the oracle with base I
    # so its return PCs are word aligned and its bit operations are independent.
    oracle_flags = compiler_flags(profile) + ([f"-march=rv{bits}i"] if rv else [])
    objects = []
    for name in ("extended-scalar-check.c", "extended-scalar-start.S"):
        obj = work / (name + ".o")
        subprocess.run([args.clang, *oracle_flags, "-O2", "-c", HERE / name,
                        "-o", obj], check=True)
        objects.append(obj)

    original = work / "original.elf"
    compile_elf(args, work, profile, asm, original, "eval", extra_flags=flags)
    native = work / "native.elf"
    compile_elf(args, work, profile, asm, native, "_start", callees=objects, extra_flags=flags)
    subprocess.run([*runner, native], check=True)
    print(f"{profile} original passed", flush=True)
    with original.open("rb") as stream:
        elf = ELFFile(stream)
        original_addresses = [elf["e_entry"]]
        if not rv:
            original_addresses.append(elf.get_section_by_name(".data")["sh_addr"])

    for route in ("highc", "llvmc", "llvm"):
        out = project(args, original, "eval", route)
        storage = llvmc_image_storage(out, original, ".data") if not rv and route == "llvmc" else []
        for opt, base in (("-O0", "0x210000"), ("-O2", "0x610000")):
            rebuilt = work / "rebuilt.elf"
            compile_elf(args, work, profile, out, rebuilt, "_start", opt, [*storage, *objects],
                        [*flags, "-fno-strict-aliasing", f"-Wl,{placement}={base}"])
            with rebuilt.open("rb") as stream:
                if any(s["p_type"] == "PT_LOAD" and
                       any(s["p_vaddr"] <= a < s["p_vaddr"] + s["p_memsz"]
                           for a in original_addresses) for s in ELFFile(stream).iter_segments()):
                    raise AssertionError("original code or data is still mapped")
            subprocess.run([*runner, rebuilt], check=True)
            print(f"{profile} {route} {opt} passed", flush=True)


def check_power10_linkage(args, work, profile, loader):
    qemu, cpu = CPUS[profile]
    interpreter = f"ld-musl-{profile}.so.1"
    sysroot = work / "sysroot"
    (sysroot / "lib").mkdir(parents=True, exist_ok=True)
    shutil.copy2(loader, sysroot / "lib" / interpreter)
    runner = [args.qemu_dir / f"qemu-{qemu}", "-cpu", cpu, "-L", sysroot]
    cc = [args.clang, *compiler_flags(profile), "-fPIC", "-nostdlib", f"-fuse-ld={args.ld}"]
    provider = work / "provider.so"
    subprocess.run([*cc, "-shared", "-O2", "-DPROVIDER", HERE / "powerpc-plt.c",
                    "-Wl,-soname,provider.so", "-o", provider], check=True)
    original = HERE / "ppc64-elfv2-notoc.S"
    program = work / "notoc.elf"
    link = ["-no-pie", "-Wl,--power10-stubs", f"-Wl,--dynamic-linker=/lib/{interpreter}",
            "-Wl,-rpath,$ORIGIN"]
    subprocess.run([*cc, *link, "-Wl,-e,notoc_cross", original, provider, "-o", program],
                   check=True)
    placement = image_base_option(args.ld)

    def execute(source, opt, base):
        output = work / "notoc-runtime.elf"
        subprocess.run([*cc, *link, opt, f"-Wl,{placement}={base}", "-Wl,-e,_start",
                        "-DTEST_PPC64_NOTOC", source, HERE / "extended-scalar-check.c",
                        HERE / "extended-scalar-start.S", provider, "-o", output], check=True)
        subprocess.run([*runner, output], check=True)
        return output

    native = execute(original, "-O2", "0x10000000")
    # Require the actual notoc call to enter a prefixed load of r12. An older
    # linker that produced a POWER8 stub would exercise different code.
    with native.open("rb") as stream:
        elf = ELFFile(stream)
        text = elf.get_section_by_name(".text")
        order = "little" if elf.little_endian else "big"
        symbol, = elf.get_section_by_name(".symtab").get_symbol_by_name("notoc_cross")
        relocs = elf.get_section_by_name(".rela.plt")
        symbols = elf.get_section(relocs["sh_link"])
        slot, = (r["r_offset"] for r in relocs.iter_relocations()
                 if symbols.get_symbol(r["r_info_sym"]).name == "remote_apply")
        start = symbol["st_value"] - text["sh_addr"]
        code = text.data()
        for off in range(start, start + symbol["st_size"], 4):
            word = int.from_bytes(code[off:off + 4], order)
            if word & 0xfc000003 != 0x48000001:
                continue
            delta = ((word & 0x03fffffc) ^ 0x02000000) - 0x02000000
            stub = text["sh_addr"] + off + delta
            stream.seek(next(elf.address_offsets(stub, 8)))
            prefix = int.from_bytes(stream.read(4), order)
            suffix = int.from_bytes(stream.read(4), order)
            disp = ((prefix & 0x3ffff) << 16) | (suffix & 0xffff)
            target = stub + ((disp ^ (1 << 33)) - (1 << 33))
            if prefix & 0xfffc0000 == 0x04100000 and suffix >> 16 == 0xe580 and target == slot:
                break
        else:
            raise AssertionError("notoc_cross did not call a PC-relative PLT stub")
    print(f"{profile} POWER10 PLT original passed", flush=True)
    for route in ("highc", "llvmc", "llvm"):
        out = project(args, program, "notoc_cross", route)
        for opt, base in (("-O0", "0x210000"), ("-O2", "0x610000")):
            execute(out, opt, base)
            print(f"{profile} POWER10 PLT {route} {opt} passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for program in ("neverd", "clang", "ld"):
        parser.add_argument(f"--{program}", required=True, type=Path)
    parser.add_argument("--qemu-dir", type=Path, default=Path("/usr/bin"))
    parser.add_argument("--ppc64-loaders", nargs=2, type=Path, metavar=("BIG", "LITTLE"),
                        help="ELFv2 musl dynamic loader for each PPC64 byte order")
    parser.add_argument("--profiles", nargs="+", choices=CPUS, default=list(CPUS))
    args = parser.parse_args()
    with TemporaryDirectory() as directory:
        for profile in args.profiles:
            work = Path(directory) / profile
            work.mkdir()
            check(args, work, profile)
            if args.ppc64_loaders and profile.startswith("powerpc"):
                loader = args.ppc64_loaders[int(profile.endswith("le"))]
                check_power10_linkage(args, work, profile, loader)


if __name__ == "__main__":
    main()
