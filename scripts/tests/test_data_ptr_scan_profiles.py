"""Cross-compile data-reference controls and inspect the linked target images."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SOURCE = """
#ifdef __APPLE__
#define DATA "__DATA,__data"
#else
#define DATA ".data"
#endif
#define STORED __attribute__((used, section(DATA)))
static volatile int scalar STORED = 17;
static const int marker = 23;
static const volatile int *volatile refs[] STORED = { &scalar, &scalar, &marker };
int entry(void) { return scalar + scalar; }
"""


class DataPointerProfiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        listing = subprocess.check_output([cls.clang, "--print-targets"], text=True)
        cls.backends = {line.split()[0] for line in listing.splitlines() if " - " in line}

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="neverd-data-profiles-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.source = self.root / "input.c"
        self.source.write_text(SOURCE)
        self.obj = self.root / "input.o"
        self.image = self.root / "image"

    def tool(self, name):
        for directory in (self.clang.parent, None):
            result = shutil.which(name, path=str(directory) if directory else None)
            if result:
                return result
        self.skipTest(f"{name} unavailable")

    def compile(self, triple, *flags):
        arch = triple.split("-")[0]
        backend = {"arm64": "aarch64", "powerpc": "ppc32", "powerpcle": "ppc32le",
                   "powerpc64": "ppc64", "powerpc64le": "ppc64le",
                   "x86_64": "x86-64"}.get(arch, arch)
        if backend not in self.backends:
            self.skipTest(f"Clang backend {backend} unavailable")
        subprocess.run([self.clang, "--target=" + triple, "-O1", "-ffreestanding",
                        "-fno-stack-protector", *flags, "-c", self.source,
                        "-o", self.obj], check=True)

    def link_elf(self, *flags, linker="ld.lld"):
        subprocess.run([self.tool(linker), *flags, "-e", "entry", self.obj,
                        "-o", self.image], check=True)

    def report(self, image=None):
        result = subprocess.run([self.neverd, "scan", "--data-ptr", image or self.image],
                                check=True, capture_output=True, text=True, timeout=30)
        return json.loads(result.stdout)

    def counts(self, fmt, pointers=3, references=2):
        report = self.report()
        self.assertEqual(report["format"], fmt)
        self.assertEqual(report["section"], "__data" if fmt == "Mach-O" else ".data")
        self.assertEqual(report["data_pointers"], pointers)
        self.assertEqual(report["code_references"], references)
        self.assertGreater(report["functions_scanned"], 0)
        self.assertEqual(report["incomplete_functions"], 0)

    def test_windows(self):
        for arch in ("x86_64", "aarch64"):
            with self.subTest(arch=arch):
                self.compile(arch + "-pc-windows-msvc")
                subprocess.run([self.tool("lld-link"), "/entry:entry", "/nodefaultlib",
                                "/dynamicbase", "/subsystem:console",
                                "/out:" + str(self.image), self.obj], check=True)
                self.counts("PE")

    def test_apple(self):
        for arch, platform, version in (("arm64", "macos", "11.0"),
                                        ("x86_64", "macos", "11.0"),
                                        ("arm64", "ios", "15.0")):
            with self.subTest(arch=arch, platform=platform):
                self.compile(f"{arch}-apple-{platform}{version}", "-fPIC")
                self.assertEqual(self.report(self.obj)["data_pointers"], 3)
                subprocess.run([self.tool("ld64.lld"), "-arch", arch, "-platform_version",
                                platform, version, version, "-e", "_entry",
                                self.obj, "-o", self.image], check=True)
                # The linker maps marker in executable __TEXT,__const.
                self.counts("Mach-O", pointers=2)
                if platform == "macos":
                    subprocess.run([self.tool("ld64.lld"), "-arch", arch,
                                    "-platform_version", platform, "13.0", "13.0",
                                    "-fixup_chains", "-e", "_entry", self.obj,
                                    "-o", self.image], check=True)
                    self.counts("Mach-O", pointers=2)

    def test_elf_pie(self):
        profiles = [
            ("x86_64-linux-gnu", []), ("aarch64-linux-gnu", []),
            ("aarch64-linux-android29", []), ("aarch64-unknown-freebsd14", []),
            ("powerpc64-linux-gnu", ["-mabi=elfv2"]), ("powerpc64le-linux-gnu", []),
        ]
        for bits, abi in ((32, "ilp32"), (64, "lp64")):
            for isa, suffix in (("i", ""), ("gc", "d")):
                profiles.append((f"riscv{bits}-linux-gnu",
                                 [f"-march=rv{bits}{isa}", "-mabi=" + abi + suffix]))
        for triple, flags in profiles:
            with self.subTest(triple=triple, flags=flags):
                self.compile(triple, "-fPIC", *flags)
                self.link_elf("-pie", "--no-dynamic-linker", "-Bsymbolic")
                self.counts("ELF")

    def test_mips_abis_and_byte_orders(self):
        for endian in ("", "el"):
            for arch, abi in (("mips", "o32"), ("mips64", "n32"), ("mips64", "n64")):
                triple = arch + endian + "-linux-gnu"
                with self.subTest(triple=triple, abi=abi):
                    self.compile(triple, "-mabi=" + abi, "-fno-pic", "-mno-abicalls")
                    self.link_elf("--emit-relocs")
                    self.counts("ELF")
                    self.compile(triple, "-mabi=" + abi, "-fPIC")
                    self.link_elf("-pie", "--no-dynamic-linker", "-Bsymbolic")
                    self.assertEqual(self.report()["data_pointers"], 3)

    def test_powerpc32_retained_relocations(self):
        for arch in ("powerpc", "powerpcle"):
            with self.subTest(arch=arch):
                self.compile(arch + "-linux-gnu", "-fno-pic")
                self.link_elf("--emit-relocs")
                self.counts("ELF")
                table = self.root / "data"
                objcopy = self.tool("llvm-objcopy")
                subprocess.run([objcopy, "--dump-section", ".data=" + str(table),
                                self.image, self.root / "copy"], check=True)
                data = bytearray(table.read_bytes())
                # Keep a mapped target but invalidate the first pointer's
                # recorded symbol-plus-addend. The second points to the same int.
                order = "little" if arch.endswith("le") else "big"
                self.assertEqual(data[4:8], data[8:12])
                data[4:8] = (int.from_bytes(data[4:8], order) + 1).to_bytes(4, order)
                table.write_bytes(data)
                stale = self.root / "stale"
                subprocess.run([objcopy, "--update-section", ".data=" + str(table),
                                self.image, stale], check=True)
                self.assertEqual(self.report(stale)["data_pointers"], 2)
                self.link_elf()
                self.counts("ELF", pointers=0)

    def test_stripped_elf_entry(self):
        for triple in ("riscv64-linux-gnu", "mips-linux-gnu", "powerpc64le-linux-gnu"):
            with self.subTest(triple=triple):
                self.compile(triple, "-fPIC")
                self.link_elf("-pie", "--no-dynamic-linker", "--strip-all")
                report = self.report()
                self.assertGreaterEqual(report["functions_scanned"], 1)
                self.assertEqual(report["data_pointers"], 3)
                if triple.startswith("riscv"):
                    self.assertEqual(report["code_references"], 2)

    def test_powerpc64_elfv1(self):
        linker = next((name for name in ("powerpc64-linux-gnu-ld",
                                         "powerpc64le-linux-gnu-ld")
                       if shutil.which(name)), None)
        if not linker:
            self.skipTest("GNU PowerPC64 ld unavailable")
        for arch, emulation in (("powerpc64", "elf64ppc"), ("powerpc64le", "elf64lppc")):
            with self.subTest(arch=arch):
                self.compile(arch + "-linux-gnu", "-mabi=elfv1", "-fPIC")
                self.link_elf("-m", emulation, "-pie", "-Bsymbolic", linker=linker)
                self.counts("ELF")

    def test_unmapped_mips_relative_relocation(self):
        self.compile("mips64el-linux-gnu", "-mabi=n64", "-fPIC")
        self.link_elf("-pie", "--no-dynamic-linker")
        table = self.root / "relocations"
        copy = self.tool("llvm-objcopy")
        subprocess.run([copy, "--dump-section", ".rel.dyn=" + str(table),
                        self.image, self.root / "copy"], check=True)
        records = table.read_bytes()
        self.assertGreaterEqual(len(records), 16)
        image = bytearray(self.image.read_bytes())
        self.assertEqual(image.count(records), 1)
        offset = image.index(records)
        image[offset:offset + 8] = bytes([255]) * 8
        broken = self.root / "broken"
        broken.write_bytes(image)
        result = subprocess.run([self.neverd, "scan", "--data-ptr", broken],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unmapped MIPS relative relocation", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    DataPointerProfiles.neverd = options.neverd.resolve(strict=True)
    DataPointerProfiles.clang = options.clang.resolve(strict=True)
    unittest.main(argv=[__file__, *remaining])
