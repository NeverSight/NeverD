#!/usr/bin/env python3
"""Execute the DWARF-backed n32 pointer/call fixture through both C routes.

Requires clang/lld with MIPS targets, pyelftools and the repository-pinned
Unicorn bindings/library. Generated callers retain the original external
callee, so O0 and O2 must both execute the call and restore the incoming link.
"""

import argparse
import hashlib
import random
import subprocess
from pathlib import Path

from roundtrip import DATA, STACK, STOP, emulator, invoke, mips
from unicorn import UC_HOOK_CODE
from mips_validation_artifacts import MIPSValidationArtifacts


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
    fixture = Path(__file__).with_name("n32-pointer-call.c")
    rng = random.Random(0x32CA11)
    values = [0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 0x12345678]
    values += [rng.getrandbits(32) for _ in range(26)]
    saved = {r: 0x123456789abc0000 + r for r in (*range(16, 24), 28, 30)}

    for little in (False, True):
        profile = "mips64el" if little else "mips64"
        triple = profile + "-unknown-linux-gnuabin32"
        flags = [f"--target={triple}", "-mabi=n32", "-march=mips64r2",
                 "-mno-abicalls", "-fno-pic", "-ffreestanding",
                 "-fno-strict-aliasing", "-nostdinc", "-isystem", resource + "/include"]
        link = [args.ld, "-m", f"elf32{'l' if little else 'b'}tsmipn32", "-e", "probe"]
        obj, original = work / (profile + ".o"), work / (profile + ".elf")
        command([args.clang, *flags, "-O1", "-g", "-c", fixture, "-o", obj], profile + "-compile")
        command([*link, "-Ttext=0x20000", obj, "-o", original], profile + "-link")
        original_uc, original_entry = emulator(original, 64, little, "probe")
        _, callee = emulator(original, 64, little, "touch")

        def observe(uc, entry, value):
            order = "little" if little else "big"
            memory = b"\xa5" * 4 + value.to_bytes(4, order) + b"\x5a" * 8
            uc.mem_write(DATA, memory)
            visits = []
            hook = uc.hook_add(UC_HOOK_CODE, lambda uc, pc, size, _: visits.append(pc),
                               begin=callee, end=callee)
            try:
                returned, output = invoke(uc, entry, 64, little, [DATA], initial_registers=saved)
            finally:
                uc.hook_del(hook)
            expected = value | (0xffffffff00000000 if value & 0x80000000 else 0)
            if (returned != expected or output != memory.hex() or len(visits) != 1 or
                    uc.reg_read(mips.UC_MIPS_REG_SP) != STACK or
                    uc.reg_read(mips.UC_MIPS_REG_RA) != STOP or
                    any(uc.reg_read(getattr(mips, f"UC_MIPS_REG_{r}")) != v
                        for r, v in saved.items())):
                raise AssertionError(f"n32 state mismatch: {profile}, value={value:#x}, return={returned:#x}")
            return returned, output

        expected = [observe(original_uc, original_entry, value) for value in values]
        for backend in ("highc", "llvmc"):
            source = work / f"{profile}-{backend}.c"
            command([args.neverd, "decompile", "--func=probe",
                     *(["--llvm"] if backend == "llvmc" else []),
                     original, "-o", source], f"{profile}-{backend}-decompile")
            artifacts.keep_source(source)
            for optimization in (0, 2):
                name = f"{profile}-{backend}-O{optimization}"
                obj, rebuilt = work / (name + ".o"), work / (name + ".elf")
                command([args.clang, *flags, f"-O{optimization}", "-c", source, "-o", obj], name + "-compile")
                command([*link, "-Ttext=0x40000", f"--defsym=touch={callee}", obj, "-o", rebuilt], name + "-link")
                uc, entry = emulator(rebuilt, 64, little, "probe", original=original)
                for value, baseline in zip(values, expected):
                    if observe(uc, entry, value) != baseline:
                        raise AssertionError(f"differential mismatch: {name}, value={value:#x}")
                results.append({"profile": profile, "backend": backend, "optimization": optimization,
                                "cases": len(values), "original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                "rebuilt_sha256": hashlib.sha256(rebuilt.read_bytes()).hexdigest()})
                artifacts.write("results.json", {"complete": False, "passed": results})
                artifacts.discard(obj, rebuilt)
        artifacts.discard(original)
    artifacts.write("results.json", {"complete": True, "passed": results})
    print(f"Passed {len(results)} n32 source groups ({sum(r['cases'] for r in results)} executions)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "n32-call-roundtrip") as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
