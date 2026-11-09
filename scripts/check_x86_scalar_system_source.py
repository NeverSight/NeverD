#!/usr/bin/env python3
"""Compare complete scalar MSR/port and x86 TLB C output with original bytes.

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
from types import SimpleNamespace

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


def execute(code, entry, completion, corrupt=None, mode=32):
    decoder = capstone.Cs(capstone.CS_ARCH_X86,
                          capstone.CS_MODE_64 if mode == 64 else capstone.CS_MODE_32)
    decoder.detail = True
    # Decode boundaries first; newer opcodes may be absent in Capstone 5.
    markers = [(bytes.fromhex('0f01c6'), 'wrmsrns', mode),
               (bytes.fromhex('0f01ff'), 'tlbsync', mode),
               (bytes.fromhex('670f01ff'), 'tlbsync', mode),
               (bytes.fromhex('0f01fe'), 'invlpgb', mode)]
    if mode == 64:
        markers.insert(0, (bytes.fromhex('670f01fe'), 'invlpgb', 32))
    accesses, offset = [], 0
    while offset < len(code):
        for marker, mnemonic, address_bits in markers:
            if code[offset:].startswith(marker):
                instruction = SimpleNamespace(mnemonic=mnemonic, address=CODE + offset,
                                              size=len(marker), address_bits=address_bits)
                break
        else:
            instruction = next(decoder.disasm(code[offset:], CODE + offset, count=1), None)
            assert instruction, 'incomplete instruction decode'
        if instruction.mnemonic in OPERATIONS | {'wrmsrns', 'tlbsync', 'invlpgb'}:
            accesses.append(instruction)
        offset += instruction.size
    assert len(accesses) == 1, 'expected exactly one scalar system instruction'
    instruction = accesses[0]
    wide = mode == 64
    machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_64 if wide else unicorn.UC_MODE_32)
    reg = lambda name: getattr(regs, 'UC_X86_REG_' + ('R' if wide else 'E') + name)
    word = mode // 8
    machine.mem_map(CODE, 16 * PAGE)
    machine.mem_write(CODE, code)
    machine.mem_map(STACK, 8 * PAGE)
    sp = STACK + 7 * PAGE - word
    machine.reg_write(reg('SP'), sp)
    machine.mem_write(sp, struct.pack('<Q' if wide else '<I', STOP))
    machine.reg_write(regs.UC_X86_REG_EFLAGS, 2)
    saved_names = ('BX', 'BP', '12', '13', '14', '15') if wide else ('BX', 'SI', 'DI', 'BP')
    saved = {reg(name): (0x19283740 + i) ^ (completion & 0xFFFFFFFF)
             for i, name in enumerate(saved_names)}
    for register, value in saved.items():
        machine.reg_write(register, value)
    machine.emu_start(CODE + entry, instruction.address, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(reg('IP')) == instruction.address
    port = instruction.mnemonic in ('in', 'out')
    tlb = instruction.mnemonic in ('tlbsync', 'invlpgb')
    writing = instruction.mnemonic in ('wrmsr', 'wrmsrns', 'out')
    if tlb:
        bits = instruction.address_bits
        control = regs.UC_X86_REG_EAX if bits == 32 else regs.UC_X86_REG_RAX
        if corrupt in ('control', 'ids', 'count'):
            register = control if corrupt == 'control' else regs.UC_X86_REG_EDX \
                if corrupt == 'ids' else regs.UC_X86_REG_ECX
            delta = 1 << (bits - 1) if corrupt == 'control' else 1
            machine.reg_write(register, machine.reg_read(register) ^ delta)
        observation = {'operation': instruction.mnemonic, 'bits': bits,
                       'control': machine.reg_read(control)}
        if instruction.mnemonic == 'invlpgb':
            observation.update(ids=machine.reg_read(regs.UC_X86_REG_EDX),
                               count=machine.reg_read(regs.UC_X86_REG_ECX))
    else:
        if corrupt == 'write_value':
            assert writing
            machine.reg_write(regs.UC_X86_REG_EAX, machine.reg_read(regs.UC_X86_REG_EAX) ^ 1)
        if port:
            selector_operand = instruction.operands[0 if writing else 1]
            if corrupt == 'selector':
                if selector_operand.type == capstone.x86.X86_OP_IMM:
                    mutated = bytearray(code)
                    mutated[instruction.address - CODE + instruction.size - 1] ^= 1
                    return execute(bytes(mutated), entry, completion, mode=mode)
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
    assert machine.reg_read(reg('IP')) == STOP, 'completion did not return'
    assert machine.reg_read(reg('SP')) == sp + word, 'stack was not restored'
    assert all(machine.reg_read(r) == v for r, v in saved.items()), 'callee-saved register changed'
    if corrupt == 'result':
        machine.reg_write(reg('AX'), machine.reg_read(reg('AX')) ^ 1)
    observation['result'] = machine.reg_read(reg('AX'))
    return observation


def compare(actual, expected):
    assert actual.keys() == expected.keys(), 'observation fields changed'
    for field in expected:
        assert actual[field] == expected[field], f'{field} mismatch: {actual} != {expected}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--backend-llc', type=Path,
                        help='NeverC: i386 external driver, x64 emitted IR through llc')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--family', choices=('scalar', 'wrmsrns', 'tlb'), default='scalar')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    sources = sorted(args.exports.glob(args.family + '*_*.c'))
    expected_sources = {'scalar': 84, 'wrmsrns': 12, 'tlb': 84}[args.family]
    assert len(sources) == expected_sources, 'missing complete C profiles'
    records, controls = [], []
    for source in sources:
        stem, route = source.stem.rsplit('.', 1)
        original = source.with_name(stem + '.bin')
        mode = 64 if stem.startswith('tlb64_') else 32
        baselines = {v: execute(original.read_bytes(), 0, v, mode=mode) for v in COMPLETIONS}
        for level in ('O0', 'O2'):
            output = args.output / (source.stem + '-' + level)
            ir, obj = Path(str(output) + '.ll'), Path(str(output) + '.o')
            triple = ('x86_64' if mode == 64 else 'i386') + '-unknown-linux-gnu'
            common = [str(args.compiler), '--target=' + triple, '-' + level,
                      '-std=gnu23', '-ffreestanding', '-fno-lto', '-fno-pic', '-fno-pie',
                      '-fno-stack-protector', '-fno-strict-aliasing', str(source)]
            if args.backend_llc:
                common += ['-fno-builtin-mimalloc']
            commands = [common + ['-S', '-emit-llvm', '-o', str(ir)]]
            if args.backend_llc and mode == 64:
                # NeverC's external driver owns Linux i386. For code64, pass
                # its actual IR to the qualified full backend explicitly.
                commands.append([str(args.backend_llc), '-mtriple=' + triple,
                                 '-' + level, '-verify-machineinstrs',
                                 '-relocation-model=static', '-filetype=obj',
                                 str(ir), '-o', str(obj)])
                object_route = 'neverc-ir-to-llvm'
            else:
                commands.append(common + ['-c', '-o', str(obj)] +
                                (['-fllvm-backend=' + str(args.backend_llc)]
                                 if args.backend_llc else []))
                object_route = 'neverc-driver' if args.backend_llc else 'compiler-driver'
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
                compare(execute(code, entry, value, mode=mode), expected)
            expected = baselines[COMPLETIONS[1]]
            fields = ('control', 'result') if expected['operation'] == 'tlbsync' else \
                ('control', 'ids', 'count', 'result') if expected['operation'] == 'invlpgb' else \
                ('selector', 'write_value' if expected['write_value'] is not None else 'result')
            for corruption in fields:
                try:
                    compare(execute(code, entry, COMPLETIONS[1], corruption, mode), expected)
                except AssertionError as error:
                    assert str(error).startswith(corruption + ' mismatch:'), error
                else:
                    raise AssertionError(corruption + ' corruption escaped the oracle')
                controls.append({'source': source.name, 'optimization': level, 'field': corruption})
            records.append({'source': source.name, 'route': route, 'optimization': level,
                            'code_mode': mode, 'object_route': object_route,
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
