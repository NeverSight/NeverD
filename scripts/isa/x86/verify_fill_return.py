#!/usr/bin/env python3
"""Single-step the fill fixture with synthetic memory; never load an executable."""

import argparse
import hashlib
import json
from pathlib import Path
import re

from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, __version__ as unicorn_version
from unicorn.x86_const import (
    UC_X86_REG_EFLAGS,
    UC_X86_REG_RAX,
    UC_X86_REG_RCX,
    UC_X86_REG_RDX,
    UC_X86_REG_R8,
    UC_X86_REG_RIP,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-instructions", type=int, default=10000)
    args = parser.parse_args()
    fixture = (Path(__file__).resolve().parents[3] /
               "unittests/lift/core/X64FillReturnTests.cpp")
    match = re.search(r"const uint8_t Code\[\] = \{(.*?)\};", fixture.read_text(), re.S)
    if not match:
        raise RuntimeError("missing instruction fixture")
    code = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match[1]))
    machine = Uc(UC_ARCH_X86, UC_MODE_64)
    machine.mem_map(0x1000, 0x1000)
    machine.mem_write(0x1000, code)
    bases = (0x20000, 0xffff0000, 0x100000020000)
    for address in bases:
        machine.mem_map(address, 0x20000)
    cases = steps = 0
    counts = [*range(257), 511, 512, 513, 1023, 1024, 4095]
    for address in bases:
        for count in counts:
            for alignment in range(16):
                destination = address + 0xf800 + alignment
                machine.mem_write(destination - 1, bytes([0xa5]) * (count + 2))
                for register, value in (
                    (UC_X86_REG_RCX, destination),
                    (UC_X86_REG_RDX, 0x123456789abcde5c),
                    (UC_X86_REG_R8, count),
                    (UC_X86_REG_RIP, 0x1000),
                    (UC_X86_REG_EFLAGS, 2),
                ):
                    machine.reg_write(register, value)
                for _ in range(args.max_instructions):
                    pc = machine.reg_read(UC_X86_REG_RIP)
                    if not 0x1000 <= pc < 0x1000 + len(code):
                        raise AssertionError(f"instruction left fixture: {pc:#x}")
                    # RET is a boundary, not an executed instruction. No native
                    # process, service, image entry or external target is run.
                    if code[pc - 0x1000] == 0xc3:
                        break
                    machine.emu_start(pc, 0x1000 + len(code), count=1)
                    steps += 1
                else:
                    raise AssertionError("instruction budget exhausted")
                expected = (destination if count < 8 else
                            destination + (count // 8) * 8 if count < 79 else
                            (destination + count) & ~15)
                if machine.reg_read(UC_X86_REG_RCX) != expected:
                    raise AssertionError((destination, count, "RCX"))
                if machine.reg_read(UC_X86_REG_RAX) != destination:
                    raise AssertionError((destination, count, "RAX"))
                actual = bytes(machine.mem_read(destination - 1, count + 2))
                if actual != bytes([0xa5]) + bytes([0x5c]) * count + bytes([0xa5]):
                    raise AssertionError((destination, count, "write extent"))
                cases += 1
    result = {
        "unicorn_version": unicorn_version,
        "fixture_sha256": hashlib.sha256(code).hexdigest(),
        "cases": cases,
        "single_instruction_steps": steps,
        "all_checks_passed": True,
        "native_executable_or_vm_started": False,
        "return_instruction_executed": False,
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
