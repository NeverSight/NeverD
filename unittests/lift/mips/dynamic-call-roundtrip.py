#!/usr/bin/env python3
"""Compare untyped changing MIPS callbacks with both regenerated C routes.

Requires cross-target Clang/lld, pyelftools and repository-pinned Unicorn.
No debug declarations or retained relocations describe the dynamic target.
"""

import argparse
import hashlib
import io
import random
import re
import subprocess
from itertools import product
from pathlib import Path

from elftools.elf.elffile import ELFFile
from roundtrip import STACK, STOP, emulator, invoke, mips
from unicorn import UC_HOOK_CODE, UcError
from mips_validation_artifacts import MIPSValidationArtifacts


def symbols(path):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    return {s.name: s for s in elf.get_section_by_name(".symtab").iter_symbols()
            if s.name}


def run(args, artifacts):
    work = artifacts.work
    results = []
    artifacts.write("results.json", {"complete": False, "passed": results})

    def command(argv, name):
        log = work / (name + ".log")
        with artifacts.process(argv, log) as (process, _):
            process.wait(timeout=60)
        if process.returncode:
            raise RuntimeError(f"{name}: {log.read_text()[-4096:]}")

    resource = subprocess.check_output([args.clang, "-print-resource-dir"],
                                       text=True).strip()
    rng = random.Random(0xCA11)
    for abi, little, value_bits in product(("32", "n32", "64"), (False, True), (32, 64)):
        if abi == "32" and value_bits == 64:
            continue  # o32's split doubleword ABI is outside this carrier check.
        value_mask = (1 << value_bits) - 1
        values = [0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678,
                  1 << (value_bits - 1), value_mask]
        values += [rng.getrandbits(value_bits) for _ in range(24)]
        bits = 32 if abi == "32" else 64
        profile = f"{abi}-{'le' if little else 'be'}-i{value_bits}"
        fixture = work / (profile + ".c")
        fixture.write_text(("typedef unsigned " +
                            ("long long " if value_bits == 64 else "") + "word;\n") + """
word first_callback(word x, word y) { return x ^ y; }
word second_callback(word x, word y) { return x + y; }
word (*volatile target)(word, word) = first_callback;
word probe(word value) { return target(value, 37) + 1; }
""")
        artifacts.keep_source(fixture)
        arch = ("mips" if bits == 32 else "mips64") + ("el" if little else "")
        environment = {"32": "gnu", "n32": "gnuabin32", "64": "gnuabi64"}[abi]
        flags = [f"--target={arch}-unknown-linux-{environment}", f"-mabi={abi}",
                 f"-march=mips{bits}r2", "-G0", "-mno-abicalls", "-fno-pic",
                 "-ffreestanding", "-fno-strict-aliasing", "-nostdinc",
                 "-isystem", resource + "/include"]
        obj, original = work / (profile + ".o"), work / (profile + ".elf")
        command([args.clang, *flags, "-O2", "-c", fixture, "-o", obj], profile + "-compile")
        command([args.ld, "-e", "probe", "-Ttext=0x20000", obj, "-o", original], profile + "-link")
        artifacts.discard(obj)
        original_symbols = symbols(original)
        original_uc, original_entry = emulator(original, bits, little, "probe")
        slot = original_symbols["target"]["st_value"]
        callbacks = [original_symbols[name]["st_value"]
                     for name in ("first_callback", "second_callback")]
        mask = (1 << bits) - 1
        saved = {r: (0x123456789abc0000 + r) & mask
                 for r in (*range(16, 24), 28, 30)}

        def observe(uc, entry, slots, variant, value):
            pointer = callbacks[variant]
            for location in set(slots):
                uc.mem_write(location, pointer.to_bytes(8 if abi == "64" else 4,
                                                       "little" if little else "big"))
            visits = []
            hook = uc.hook_add(UC_HOOK_CODE, lambda uc, pc, size, _: visits.append(pc),
                               begin=pointer, end=pointer)
            # n32/n64 carry unsigned C words sign-extended in their GPRs.
            argument = (value - (1 << 32)
                        if bits == 64 and value_bits == 32 and value & 0x80000000
                        else value)
            try:
                returned, _ = invoke(uc, entry, bits, little, [argument], initial_registers=saved)
            except UcError as error:
                raise AssertionError(
                    f"{profile} entry {entry:#x}, callback {variant}, input {value:#x}: "
                    f"{error}, pc={uc.reg_read(mips.UC_MIPS_REG_PC):#x}") from error
            finally:
                uc.hook_del(hook)
            expected = ((value + 37 if variant else value ^ 37) + 1) & value_mask
            if bits == 64 and value_bits == 32 and expected & 0x80000000:
                expected |= 0xffffffff00000000
            if (returned != expected or len(visits) != 1 or
                    uc.reg_read(mips.UC_MIPS_REG_SP) != STACK or
                    uc.reg_read(mips.UC_MIPS_REG_RA) != STOP or
                    any(uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}")) != v
                        for r, v in saved.items())):
                state = {"returned": returned, "expected": expected, "visits": visits,
                         "sp": uc.reg_read(mips.UC_MIPS_REG_SP),
                         "ra": uc.reg_read(mips.UC_MIPS_REG_RA),
                         "saved": {r: uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}"))
                                   for r, v in saved.items()
                                   if uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}")) != v}}
                raise AssertionError(f"callback state mismatch: {profile}, {variant}, {value:#x}: {state}")

        for variant, value in product(range(2), values):
            observe(original_uc, original_entry, [slot], variant, value)
        for backend in ("highc", "llvmc"):
            source = work / f"{profile}-{backend}.c"
            command([args.neverd, "decompile", "--func=probe",
                     *(["--llvm"] if backend == "llvmc" else []),
                     original, "-o", source], profile + "-" + backend + "-decompile")
            artifacts.keep_source(source)
            for optimization in (0, 2):
                name = f"{profile}-{backend}-O{optimization}"
                obj, rebuilt = work / (name + ".o"), work / (name + ".elf")
                command([args.clang, *flags, f"-O{optimization}", "-c", source,
                         "-o", obj], name + "-compile")
                bindings = []
                for symbol, record in symbols(obj).items():
                    if record["st_shndx"] != "SHN_UNDEF":
                        continue
                    if symbol in original_symbols:
                        address = original_symbols[symbol]["st_value"]
                    elif match := re.fullmatch(r"(?:unk|off)_([0-9A-Fa-f]+)", symbol):
                        address = int(match[1], 16)
                    else:
                        raise AssertionError(f"unbound source dependency: {symbol}")
                    bindings.append(f"--defsym={symbol}={address}")
                command([args.ld, "-e", "probe", "-Ttext=0x100000", *bindings,
                         obj, "-o", rebuilt], name + "-link")
                uc, entry = emulator(rebuilt, bits, little, "probe", original=original)
                rebuilt_symbols = symbols(rebuilt)
                slots = [slot]
                if "target" in rebuilt_symbols:
                    slots.append(rebuilt_symbols["target"]["st_value"])
                for variant, value in product(range(2), values):
                    observe(uc, entry, slots, variant, value)
                results.append({"profile": profile, "backend": backend,
                                "optimization": optimization, "cases": 2 * len(values),
                                "original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                "rebuilt_sha256": hashlib.sha256(rebuilt.read_bytes()).hexdigest()})
                artifacts.write("results.json", {"complete": False, "passed": results})
                artifacts.discard(obj, rebuilt)
        artifacts.discard(original)
    artifacts.write("results.json", {"complete": True, "passed": results})
    print(f"Passed {len(results)} dynamic MIPS source groups ({sum(r['cases'] for r in results)} executions)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "dynamic-call-roundtrip",
                                 (args.neverd, args.clang, args.ld)) as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
