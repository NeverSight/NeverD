#!/usr/bin/env python3
"""Rebuild GNU MIPS n32 ownership fixtures with Debian cross glibc and binutils."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    source = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--sysroot", type=Path, default=Path("/usr"))
    parser.add_argument("--output", type=Path, default=source / "binaries" / "ownership")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        obj = Path(temporary) / "ownership.o"
        for little in (False, True):
            target = "mips64el" if little else "mips64"
            toolchain = target + "-linux-gnuabi64"
            libc = args.sysroot / toolchain / "lib32" / "libc.so.6"
            profile = "mips64n32el" if little else "mips64n32"
            for variant in ("released", "unreleased", "unrelated-store"):
                image = args.output.resolve() / (profile + "-" + variant)
                subprocess.run([
                    args.clang, "--target=" + target + "-unknown-linux-gnu", "-O1", "-g",
                    "-fdebug-compilation-dir=.", "-ffreestanding", "-fno-builtin",
                    "-fPIC", "-mabicalls", "-march=mips64r2", "-mabi=n32",
                    "-DRELEASE_ALLOCATION=" + str(int(variant != "unreleased")),
                    "-DRECORD_COMPLETION=" + str(int(variant == "unrelated-store")),
                    "-c", "ownership.c", "-o", obj,
                ], cwd=source, check=True)
                subprocess.run([
                    toolchain + "-ld", "-m", "elf32" + ("l" if little else "b") + "tsmipn32",
                    "--build-id=none", "-z", "max-page-size=4096", "-e", "main", obj,
                    "--no-as-needed", "--allow-shlib-undefined", libc, "-o", image,
                ], check=True)
                image.chmod(0o644)


if __name__ == "__main__":
    main()
