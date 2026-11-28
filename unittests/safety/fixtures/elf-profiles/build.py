#!/usr/bin/env python3
"""Rebuild linked ELF scan controls with Clang, LLD and GNU PowerPC ld."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def profiles():
    for endian in ("", "el"):
        mips = "mips" + endian
        wide = "mips64" + endian
        for name, target, cpu, abi in (
            (mips, mips, "mips32r2", "o32"),
            ("mips64o32" + endian, mips, "mips3", "o32"),
            ("mips64n32" + endian, wide, "mips64r2", "n32"),
            (wide, wide, "mips64r2", "n64"),
        ):
            yield name, target, ["-march=" + cpu, "-mabi=" + abi, "-mno-abicalls"], None
    for suffix in ("", "le"):
        ppc = "powerpc" + suffix
        wide = "powerpc64" + suffix
        yield ppc, ppc, ["-mcpu=ppc"], None
        yield wide, wide, ["-mcpu=power8", "-mabi=elfv2"], None
        # GNU ld retains the ELFv1 function descriptor ABI in the linked image.
        yield wide + "-elfv1", wide, ["-mcpu=power8", "-mabi=elfv1"], (
            "elf64lppc" if suffix else "elf64ppc"
        )
    for name, abi in (("riscv32", "ilp32"), ("riscv64", "lp64")):
        yield name, name, ["-march=rv" + name[-2:] + "i", "-mabi=" + abi], None
        yield name + "-" + abi + "d", name, ["-march=rv" + name[-2:] + "gc",
                                              "-mabi=" + abi + "d"], None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld.lld")
    parser.add_argument("--ppc64-ld", default="powerpc64le-linux-gnu-ld")
    parser.add_argument("--output", type=Path, default=Path(__file__).parent / "binaries")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = Path(__file__).resolve().parent
    imports = args.output / "imports"
    imports.mkdir(exist_ok=True)
    linker_flags = ["--build-id=none", "-z", "max-page-size=4096"]
    with tempfile.TemporaryDirectory() as temporary:
        obj = Path(temporary) / "frame.o"
        provider = Path(temporary) / "liblabels.so"
        for name, target, flags, emulation in profiles():
            compiler = [args.clang, "--target=" + target + "-unknown-linux-gnu",
                        "-O1", "-g0", "-ffreestanding", "-fno-stack-protector"]
            static = [*compiler, *flags, "-fno-pic", "-fno-pie", "-c"]
            frame = "hard_float.c" if name.endswith(("ilp32d", "lp64d")) else "frame.c"
            subprocess.run([*static, sources / frame, "-o", obj], check=True)
            linker = [args.ppc64_ld, "-m", emulation] if emulation else [args.lld]
            image = args.output / name
            subprocess.run([*linker, *linker_flags, "-static", "-e", "first",
                            obj, "-o", image], check=True)
            image.chmod(0o644)
            if emulation:
                continue

            # The imported label function comes from a separately linked
            # provider. Only the consumer ELF is needed for static inspection.
            pic_flags = [flag for flag in flags if flag != "-mno-abicalls"]
            subprocess.run([*compiler, *pic_flags, "-fpic", "-c", sources / "label.c",
                            "-o", obj], check=True)
            subprocess.run([args.lld, *linker_flags, "-shared", "-soname",
                            provider.name, obj, "-o", provider], check=True)
            subprocess.run([*static, sources / "read_label.c", "-o", obj], check=True)
            image = imports / name
            subprocess.run([args.lld, *linker_flags, "-e", "first", obj, provider,
                            "-o", image], check=True)
            image.chmod(0o644)


if __name__ == "__main__":
    main()
