#!/usr/bin/env python3
"""Qualify NeverD bounded SGX/SMX state exports independently of the lifter.

Run NeverDX86BoundedLeafTests with NEVERD_BOUNDED_LEAF_EXPORT_DIR first.
--oracle names NeverC's tests/verify-sgx-supervisor-completion.py; its adjacent
raw-opcode/POPFQ reference and Linux signal-context driver provide the physical
oracle. Controlled Unicorn completions verify all four registers, six flags,
reserved flag bits and the calling convention, without implementing an enclave
or measured environment. Physical outcomes are reported separately. Requires
capstone, lief and unicorn. The pipeline route only cross-compiles fixtures.
"""

import argparse
import importlib.util
import itertools
import json
from pathlib import Path
import platform
import re
import struct
import sys
import tempfile

import capstone
import lief
import unicorn
import unicorn.x86_const as regs


LEAVES = {"enclu": (0, 1, 5, 6, 7, 8, 9, 10, 11), "getsec": (0, 6, 7, 8)}
OPCODES = {"enclu": b"\x0f\x01\xd7", "getsec": b"\x0f\x37"}
PACKED_BITS = (8, 10, 12, 14, 15, 0)
FLAG_MASK = 0xd501
CODE, STACK, DATA = 0x100000, 0x200000, 0x300000
PAGE, STOP = 0x10000, CODE + 0xf000
VALUES = ((0, 0, 0), (0xd0123456789abcde, 0x798ab6cdaaaaffff, 0x80000000000000c0),
          (0xffffffffffffffff, 0x0102030405060708, 0x8000000180000001))


def load_oracle(path):
    spec = importlib.util.spec_from_file_location("neverd_bounded_reference", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def packed(value):
    return sum(((value >> index) & 1) << bit for index, bit in enumerate(PACKED_BITS))


def control(bits, leaf):
    return leaf | (0x8800000000000000 if bits == 64 else 0)


def controlled(code, operation, leaf, bits, values, flags, reserved, result, abi):
    mask = (1 << bits) - 1
    word = bits // 8
    pack = lambda value: struct.pack("<Q" if bits == 64 else "<I", value & mask)
    register = lambda name: getattr(regs, "UC_X86_REG_" + ("R" if bits == 64 else "E") + name)
    machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_64 if bits == 64 else unicorn.UC_MODE_32)
    for base in (CODE, STACK, DATA):
        machine.mem_map(base, PAGE)
    machine.mem_write(CODE, code)
    inputs = (control(bits, leaf), *(value & mask for value in values))
    machine.mem_write(DATA, b"".join(pack(value) for value in (*inputs, flags | reserved)))
    machine.mem_write(DATA + PAGE // 2, bytes([0xa5]) * (5 * word))
    sp = STACK + PAGE - 64 - word
    machine.reg_write(register("SP"), sp)
    machine.mem_write(sp, pack(STOP))
    if bits == 64:
        machine.reg_write(register("CX" if abi == "win64" else "DI"), DATA)
        machine.reg_write(register("DX" if abi == "win64" else "SI"), DATA + PAGE // 2)
    else:
        machine.mem_write(sp + word, pack(DATA) + pack(DATA + PAGE // 2))
    saved_names = ("BX", "BP", "12", "13", "14", "15") if bits == 64 else ("BX", "BP", "SI", "DI")
    if bits == 64 and abi == "win64":
        saved_names += ("SI", "DI")
    saved = {register(name): (0xa1b2c3d4e5f60710 + index) & mask for index, name in enumerate(saved_names)}
    for reg, value in saved.items():
        machine.reg_write(reg, value)
    machine.reg_write(regs.UC_X86_REG_EFLAGS, 0x202)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64 if bits == 64 else capstone.CS_MODE_32)
    sites = [instruction for instruction in decoder.disasm(code, CODE) if instruction.mnemonic == operation]
    assert len(sites) == 1, (operation, "native boundary lost or duplicated")
    site = sites[0]
    assert bytes(site.bytes) == OPCODES[operation]
    machine.emu_start(CODE, site.address, count=10000)
    assert machine.reg_read(register("IP")) == site.address
    for name, expected in zip(("AX", "BX", "CX", "DX"), inputs):
        assert machine.reg_read(register(name)) == expected, (operation, leaf, "input", name)
    cpu_flags = machine.reg_read(regs.UC_X86_REG_EFLAGS)
    assert ((cpu_flags & 0xd5) << 8) | ((cpu_flags >> 11) & 1) == flags, "incoming flags changed"
    outputs = tuple(((0x9182736455aa0123 + index * 0x17659843) ^ (value >> 2) ^ (result * 0xfedcba98)) & mask
                    for index, value in enumerate(inputs))
    input_bits = sum(((flags >> bit) & 1) << index for index, bit in enumerate(PACKED_BITS))
    output_flags = packed((input_bits ^ (result * 21)) & 63)
    for name, value in zip(("AX", "BX", "CX", "DX"), outputs):
        machine.reg_write(register(name), value)
    machine.reg_write(regs.UC_X86_REG_EFLAGS,
                      (cpu_flags & ~0x8d5) | ((output_flags >> 8) & 0xd5) | ((output_flags & 1) << 11))
    machine.emu_start(site.address + site.size, STOP, count=10000)
    assert machine.reg_read(register("IP")) == STOP
    actual = bytes(machine.mem_read(DATA + PAGE // 2, 5 * word))
    assert actual == b"".join(pack(value) for value in (*outputs, output_flags)), "register/flag output changed"
    assert all(machine.reg_read(reg) == value for reg, value in saved.items()), "callee-save corruption"
    assert bytes(machine.mem_read(DATA, 5 * word)) == b"".join(pack(value) for value in (*inputs, flags | reserved)), "input memory modified"


def compile_state(args, oracle, directory, operation, leaf, opt):
    stem = f"{args.bits}-{operation}-{leaf}"
    source = args.exports / "state" / stem
    target = "pc-windows-msvc" if args.abi == "win64" else "unknown-linux-gnu"
    obj = directory / f"{stem}-{opt}.o"
    if args.route == "object":
        return source.with_suffix(".o")
    if args.route == "ir":
        ir = source.with_suffix(".ll")
        if opt == "O2":
            optimized = directory / f"{stem}-optimized.ll"
            oracle.run([args.llc.parent / "opt", "-S", "-passes=default<O2>", ir, "-o", optimized])
            ir = optimized
        oracle.run([args.llc, "-mtriple=" + ("x86_64" if args.bits == 64 else "i386") + "-" + target,
                    "-" + opt, "-verify-machineinstrs", "-relocation-model=pic", "-filetype=obj", ir, "-o", obj])
    else:
        src = source.with_name(source.name + "-" + args.route + ".c")
        command = [args.compiler, "-m" + str(args.bits), "-std=gnu11", "-" + opt,
                   "-ffreestanding", "-fno-lto", "-fno-stack-protector",
                   *(["-fPIC"] if args.abi == "sysv" else []),
                   "-Werror=implicit-function-declaration", "-c", src, "-o", obj]
        if args.abi == "win64":
            command += (["-mabi=ms"] if "gcc" in Path(args.compiler).name else
                        ["--target=" + ("x86_64" if args.bits == 64 else "i386") + "-" + target])
        if args.neverc:
            command += ["-fno-builtin-mimalloc", "-mllvm", "-verify-machineinstrs"]
        oracle.run(command)
        if args.neverc:
            native_ir = obj.with_suffix(".native.ll")
            inspect = list(command)
            inspect[inspect.index("-c")] = "-S"
            inspect[inspect.index("-o") + 1] = native_ir
            oracle.run(inspect + ["-emit-llvm"])
            text = native_ir.read_text()
            assert f"@llvm.x86.{operation}.bounded.state(" in text, "NeverC builtin path was not selected"
            assert " asm " not in text, "NeverC source route fell back to assembly"
    return obj


def native_driver(args, oracle, directory):
    if args.abi != "sysv" or args.bits != 64 or platform.system() != "Linux" or platform.machine() != "x86_64":
        return None
    fixtures = args.oracle.parent / "fixtures" / "bounded-leaf-state"
    driver = directory / "bounded-state-host"
    oracle.run([args.host_cc, "-O2", "-Wall", "-Wextra", "-Werror", fixtures / "host.c", "-ldl", "-o", driver])
    for operation in LEAVES:
        oracle.run([args.host_cc, "-fPIC", "-c", fixtures / "reference.S",
                    *(["-DNEVERC_TEST_ENCLU"] if operation == "enclu" else []),
                    "-o", directory / f"reference-{operation}.o"])
    return driver


def native_observation(args, oracle, directory, driver, obj, operation, leaf, opt):
    if driver is None:
        return None
    library = directory / f"{operation}-{leaf}-{opt}.so"
    oracle.run([args.host_cc, "-shared", "-nostdlib", obj, directory / f"reference-{operation}.o", "-o", library])
    result = oracle.run([driver, library.resolve(), operation, str(control(64, leaf))])
    match = re.fullmatch(r"384 native boundaries: (\d+) faults, (\d+) normal completions\s*", result)
    assert match and int(match[1]) + int(match[2]) == 384, result
    return {"operation": operation, "leaf": leaf, "optimization": opt,
            "faults": int(match[1]), "normal_completions": int(match[2])}


def pipeline(args, oracle, directory):
    targets = {"0": "unknown-linux-gnu", "1": "pc-windows-msvc", "2": "apple-darwin"}
    count = 0
    sources = sorted(args.exports.glob(f"{args.bits}-*-*.c"))
    assert len(sources) == 3 * sum(map(len, LEAVES.values())) * 2 * 2, len(sources)
    for source in sources:
        fields = source.stem.split("-")
        fmt, operation = fields[1:3]
        # Use target-independent declarations instead of a target CRT.
        text = source.read_text().replace("#include <string.h>",
                  "extern void *memcpy(void *, const void *, __SIZE_TYPE__);\n"
                  "extern void *memset(void *, int, __SIZE_TYPE__);")
        local = directory / source.name
        local.write_text(text)
        for opt in ("O0", "O2"):
            obj = directory / (source.stem + "-" + opt + ".o")
            triple = ("x86_64" if args.bits == 64 else "i386") + "-" + targets[fmt]
            command = [args.compiler, "--target=" + triple, "-std=gnu11", "-" + opt,
                       "-ffreestanding", "-fno-lto", "-fno-stack-protector", "-fno-strict-aliasing",
                       "-Werror=implicit-function-declaration", "-c", local, "-o", obj]
            if args.neverc:
                command += ["-fno-builtin-mimalloc", "-mllvm", "-verify-machineinstrs"]
            oracle.run(command)
            binary = lief.parse(str(obj))
            decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64 if args.bits == 64 else capstone.CS_MODE_32)
            sites = [inst for section in binary.sections if section.name in (".text", "__text")
                     for inst in decoder.disasm(bytes(section.content), 0) if inst.mnemonic == operation]
            assert len(sites) == 1 and bytes(sites[0].bytes) == OPCODES[operation], (source.name, opt)
            count += 1
    return {"compiled_objects": count, "executed": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--route", choices=("high", "llvm", "ir", "object", "pipeline"), required=True)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--llc", type=Path)
    parser.add_argument("--neverc", action="store_true")
    parser.add_argument("--host-cc", default="cc")
    parser.add_argument("--abi", choices=("sysv", "win64"), default="sysv")
    parser.add_argument("--bits", type=int, choices=(32, 64), default=64)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--keep", type=Path)
    args = parser.parse_args()
    if args.route == "ir" and not args.llc:
        parser.error("IR validation requires --llc")
    if args.neverc and args.bits != 64:
        parser.error("NeverC's native API is x64 only")
    if args.route == "object" and args.abi != "sysv":
        parser.error("production state objects use the ELF SysV ABI")
    args.exports, args.oracle = args.exports.resolve(), args.oracle.resolve()
    if args.llc:
        args.llc = args.llc.resolve()
    oracle = load_oracle(args.oracle)
    with tempfile.TemporaryDirectory(prefix="neverd-bounded-state-") as temporary:
        directory = args.keep.resolve() if args.keep else Path(temporary)
        directory.mkdir(parents=True, exist_ok=True)
        if args.route == "pipeline":
            report = pipeline(args, oracle, directory)
        else:
            driver = native_driver(args, oracle, directory)
            checks, observations = 0, []
            opts = ("production",) if args.route == "object" else ("O0", "O2")
            for operation, leaves in LEAVES.items():
                for leaf, opt in itertools.product(leaves, opts):
                    obj = compile_state(args, oracle, directory, operation, leaf, opt)
                    code = oracle.text_section(obj)
                    for values, flag_bits, reserved, result in itertools.product(VALUES, range(64), (0, 0x2afe), range(4)):
                        controlled(code, operation, leaf, args.bits, values, packed(flag_bits), reserved, result, args.abi)
                        checks += 1
                    observation = native_observation(args, oracle, directory, driver, obj, operation, leaf, opt)
                    if observation:
                        observations.append(observation)
                print(operation + ": state and ABI checks passed", flush=True)
            report = {"controlled_checks": checks, "physical_outcomes": observations,
                      "native_boundaries": sum(o["faults"] + o["normal_completions"] for o in observations),
                      "successful_enclave_or_smx_semantics": False}
        report.update(route=args.route, bits=args.bits, abi=args.abi, compiler=str(args.llc or args.compiler))
        args.report.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report if args.route == "pipeline" else {k: v for k, v in report.items() if k != "physical_outcomes"}), flush=True)


if __name__ == "__main__":
    main()
