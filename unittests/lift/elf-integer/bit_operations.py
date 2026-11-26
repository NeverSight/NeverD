#!/usr/bin/env python3
"""Check all Zbb forms and PowerPC byte-reversed transfers and trailing-zero counts after each ELF projection."""

import argparse
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory

from elftools.elf.elffile import ELFFile
from unicorn import ppc_const

from extended_scalar import image_base_option, project
from roundtrip import PROFILES, RETURN, STACK, compile_elf, compiler_flags, machine

HERE = Path(__file__).resolve().parent


def signed(x, bits):
    sign = 1 << (bits - 1)
    return (x & (sign - 1)) - (x & sign)


def rotate(x, count, bits):
    mask = (1 << bits) - 1
    x &= mask
    count %= bits
    return ((x << count) | (x >> (bits - count))) & mask


def trailing(x, bits):
    return (x & -x).bit_length() - 1 if x else bits


def riscv_oracle(bits):
    mask = (1 << bits) - 1
    values = (0, mask, 0x80000000, 0x0080000100ff007f & mask,
              0x0123456789abcdef & mask, 1 << (bits - 1), 0xff80)
    cases = []
    for x in values:
        for y in (0, 1, 31, 32, 63, 64, mask, x):
            xb = x.to_bytes(bits // 8, "little")
            sx, sy = signed(x, bits), signed(y, bits)
            result = [x & ~y, x | ~y, ~(x ^ y), min(sx, sy), min(x, y), max(sx, sy),
                      max(x, y), rotate(x, y, bits), rotate(x, -y, bits), rotate(x, 1, bits),
                      bits - x.bit_length(), trailing(x, bits), x.bit_count(),
                      signed(x, 8), signed(x, 16), x & 0xffff,
                      int.from_bytes(bytes(255 if b else 0 for b in xb), "little"),
                      int.from_bytes(xb, "big")]
            if bits == 64:
                word = x & 0xffffffff
                result.extend([signed(rotate(word, n, 32), 32) for n in (y, -y, 1)])
                result.extend([32 - word.bit_length(), trailing(word, 32), word.bit_count()])
            cases.append(f"{{{x:#x}UL,{y:#x}UL,{{{words(v & mask for v in result)}}}}}")
    count = len(result)
    return f"""
typedef unsigned long Word;
extern Word eval(Word *, Word, Word);
static const struct {{ Word x, y, expected[{count}]; }} cases[] = {{
{", ".join(cases)}
}};
int check(void) {{
  Word out[{count}];
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {{
    for (unsigned j = 0; j < {count}; ++j) out[j] = ~cases[i].expected[j];
    if (eval(out, cases[i].x, cases[i].y) != cases[i].x) return 1;
    for (unsigned j = 0; j < {count}; ++j)
      if (out[j] != cases[i].expected[j]) return 2 + j;
  }}
  return 0;
}}
"""


def words(items):
    return ", ".join(f"{x:#x}UL" for x in items)


def ppc_oracle(bits, little):
    order = "big" if little else "little"
    sizes = (2, 4, 8) if bits == 64 else (2, 4)
    cases = []
    for index in (0, 8):
        for value in (0, 0x100, 1 << (bits - 1), (1 << bits) - 1,
                      0xf123456789abcdef & ((1 << bits) - 1)):
            data = bytearray((0x83 + i * 19) % 256 for i in range(48))
            loads = [int.from_bytes(data[index:index + size], order) for size in sizes]
            for size, start in zip(sizes, (index + 8, 16, index + 24)):
                low = value % (1 << (8 * size))
                data[start:start + size] = low.to_bytes(size, order)
            for width in ([32, 64] if bits == 64 else [32]):
                count = trailing(value % (1 << width), width)
                loads.extend((count, int(count == 0)))
            cases.append(f"{{{index},{value:#x}UL,{{{words(loads)}}},{{{words(data)}}}}}")
    slots = len(sizes) + (4 if bits == 64 else 2)
    return f"""
typedef unsigned long Word;
extern Word eval(Word *, unsigned char *, Word, Word);
static const struct {{ Word index, value, results[{slots}]; unsigned char bytes[48]; }}
cases[] = {{ {", ".join(cases)} }};
int check(void) {{
  Word out[{slots}], storage[48 / sizeof(Word)];
  unsigned char *data = (unsigned char *)storage;
  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {{
    for (unsigned j = 0; j < {slots}; ++j) out[j] = ~cases[i].results[j];
    for (unsigned j = 0; j < 48; ++j) data[j] = 0x83 + j * 19;
    if (eval(out, data, cases[i].index, cases[i].value) != cases[i].index) return 1;
    for (unsigned j = 0; j < {slots}; ++j)
      if (out[j] != cases[i].results[j]) return 2 + j;
    for (unsigned j = 0; j < 48; ++j)
      if (data[j] != cases[i].bytes[j]) return 8 + j;
  }}
  return 0;
}}
"""


def execute(args, profile, path):
    if profile in ("powerpc", "powerpcle"):
        # The 64-bit model executes ISA 3.0 word instructions with MSR.SF clear.
        cpu, symbols = machine(path, profile, power8=True)
        cpu.reg_write(ppc_const.UC_PPC_REG_1, STACK + 2048)
        cpu.reg_write(ppc_const.UC_PPC_REG_LR, RETURN)
        cpu.emu_start(symbols["check"], RETURN, count=100000)
        status = cpu.reg_read(ppc_const.UC_PPC_REG_3)
        if cpu.reg_read(ppc_const.UC_PPC_REG_PC) != RETURN or status:
            raise RuntimeError(f"{profile} {path.name}: check returned {status} or timed out")
    else:
        qemu = profile.replace("powerpc", "ppc")
        cpu = "power10" if "ppc64" in qemu else "rv" + profile[-2:] + ",zbb=true"
        subprocess.run([args.qemu_dir / ("qemu-" + qemu), "-cpu", cpu, path], check=True)


def check(args, work, profile):
    rv = profile.startswith("riscv")
    bits = 64 if "64" in profile else 32
    flags = [f"-march=rv{bits}i_zbb"] if rv else []
    source = HERE / ("riscv-zbb.S" if rv else "powerpc-byte-reversed.S")
    oracle = work / "check.c"
    oracle.write_text(riscv_oracle(bits) if rv else ppc_oracle(bits, profile.endswith("le")))
    # Keep the caller's ABI independent; base I also enforces the source's IALIGN=32.
    caller_flags = compiler_flags(profile) + ([f"-march=rv{bits}i"] if rv else [])
    sources = [oracle]
    if rv or bits == 64:
        sources.append(HERE / "extended-scalar-start.S")
    objects = []
    for src in sources:
        obj = work / (src.name + ".o")
        subprocess.run([args.clang, *caller_flags, "-O2", "-c", src, "-o", obj], check=True)
        objects.append(obj)
    entry = "_start" if rv or bits == 64 else "check"
    original = work / "original.elf"
    compile_elf(args, work, profile, source, original, "eval", extra_flags=flags)
    native = work / "native.elf"
    compile_elf(args, work, profile, source, native, entry, callees=objects, extra_flags=flags)
    execute(args, profile, native)
    print(f"{profile} original passed", flush=True)
    placement = image_base_option(args.ld)
    with original.open("rb") as stream:
        old_entry = ELFFile(stream)["e_entry"]
    for route in ("highc", "llvmc", "llvm"):
        generated = project(args, original, "eval", route)
        for opt, base in (("-O0", "0x210000"), ("-O2", "0x610000")):
            rebuilt = work / "rebuilt.elf"
            compile_elf(args, work, profile, generated, rebuilt, entry, opt, objects,
                        [*flags, "-fno-strict-aliasing", f"-Wl,{placement}={base}"])
            with rebuilt.open("rb") as stream:
                if any(s["p_type"] == "PT_LOAD" and
                       s["p_vaddr"] <= old_entry < s["p_vaddr"] + s["p_memsz"]
                       for s in ELFFile(stream).iter_segments()):
                    raise AssertionError("original entry is still mapped")
            execute(args, profile, rebuilt)
            print(f"{profile} {route} {opt} passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ("neverd", "clang", "ld"):
        parser.add_argument("--" + tool, required=True, type=Path)
    parser.add_argument("--qemu-dir", type=Path, default=Path("/usr/bin"))
    parser.add_argument("--profiles", nargs="+", choices=PROFILES, default=PROFILES)
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        for profile in args.profiles:
            work = Path(temp) / profile
            work.mkdir()
            check(args, work, profile)


if __name__ == "__main__":
    main()
