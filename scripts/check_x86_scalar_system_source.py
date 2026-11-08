#!/usr/bin/env python3
"""Compare complete i386 scalar MSR/port C output with original bytes.

Only ordinary instructions execute in Unicorn. The independently decoded
privileged instruction receives a controlled completion. Check selector/value
widths, live results, dead effects and the C ABI. No hardware MSR/port I/O runs.
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
INSTRUCTION_BUDGET = 10000
COMPLETIONS = (0, 0xFEDCBA9876543210, 0x80000000FFFFFFFF)
OPERATIONS = {'rdmsr', 'wrmsr', 'in', 'out'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(code, entry, completion, corrupt=None):
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    decoded = list(decoder.disasm(code, CODE))
    assert sum(i.size for i in decoded) == len(code), 'incomplete instruction decode'
    accesses = [i for i in decoded if i.mnemonic in OPERATIONS]
    assert len(accesses) == 1, 'expected exactly one scalar system instruction'
    instruction = accesses[0]
    machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    machine.mem_map(CODE, 16 * PAGE)
    machine.mem_write(CODE, code)
    machine.mem_map(STACK, 8 * PAGE)
    sp = STACK + 7 * PAGE - 4
    machine.reg_write(regs.UC_X86_REG_ESP, sp)
    machine.mem_write(sp, struct.pack('<I', STOP))
    machine.reg_write(regs.UC_X86_REG_EFLAGS, 2)
    saved = {r: 0x19283740 + i for i, r in enumerate(
        (regs.UC_X86_REG_EBX, regs.UC_X86_REG_ESI, regs.UC_X86_REG_EDI,
         regs.UC_X86_REG_EBP))}
    for register, value in saved.items():
        machine.reg_write(register, value)
    machine.emu_start(CODE + entry, instruction.address, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(regs.UC_X86_REG_EIP) == instruction.address
    port = instruction.mnemonic in ('in', 'out')
    writing = instruction.mnemonic in ('wrmsr', 'out')
    if corrupt == 'write_value':
        assert writing
        machine.reg_write(regs.UC_X86_REG_EAX, machine.reg_read(regs.UC_X86_REG_EAX) ^ 1)
    if port:
        selector_operand = instruction.operands[0 if writing else 1]
        if corrupt == 'selector':
            if selector_operand.type == capstone.x86.X86_OP_IMM:
                mutated = bytearray(code)
                mutated[instruction.address - CODE + instruction.size - 1] ^= 1
                return execute(bytes(mutated), entry, completion)
            machine.reg_write(regs.UC_X86_REG_DX, machine.reg_read(regs.UC_X86_REG_DX) ^ 1)
        selector = selector_operand.imm if selector_operand.type == capstone.x86.X86_OP_IMM \
            else machine.reg_read(regs.UC_X86_REG_DX)
        width = instruction.operands[1 if writing else 0].size * 8
        value = machine.reg_read(regs.UC_X86_REG_EAX) & ((1 << width) - 1)
    else:
        if corrupt == 'selector':
            machine.reg_write(regs.UC_X86_REG_ECX, machine.reg_read(regs.UC_X86_REG_ECX) ^ 1)
        selector, width = machine.reg_read(regs.UC_X86_REG_ECX), 64
        value = machine.reg_read(regs.UC_X86_REG_EAX) | machine.reg_read(regs.UC_X86_REG_EDX) << 32
    observation = {'operation': instruction.mnemonic, 'bits': width,
                   'selector': selector, 'write_value': value if writing else None}
    if not writing:
        if port:
            mask = (1 << width) - 1
            ax = machine.reg_read(regs.UC_X86_REG_EAX)
            machine.reg_write(regs.UC_X86_REG_EAX, (ax & ~mask) | (completion & mask))
        else:
            machine.reg_write(regs.UC_X86_REG_EAX, completion & 0xFFFFFFFF)
            machine.reg_write(regs.UC_X86_REG_EDX, completion >> 32)
    machine.emu_start(instruction.address + instruction.size, STOP, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(regs.UC_X86_REG_EIP) == STOP, 'completion did not return'
    assert machine.reg_read(regs.UC_X86_REG_ESP) == sp + 4, 'stack was not restored'
    assert all(machine.reg_read(r) == v for r, v in saved.items()), 'callee-saved register changed'
    if corrupt == 'result':
        machine.reg_write(regs.UC_X86_REG_EAX, machine.reg_read(regs.UC_X86_REG_EAX) ^ 1)
    observation['result'] = machine.reg_read(regs.UC_X86_REG_EAX)
    return observation


def compare(actual, expected):
    for field in ('operation', 'bits', 'selector', 'write_value', 'result'):
        assert actual[field] == expected[field], f'{field} mismatch: {actual} != {expected}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--backend-llc', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = sorted(args.exports.glob('scalar32_*.c'))
    assert len(sources) == 84, 'expected 42 i386 profiles and both complete C routes'
    records, controls = [], []
    for source in sources:
        stem, route = source.stem.rsplit('.', 1)
        original = source.with_name(stem + '.bin')
        baselines = {v: execute(original.read_bytes(), 0, v) for v in COMPLETIONS}
        for level in ('O0', 'O2'):
            output = args.output / (source.stem + '-' + level)
            ir, obj = Path(str(output) + '.ll'), Path(str(output) + '.o')
            common = [str(args.compiler), '--target=i386-unknown-linux-gnu', '-' + level,
                      '-std=gnu23', '-ffreestanding', '-fno-lto', '-fno-pic', '-fno-pie',
                      '-fno-stack-protector', '-fno-strict-aliasing', str(source)]
            if args.backend_llc:
                common += ['-fno-builtin-mimalloc']
            commands = [common + ['-S', '-emit-llvm', '-o', str(ir)],
                        common + ['-c', '-o', str(obj)] +
                        (['-fllvm-backend=' + str(args.backend_llc)] if args.backend_llc else [])]
            for step, command in enumerate(commands):
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                Path(str(output) + f'-{step}.log').write_text(result.stdout + result.stderr)
                assert result.returncode == 0, result.stderr
            if args.backend_llc:
                text = ir.read_text()
                assert ' asm ' not in text and '@llvm.x86.' in text
            image = lief.parse(str(obj))
            assert isinstance(image, lief.ELF.Binary)
            assert not any(r.has_section and r.section.name == '.text' for r in image.relocations)
            code = bytes(image.get_section('.text').content)
            entry = image.get_symbol('virtualization_test').value
            for value, expected in baselines.items():
                compare(execute(code, entry, value), expected)
            expected = baselines[COMPLETIONS[1]]
            for corruption in ('selector', 'write_value' if expected['write_value'] is not None else 'result'):
                try:
                    compare(execute(code, entry, COMPLETIONS[1], corruption), expected)
                except AssertionError as error:
                    assert str(error).startswith(corruption + ' mismatch:'), error
                else:
                    raise AssertionError(corruption + ' corruption escaped the oracle')
                controls.append({'source': source.name, 'optimization': level, 'field': corruption})
            records.append({'source': source.name, 'route': route, 'optimization': level,
                            'source_sha256': digest(source), 'original_sha256': digest(original),
                            'object_sha256': digest(obj), 'commands': commands,
                            'observations': baselines, 'comparisons': len(COMPLETIONS)})
    report = {'profiles': records, 'comparisons': sum(r['comparisons'] for r in records),
              'negative_controls': controls, 'compiler_sha256': digest(args.compiler),
              'backend_sha256': digest(args.backend_llc) if args.backend_llc else None,
              'privileged_hardware_execution': False}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(records)} objects; {report["comparisons"]} complete C comparisons; '
          f'{len(controls)} negative controls rejected')


if __name__ == '__main__':
    main()
