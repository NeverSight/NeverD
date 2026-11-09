#!/usr/bin/env python3
"""Compare complete i386 SVM Clang exports with independent original bytes.

Runs scalar instructions on each side of a stopped VMLOAD/VMSAVE/INVLPGA.
Checks the native address, full ECX ASID, live return and C ABI. No VMCB
state transfer, hardware TLB invalidation or privileged instruction executes.
Requires capstone, lief and unicorn.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import capstone
import lief
import unicorn
import unicorn.x86_const as regs


CODE, STOP, STACK, PAGE = 0x10000, 0x1FFFF, 0x80000, 0x1000
OPERATIONS = {"vmload", "vmsave", "invlpga"}
INSTRUCTION_BUDGET = 10000


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(code, entry, corrupt=None):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    native = [i for i in decoder.disasm(code, CODE) if i.mnemonic in OPERATIONS]
    assert len(native) == 1, f"expected one SVM instruction, found {len(native)}"
    instruction = native[0]
    machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    machine.mem_map(CODE, 16 * PAGE)
    machine.mem_write(CODE, code)
    machine.mem_map(STACK, 8 * PAGE)
    sp = STACK + 7 * PAGE - 4
    machine.reg_write(regs.UC_X86_REG_ESP, sp)
    machine.mem_write(sp, struct.pack("<I", STOP))
    machine.reg_write(regs.UC_X86_REG_EFLAGS, 2)
    saved = {r: 0x19283740 + i for i, r in enumerate(
        (regs.UC_X86_REG_EBX, regs.UC_X86_REG_ESI, regs.UC_X86_REG_EDI,
         regs.UC_X86_REG_EBP))}
    for register, value in saved.items():
        machine.reg_write(register, value)
    machine.emu_start(CODE + entry, instruction.address, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(regs.UC_X86_REG_EIP) == instruction.address
    bits = 16 if instruction.bytes[0] == 0x67 else 32
    if corrupt == "address":
        machine.reg_write(regs.UC_X86_REG_EAX, machine.reg_read(regs.UC_X86_REG_EAX) ^ 1)
    if corrupt == "asid":
        machine.reg_write(regs.UC_X86_REG_ECX, machine.reg_read(regs.UC_X86_REG_ECX) ^ 0x80000000)
    observation = {"opcode": bytes(instruction.bytes).hex(), "address_bits": bits,
                   "address": machine.reg_read(regs.UC_X86_REG_EAX) & ((1 << bits) - 1),
                   "asid": machine.reg_read(regs.UC_X86_REG_ECX) if instruction.mnemonic == "invlpga" else None}
    machine.emu_start(instruction.address + instruction.size, STOP, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(regs.UC_X86_REG_EIP) == STOP, "completion did not return"
    assert machine.reg_read(regs.UC_X86_REG_ESP) == sp + 4, "stack was not restored"
    assert all(machine.reg_read(r) == v for r, v in saved.items()), "callee-saved register changed"
    observation["result"] = machine.reg_read(regs.UC_X86_REG_EAX)
    return observation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True, help="Clang compiler")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = sorted(args.exports.glob("svm32_*.c"))
    assert sources, "no complete SVM C exports"
    records, rejected = [], set()
    for source in sources:
        stem, route = source.stem.rsplit(".", 1)
        original = source.with_name(stem + ".bin")
        baseline = execute(original.read_bytes(), 0)
        operation = stem[len("svm32_"):].rsplit("_", 1)[0]
        for opt in ("O0", "O2"):
            output = args.output / (source.stem + "-" + opt)
            obj, ir = Path(str(output) + ".o"), Path(str(output) + ".ll")
            command = [str(args.compiler), "--target=i386-unknown-linux-gnu", "-" + opt,
                       "-ffreestanding", "-fno-lto", "-fno-pic", "-fno-pie",
                       "-fno-stack-protector", "-fno-strict-aliasing", str(source)]
            commands = [command + ["-S", "-emit-llvm", "-o", str(ir)],
                        command + ["-c", "-o", str(obj)]]
            for step, invocation in enumerate(commands):
                result = subprocess.run(invocation, capture_output=True, text=True, timeout=120)
                (args.output / (source.stem + "-" + opt + f"-{step}.log")).write_text(result.stdout + result.stderr)
                assert result.returncode == 0, f"{source.name}: {result.stderr}"
            image = lief.parse(str(obj))
            assert not any(r.section and r.section.name == ".text" for r in image.relocations), \
                "source needs a linked execution fixture"
            code = bytes(image.get_section(".text").content)
            symbol = image.get_symbol("virtualization_test")
            assert symbol is not None
            actual = execute(code, symbol.value)
            assert actual == baseline, f"{source.name}/{opt}: {actual} != {baseline}"
            for corruption in ("address", "asid"):
                if corruption == "asid" and not operation.startswith("invlpga"):
                    continue
                assert execute(code, symbol.value, corruption)[corruption] != baseline[corruption]
                rejected.add(operation + ":" + corruption)
            if operation.endswith("addr16"):
                encoded = bytes.fromhex(baseline["opcode"])
                assert code.count(encoded) == 1
                wrong = code.replace(encoded, b"\x90" + encoded[1:])
                assert execute(wrong, symbol.value)["address_bits"] != baseline["address_bits"]
                rejected.add(operation + ":prefix")
            records.append({"source": source.name, "route": route, "source_sha256": digest(source),
                            "original_sha256": digest(original), "optimization": opt,
                            "object_sha256": digest(obj), "observation": actual})
    (args.output / "results.json").write_text(json.dumps({
        "compiler_sha256": digest(args.compiler),
        "negative_controls": sorted(rejected), "profiles": records,
        "comparisons": len(records)}, indent=2) + "\n")
    print(f"PASS: {len(records)} complete C original-byte comparisons; {len(rejected)} negative controls rejected")


if __name__ == "__main__":
    main()
