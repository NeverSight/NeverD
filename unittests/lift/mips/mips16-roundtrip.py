#!/usr/bin/env python3
"""Qualify MIPS16 source modes and results with GNU tools and pinned Unicorn.

Both source backends are compiled under an adverse -mmicromips default, at
O0/O2 and in both byte orders. GNU as/ld are required: LLVM's MIPS16 assembler
and linker do not implement the necessary instruction and relocation set.
"""

import argparse
import hashlib
import io
from itertools import product
from pathlib import Path
import random
import re
import subprocess
import sys

from elftools.elf.elffile import ELFFile
import roundtrip
from unicorn import UC_HOOK_CODE
from unicorn import mips_const as mips

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts

FUNCTIONS = ("m16_add", "m16_unsigned", "m16_logic", "m16_branch",
             "m16_test", "m16_slot", "m16_skip_data", "m16_memory",
             "m16_stack", "m16_call", "m16_callx", "m16_pc_load")
INPUTS = [(0, 0), (1, 0xffffffff), (0xffffffff, 1),
          (0x7fffffff, 0x80000000), (0x80000000, 0x80000000),
          (0xffff, 0x12345678), (0x12345678, 0x89abcdef)]
rng = random.Random(0x4d3136)
INPUTS += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(17)]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mode_evidence(path, function):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    if elf["e_flags"] & 0x06000000 != 0x04000000:
        raise RuntimeError(f"{path}: expected MIPS16, not microMIPS ELF flags")
    symbols = elf.get_section_by_name(".symtab")
    for index, symbol in enumerate(symbols.iter_symbols()):
        if symbol.name != function:
            continue
        other = symbols.data()[index * symbols["sh_entsize"] + 13]
        if other & 0xf0 != 0xf0:
            raise RuntimeError(f"{path}: missing STO_MIPS_MIPS16 for {function}")
        return {"flags": elf["e_flags"], "symbol_other": other,
                "entry": elf["e_entry"], "symbol_value": symbol["st_value"]}
    raise RuntimeError(f"{path}: missing function {function}")


def expected(function, inputs, little):
    a, b = inputs
    if function == "m16_memory":
        data = b.to_bytes(4, "little" if little else "big")
        byte = data[1] - 256 if data[1] >= 128 else data[1]
        return (byte + int.from_bytes(data[2:4], "little" if little else "big")) & 0xffffffff
    return {"m16_add": a + b, "m16_unsigned": a + 0x8000,
            "m16_logic": a ^ b, "m16_branch": 7 if a == 0 else 9,
            "m16_test": int(a == 0xffff), "m16_slot": a + 1,
            "m16_skip_data": a + b, "m16_stack": a,
            "m16_call": a + b + 1, "m16_callx": a + b + 1,
            "m16_pc_load": 0x89abcdef}[function] & 0xffffffff


def observe(path, little, function, inputs, return_tag, original=None):
    uc, entry = roundtrip.emulator(path, 32, little, function,
                                  original=original, cpu_model=mips.UC_CPU_MIPS32_24KF)
    called = []
    callee = {"m16_call": "m16_add", "m16_callx": "m16_classic_add"}.get(function)
    if callee:
        elf = ELFFile(io.BytesIO(path.read_bytes()))
        address = elf.get_section_by_name(".symtab").get_symbol_by_name(callee)[0]["st_value"] & ~1
        uc.hook_add(UC_HOOK_CODE, lambda _uc, pc, _size, _: called.append(pc) if pc == address else None)
    saved = {reg: 0xa5000000 | (reg << 16) | (reg * 0x101)
             for reg in (*range(16, 24), 30)}
    for reg in range(1, 32):
        uc.reg_write(getattr(mips, f"UC_MIPS_REG_{reg}"), saved.get(reg, 0))
    uc.reg_write(mips.UC_MIPS_REG_SP, roundtrip.STACK)
    uc.reg_write(mips.UC_MIPS_REG_RA, roundtrip.STOP | return_tag)
    arguments = (roundtrip.DATA, inputs[1]) if function == "m16_memory" else inputs
    for index, value in enumerate(arguments):
        uc.reg_write(getattr(mips, f"UC_MIPS_REG_{4 + index}"), value)
    # Execute in the caller's ISA after the return. A canonical PC alone does
    # not prove that the function retained the incoming ISA tag. These literal
    # instructions write v1: MIPS16 LI v1,0x51 or classic ADDIU v1,zero,0x51.
    sentinel = (0x6b51).to_bytes(2, "little" if little else "big") if return_tag else (
        0x24030051).to_bytes(4, "little" if little else "big")
    uc.mem_write(roundtrip.STOP, sentinel)
    stop = roundtrip.STOP + len(sentinel)
    returned = []
    def observe_return(_uc, address, size, _):
        if address == roundtrip.STOP:
            returned.append(size)
    uc.hook_add(UC_HOOK_CODE, observe_return)
    uc.emu_start(entry, stop, count=100000)
    # This model can retain the last instruction's PC after an until-stop.
    # The executed instruction width and result qualify the actual return mode.
    if returned != [len(sentinel)] or uc.reg_read(mips.UC_MIPS_REG_V1) != 0x51:
        raise RuntimeError("MIPS16 scalar leaf did not return in the caller's ISA")
    if callee and called != [address]:
        raise RuntimeError("MIPS16 caller did not execute its linked callee exactly once")
    if uc.reg_read(mips.UC_MIPS_REG_SP) != roundtrip.STACK:
        raise RuntimeError("MIPS16 scalar leaf changed the caller's stack pointer")
    for reg, value in saved.items():
        if uc.reg_read(getattr(mips, f"UC_MIPS_REG_{reg}")) & 0xffffffff != value:
            raise RuntimeError(f"MIPS16 scalar leaf changed saved GPR {reg}")
    memory = (inputs[1].to_bytes(4, "little" if little else "big") + bytes(12)
              if function == "m16_memory" else bytes(16))
    if bytes(uc.mem_read(roundtrip.DATA, 16)) != memory:
        raise RuntimeError("MIPS16 function changed unexpected output-memory bytes")
    return uc.reg_read(mips.UC_MIPS_REG_V0) & 0xffffffff


def run(args, artifacts):
    fixture = Path(__file__).with_name("mips16.S")
    report = {"complete": False, "passed": [], "failures": [],
              "fixture_sha256": digest(fixture), "neverd_sha256": digest(args.neverd)}
    artifacts.write("results.json", report)
    resource = subprocess.check_output([str(args.clang), "-print-resource-dir"], text=True).strip()
    headers = artifacts.work / "includes"
    headers.mkdir()
    (headers / "string.h").write_text("#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\nvoid *memset(void *, int, size_t);\n")
    command = lambda argv, log: roundtrip.run_command(artifacts, argv, log)
    for little in (False, True):
        profile = "mipsel" if little else "mips"
        assemble = [args.assembler, "-mabi=32", "-march=mips32r2", "-EL" if little else "-EB"]
        link = [args.ld, "-m", f"elf32{'l' if little else 'b'}tsmip", "-static"]
        original = artifacts.work / (profile + ".elf")
        obj, log = original.with_suffix(".o"), original.with_suffix(".log")
        command([*assemble, fixture, "-o", obj], log)
        command([*link, "-Ttext", "0x100000", "-e", FUNCTIONS[0], obj, "-o", original], log)
        original_hash = digest(original)
        artifacts.discard(obj, log)
        for function in FUNCTIONS:
            original_mode = mode_evidence(original, function)
            cases = list(product(INPUTS, (0, 1)))
            before = [observe(original, little, function, inputs, tag) for inputs, tag in cases]
            for backend, optimization in product(("highc", "llvmc"), ("-O0", "-O2")):
                stem = artifacts.work / f"{profile}-{function}-{backend}-{optimization[1:]}"
                source, assembly, rebuilt, obj, log = (stem.with_suffix(ext) for ext in (".c", ".s", ".elf", ".o", ".log"))
                try:
                    command([args.neverd, "decompile", "--func", function,
                             *(["--llvm"] if backend == "llvmc" else ["--callees"]), "-o", source, original], log)
                    if "__attribute__((mips16,nomicromips))" not in source.read_text():
                        raise RuntimeError("source lost its explicit compressed ISA contract")
                    # Keep real interworking calls observable at both optimization
                    # levels instead of letting the compiler inline the callee.
                    flags = [args.clang, f"--target={profile}-linux-gnu", "-march=mips32r2", "-mabi=32",
                             "-mmicromips", "-mno-abicalls", "-fno-pic", "-ffreestanding", "-G0",
                             "-fno-addrsig", "-fno-strict-aliasing", "-fno-inline", "-fno-optimize-sibling-calls", "-nostdinc", "-isystem", resource + "/include",
                             "-I", headers, optimization, "-S"]
                    command([*flags, source, "-o", assembly], log)
                    command([*assemble, assembly, "-o", obj], log)
                    objects, dependencies, callee_sources = [obj], [], []
                    generated = ELFFile(io.BytesIO(obj.read_bytes()))
                    for symbol in generated.get_section_by_name(".symtab").iter_symbols():
                        if symbol["st_shndx"] != "SHN_UNDEF" or symbol.name not in ("m16_add", "m16_classic_add"):
                            continue
                        child = stem.with_name(stem.name + "-" + symbol.name)
                        child_c, child_s, child_o = (child.with_suffix(ext) for ext in (".c", ".s", ".o"))
                        dependencies.extend((child_c, child_s, child_o))
                        command([args.neverd, "decompile", "--llvm", "--func", symbol.name, "-o", child_c, original], log)
                        command([*flags, child_c, "-o", child_s], log)
                        command([*assemble, child_s, "-o", child_o], log)
                        objects.append(child_o)
                        callee_sources.append({"name": symbol.name, "source_sha256": digest(child_c)})
                    bindings = [f"--defsym={name}=0x{address}" for name, address in
                                re.findall(r"extern uint8_t (unk_([0-9A-Fa-f]+))\[", source.read_text())] if backend == "llvmc" else []
                    command([*link, "-Ttext", "0x200000", "-e", function, *bindings, *objects, "-o", rebuilt], log)
                    rebuilt_mode = mode_evidence(rebuilt, function)
                    observations = []
                    for (inputs, tag), value in zip(cases, before):
                        wanted = expected(function, inputs, little)
                        after = observe(rebuilt, little, function, inputs, tag,
                                        original if bindings or function == "m16_pc_load" else None)
                        if value != wanted or after != wanted:
                            raise RuntimeError(f"{stem.name}: input={inputs}, original={value}, generated={after}, expected={wanted}")
                        observations.append({"inputs": inputs, "return_tag": tag, "expected": wanted,
                                             "original": value, "generated": after, "stack_and_saved_gprs_preserved": True,
                                             "caller_isa_sentinel_executed": True, "linked_callee_executed": function in ("m16_call", "m16_callx")})
                    report["passed"].append({"profile": profile, "function": function, "backend": backend,
                                             "optimization": optimization, "original_mode": original_mode,
                                             "rebuilt_mode": rebuilt_mode, "original_sha256": original_hash,
                                             "source_sha256": digest(source), "assembly_sha256": digest(assembly),
                                             "rebuilt_sha256": digest(rebuilt), "observations": observations, "callee_sources": callee_sources,
                                             "original_storage_bindings": bindings})
                    print("PASS", stem.name, flush=True)
                except Exception as error:
                    artifacts.keep_source(source)
                    artifacts.keep_source(assembly)
                    report["failures"].append({"case": stem.name, "error": str(error)})
                    artifacts.write("results.json", report)
                    raise
                artifacts.write("results.json", report)
                artifacts.discard(source, assembly, rebuilt, obj, log, *dependencies)
        artifacts.discard(original)
    report["complete"] = True
    artifacts.write("results.json", report)
    print(len(report["passed"]), "MIPS16 source groups passed", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "assembler", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "mips16-roundtrip",
                                 (args.neverd, args.clang, args.assembler, args.ld,
                                  Path(__file__).with_name("mips16.S"))) as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
