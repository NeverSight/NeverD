#!/usr/bin/env python3
"""Step the synthetic probe exported by NeverDX64CallRCXTests, never a specimen.

Requires Clang, llvm-objcopy, llvm-readobj, and Unicorn. The compiler only creates
COFF objects; this script maps their .text in Unicorn and executes count=1.
"""

import argparse
import hashlib
import json
import pathlib
import random
import re
import struct
import subprocess

from unicorn import UC_ARCH_X86, UC_MODE_64, Uc
from unicorn import x86_const as x86


def reg(name):
    return getattr(x86, "UC_X86_REG_" + name)


def word(value):
    return struct.pack("<Q", value)


def read_word(machine, address):
    return struct.unpack("<Q", machine.mem_read(address, 8))[0]


def check_unwind(machine, text, saved, initial_sp, stop):
    restored = {name: machine.reg_read(reg(name)) for name in saved}
    sp = machine.reg_read(reg("RSP")) + 8
    operations = re.findall(r"0x[0-9A-Fa-f]+: (.*)", text)
    assert operations and "FrameRegister: RBP" in text, text
    for operation in operations:
        if operation.startswith("SET_FPREG reg=RBP, offset="):
            sp = machine.reg_read(reg("RBP")) - int(operation.split("=")[-1], 16)
        elif operation.startswith(("ALLOC_SMALL size=", "ALLOC_LARGE size=")):
            # llvm-readobj has decoded the scaled/unscaled UWOP allocation.
            sp += int(operation.split("=")[-1], 0)
        elif operation.startswith("PUSH_NONVOL reg="):
            name = operation.split("=")[-1]
            restored[name] = read_word(machine, sp)
            sp += 8
        else:
            raise AssertionError("Unvalidated unwind operation: " + operation)
    assert sp == initial_sp and read_word(machine, sp) == stop
    assert restored == saved


def synthetic_callee(rax, rcx, randomizer):
    # Overwrite every volatile integer register and the six volatile XMMs.
    code = bytearray()
    for opcode in (b"\x48\xba", b"\x49\xb8", b"\x49\xb9", b"\x49\xba", b"\x49\xbb"):
        code += opcode + word(randomizer.getrandbits(64))
    for index in range(6):
        code += bytes((0x0F, 0x57, 0xC0 + index * 9))  # xorps xmmN, xmmN
    for offset in (8, 16, 24, 32):
        code += bytes((0x4C, 0x89, 0x5C, 0x24, offset))  # mov [rsp+off], r11
    code += b"\x48\xb8" + word(rax) + b"\x48\xb9" + word(rcx) + b"\xc3"
    return bytes(code)


def step_case(code, unwind, randomizer, case):
    machine = Uc(UC_ARCH_X86, UC_MODE_64)
    for base, size in ((0x10000, 0x1000), (0x20000, 0x1000),
                       (0x30000, 0x1000), (0x100000, 0x20000)):
        machine.mem_map(base, size)
    machine.mem_write(0x10000, code)
    rax, rcx = (randomizer.getrandbits(64) for _ in range(2))
    machine.mem_write(0x20000, synthetic_callee(rax, rcx, randomizer))
    args = [randomizer.getrandbits(64) for _ in range(4)]
    initial_sp, stop = 0x110008 + (case % 16) * 16, 0x10F00
    machine.mem_write(stop, b"\xf4")  # Sentinel; stop before executing it.
    machine.reg_write(reg("RSP"), initial_sp)
    machine.mem_write(initial_sp, word(stop))
    machine.mem_write(initial_sp + 40, word(args[3]))
    machine.mem_write(initial_sp + 48, word(0x30000))
    for name, value in zip(("RCX", "RDX", "R8", "R9"), (0x20000, *args[:3])):
        machine.reg_write(reg(name), value)
    names = ("RBX", "RBP", "RDI", "RSI", "R12", "R13", "R14", "R15")
    saved = {name: randomizer.getrandbits(64) for name in names}
    vectors = {f"XMM{i}": randomizer.getrandbits(128) for i in range(6, 16)}
    for name, value in {**saved, **vectors}.items():
        machine.reg_write(reg(name), value)
    pc, calls, instructions = 0x10000, 0, 0
    for _ in range(256):
        if pc == stop:
            break
        if pc == 0x20000:
            calls += 1
            assert machine.reg_read(reg("RSP")) % 16 == 8
            assert [machine.reg_read(reg(n)) for n in ("RCX", "RDX", "R8", "R9")] == args
            check_unwind(machine, unwind, saved, initial_sp, stop)
        machine.emu_start(pc, 0, count=1)
        instructions += 1
        pc = machine.reg_read(reg("RIP"))
    assert pc == stop and calls == 1
    assert machine.reg_read(reg("RSP")) == initial_sp + 8
    assert machine.reg_read(reg("RAX")) == rax
    assert read_word(machine, 0x30000) == rcx
    for name, value in {**saved, **vectors}.items():
        assert machine.reg_read(reg(name)) == value, name
    return instructions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=pathlib.Path, help="exported synthetic probe.c")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--objcopy", default="llvm-objcopy")
    parser.add_argument("--readobj", default="llvm-readobj")
    options = parser.parse_args()
    options.output.mkdir(parents=True, exist_ok=True)
    source = options.probe.read_bytes()
    assert b"# neverd.win64.call.rcx.v1" in source
    randomizer, cases, instructions = random.Random(0x2047), 0, 0
    for optimization in ("O0", "O1", "O2", "O3", "Os"):
        obj = options.output / (optimization + ".o")
        raw = obj.with_suffix(".bin")
        subprocess.run([options.clang, "--target=x86_64-w64-windows-gnu",
                        "-" + optimization, "-fexceptions", "-funwind-tables",
                        "-c", str(options.probe), "-o", str(obj)], check=True)
        subprocess.run([options.objcopy, "--dump-section", ".text=" + str(raw),
                        str(obj)], check=True)
        unwind = subprocess.check_output([options.readobj, "--unwind", str(obj)], text=True)
        for case in range(128):
            instructions += step_case(raw.read_bytes(), unwind, randomizer, case)
            cases += 1
    result = dict(cases=cases, single_instruction_steps=instructions,
                  probe_sha256=hashlib.sha256(source).hexdigest(),
                  target_binaries_executed=False, native_process_or_vm_started=False,
                  checks=["input and result registers", "one call", "home area",
                          "stack alignment and restoration", "nonvolatile GPR/XMM",
                          "COFF unwind at the suspended call"])
    (options.output / "receipt.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
