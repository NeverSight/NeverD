#!/usr/bin/env python3
"""Compare complete i386 VMX Clang exports with independent original instructions.

The instruction boundary supplies VMX completions after checking native inputs,
operand widths and memory images. It does not create a hardware VMCS, validate
segment descriptors or model successful VM entry. FS/GS are bound to flat bases
in this transport witness. Requires capstone, lief and unicorn.
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
OPERATIONS = {"vmread", "vmwrite", "invept", "invvpid", "vmclear", "vmptrld",
              "vmptrst", "vmxon", "vmxoff", "vmlaunch", "vmresume", "stgi", "clgi"}
PRESERVE_FLAGS = {"stgi", "clgi"}
SUCCESS_ONLY = PRESERVE_FLAGS | {"vmptrst"}
INSTRUCTION_BUDGET = 10000
PAYLOAD = 0x9172536485A6B7C8


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(code, entry, status):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    instructions = list(decoder.disasm(code, CODE))
    native = [i for i in instructions if i.mnemonic in OPERATIONS]
    assert len(native) == 1, f"expected one instruction, found {len(native)}"
    instruction = native[0]
    machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    machine.mem_map(CODE, 16 * PAGE)
    machine.mem_write(CODE, code)
    machine.mem_map(STACK, 8 * PAGE)
    machine.mem_write(STACK, bytes([0xA5]) * (8 * PAGE))
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

    def register(number):
        return getattr(regs, "UC_X86_REG_" + instruction.reg_name(number).upper())

    observation = {"operation": instruction.mnemonic, "inputs": []}
    destination = None
    memory_operand = None
    for index, operand in enumerate(instruction.operands):
        if operand.type == capstone.x86.X86_OP_REG:
            if instruction.mnemonic != "vmread" or index != 0:
                observation["inputs"].append(["register", operand.size,
                                               machine.reg_read(register(operand.reg))])
        elif operand.type == capstone.x86.X86_OP_MEM:
            base = machine.reg_read(register(operand.mem.base)) if operand.mem.base else 0
            offset = machine.reg_read(register(operand.mem.index)) if operand.mem.index else 0
            destination = (base + offset * operand.mem.scale + operand.mem.disp) & 0xFFFFFFFF
            memory_operand = operand
            memory_input = [
                "memory", operand.size,
                instruction.reg_name(operand.mem.segment) if operand.mem.segment else ""]
            if instruction.mnemonic not in ("vmread", "vmptrst"):
                memory_input.append(bytes(machine.mem_read(destination, operand.size)).hex())
            observation["inputs"].append(memory_input)
        else:
            raise AssertionError("unexpected immediate VMX operand")
    if instruction.mnemonic == "vmread" and status == 0:
        operand = instruction.operands[0]
        if operand.type == capstone.x86.X86_OP_REG:
            machine.reg_write(register(operand.reg), PAYLOAD & 0xFFFFFFFF)
        else:
            machine.mem_write(destination, struct.pack("<I", PAYLOAD & 0xFFFFFFFF))
    elif instruction.mnemonic == "vmptrst":
        machine.mem_write(destination, struct.pack("<Q", PAYLOAD))
    expected_memory = (bytes(machine.mem_read(destination, memory_operand.size))
                       if memory_operand is not None else None)
    if instruction.mnemonic not in PRESERVE_FLAGS:
        flags = machine.reg_read(regs.UC_X86_REG_EFLAGS) & ~0x8D5
        machine.reg_write(regs.UC_X86_REG_EFLAGS,
                          flags | 2 | (0x40 if status == 1 else 1 if status == 2 else 0))
    machine.emu_start(instruction.address + instruction.size, STOP, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(regs.UC_X86_REG_EIP) == STOP, "completion did not return"
    assert machine.reg_read(regs.UC_X86_REG_ESP) == sp + 4, "stack was not restored"
    assert all(machine.reg_read(r) == v for r, v in saved.items()), "callee-saved register changed"
    if expected_memory is not None:
        assert bytes(machine.mem_read(destination, memory_operand.size)) == expected_memory, \
            "compiler accessed native output after completion"
    observation["result"] = machine.reg_read(regs.UC_X86_REG_EAX)
    observation["memory_after"] = expected_memory.hex() if expected_memory is not None else None
    return observation


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True, help="Clang compiler")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = sorted(args.exports.glob("vmx32_*.c"))
    assert sources, "no complete VMX C exports"
    records = []
    rejected = set()
    for source in sources:
        stem, route = source.stem.rsplit(".", 1)
        original = source.with_name(stem + ".bin")
        original_code = original.read_bytes()
        decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        operation, = [i.mnemonic for i in decoder.disasm(original_code, CODE)
                      if i.mnemonic in OPERATIONS]
        statuses = ((0,) if operation in SUCCESS_ONLY else
                    (1, 2) if operation in ("vmlaunch", "vmresume") else (0, 1, 2))
        baseline = {status: execute(original_code, 0, status) for status in statuses}
        for opt in ("O0", "O2"):
            output = args.output / (source.stem + "-" + opt)
            obj, ir = Path(str(output) + ".o"), Path(str(output) + ".ll")
            # The emitted translation unit, including its includes, is untouched.
            command = [str(args.compiler), "--target=i386-unknown-linux-gnu", "-" + opt,
                       "-ffreestanding", "-fno-lto", "-fno-pic", "-fno-pie",
                       "-fno-stack-protector", "-fno-strict-aliasing", str(source)]
            commands = [command + ["-S", "-emit-llvm", "-o", str(ir)],
                        command + ["-c", "-o", str(obj)]]
            for step, invocation in enumerate(commands):
                completed = subprocess.run(invocation, capture_output=True, text=True, timeout=120)
                (args.output / (source.stem + "-" + opt + f"-{step}.log")).write_text(
                    completed.stdout + completed.stderr)
                assert completed.returncode == 0, f"{source.name}: {completed.stderr}"
            image = lief.parse(str(obj))
            section = image.get_section(".text")
            assert not any(r.section and r.section.name == ".text" for r in image.relocations), \
                "source needs a linked execution fixture"
            code = bytes(section.content)
            symbol = image.get_symbol("virtualization_test")
            assert symbol is not None
            for status in statuses:
                result = execute(code, symbol.value, status)
                assert result == baseline[status], f"{source.name}/{opt}/{status}: {result} != {baseline[status]}"
            if operation not in rejected and 1 in statuses and baseline[1]["result"]:
                returns = [i for i in decoder.disasm(code, CODE) if i.mnemonic == "ret"]
                assert len(returns) == 1
                position = returns[0].address - CODE
                wrong = code[:position] + b"\x31\xc0\xc3" + code[position + returns[0].size:]
                assert execute(wrong, symbol.value, 1) != baseline[1], "discarded-status control was accepted"
                rejected.add(operation)
            records.append({"source": source.name, "source_sha256": digest(source),
                            "original_sha256": digest(original), "optimization": opt,
                            "object_sha256": digest(obj), "comparisons": len(statuses)})
            (args.output / "results.json").write_text(json.dumps({
                "compiler_sha256": digest(args.compiler),
                "negative_control_rejected": bool(rejected),
                "discarded_status_controls": sorted(rejected), "profiles": records,
                "comparisons": sum(r["comparisons"] for r in records)}, indent=2) + "\n")
    assert "vmxoff" in rejected, "VMXOFF discarded-status control was not checked"
    print(f"PASS: {len(records)} complete C profiles; {sum(r['comparisons'] for r in records)} original-byte controlled comparisons")


if __name__ == "__main__":
    main()
