#!/usr/bin/env python3
"""Compare complete VMFUNC C/native objects with independent original bytes.

Only surrounding scalar instructions execute in Unicorn. VMFUNC is intercepted
to check complete EAX/ECX and a controlled memory effect; this is not EPT, VM exit
or hardware qualification. Requires capstone, lief and unicorn.
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


CODE, STOP, STACK, DATA, PAGE = 0x10000, 0x1ffff, 0x80000, 0x40000, 0x1000
INSTRUCTION_BUDGET = 10000
COMPILER_TIMEOUT_SECONDS = 120
SEEDS = (0, 511, 0xffffffff)
TRIPLES = {0: 'unknown-linux-gnu', 1: 'pc-windows-msvc', 2: 'apple-darwin'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def object_code(path):
    image = lief.parse(str(path))
    sections = [s for s in image.sections if s.name in ('.text', '__text') and s.content]
    assert len(sections) == 1, 'expected one code section'
    section = sections[0]
    symbols = [s for s in image.symbols if s.name.lstrip('_') in
               ('vmfunc_pipeline', 'vmfunc_effects')]
    assert len(symbols) == 1, 'expected one exported function'
    # Each fixture owns exactly one function without code relocations.
    if isinstance(image, lief.ELF.Binary):
        assert not any(r.has_section and r.section.name == '.text' for r in image.relocations)
        entry = symbols[0].value
    elif isinstance(image, lief.COFF.Binary):
        assert not list(section.relocations)
        entry = symbols[0].value
    else:
        assert isinstance(image, lief.MachO.Binary)
        assert not list(section.relocations)
        entry = symbols[0].value - section.virtual_address
    return bytes(section.content), entry


def execute(code, entry, mode, seed, effects, corrupt=None):
    decoder = capstone.Cs(capstone.CS_ARCH_X86,
                          capstone.CS_MODE_64 if mode == 64 else capstone.CS_MODE_32)
    position, sites = entry, []
    while position < len(code):
        instruction = next(decoder.disasm(code[position:], CODE + position, count=1), None)
        assert instruction, 'incomplete instruction decode'
        if instruction.mnemonic == 'vmfunc':
            assert instruction.bytes == bytes.fromhex('0f01d4'), 'opcode mismatch'
            sites.append(instruction)
        position += instruction.size
        if instruction.mnemonic == 'ret':
            break
    assert len(sites) == 1, 'opcode mismatch: expected exactly one VMFUNC'
    site = sites[0]
    machine = unicorn.Uc(unicorn.UC_ARCH_X86,
                         unicorn.UC_MODE_64 if mode == 64 else unicorn.UC_MODE_32)
    reg = lambda name: getattr(regs, 'UC_X86_REG_' + ('R' if mode == 64 else 'E') + name)
    word = mode // 8
    machine.mem_map(CODE, 16 * PAGE)
    machine.mem_write(CODE, code)
    machine.mem_map(STACK, 8 * PAGE)
    machine.mem_map(DATA, PAGE)
    machine.mem_write(DATA, struct.pack('<I', seed))
    sp = STACK + 7 * PAGE - word
    machine.reg_write(reg('SP'), sp)
    machine.mem_write(sp, struct.pack('<Q' if mode == 64 else '<I', STOP))
    machine.reg_write(regs.UC_X86_REG_EFLAGS, 2)
    # Include Win64's extra preserved registers. No fixture has arguments.
    saved_names = ('BX', 'SI', 'DI', 'BP', '12', '13', '14', '15') if mode == 64 \
        else ('BX', 'SI', 'DI', 'BP')
    saved = {reg(name): ((0xd012345600000000 if mode == 64 else 0) |
                         ((0x19283740 + i) ^ seed))
             for i, name in enumerate(saved_names)}
    for register, value in saved.items():
        machine.reg_write(register, value)
    machine.emu_start(CODE + entry, site.address, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(reg('IP')) == site.address, 'instruction not reached'
    if corrupt == 'leaf':
        machine.reg_write(regs.UC_X86_REG_EAX, machine.reg_read(regs.UC_X86_REG_EAX) ^ 1)
    if corrupt == 'index':
        machine.reg_write(regs.UC_X86_REG_ECX, machine.reg_read(regs.UC_X86_REG_ECX) ^ 0x80000000)
    if corrupt == 'prior_memory':
        machine.mem_write(DATA, struct.pack('<I', seed ^ 0x1234))
    observation = {'leaf': machine.reg_read(regs.UC_X86_REG_EAX),
                   'index': machine.reg_read(regs.UC_X86_REG_ECX)}
    if effects:
        observation['prior_memory'] = struct.unpack('<I', machine.mem_read(DATA, 4))[0]
        machine.mem_write(DATA, struct.pack('<I', (seed ^ 0xaabbccdd) & 0xffffffff))
    machine.emu_start(site.address + site.size, STOP, count=INSTRUCTION_BUDGET)
    assert machine.reg_read(reg('IP')) == STOP, 'completion did not return'
    if corrupt == 'stack':
        machine.reg_write(reg('SP'), machine.reg_read(reg('SP')) + word)
    if corrupt == 'callee_saved':
        machine.reg_write(reg('BX'), saved[reg('BX')] ^ 1)
    if corrupt == 'result':
        machine.reg_write(regs.UC_X86_REG_EAX, machine.reg_read(regs.UC_X86_REG_EAX) ^ 1)
    observation['stack'] = machine.reg_read(reg('SP')) - sp
    observation['callee_saved'] = [machine.reg_read(r) for r in saved]
    if corrupt == 'result-high':
        machine.reg_write(reg('AX'), machine.reg_read(reg('AX')) ^ (1 << 32))
    observation['result'] = machine.reg_read(
        reg('AX') if not effects else regs.UC_X86_REG_EAX)
    if effects:
        observation['final_memory'] = struct.unpack('<I', machine.mem_read(DATA, 4))[0]
    return observation


def compare(actual, expected):
    assert actual.keys() == expected.keys(), 'observation fields changed'
    for field in expected:
        assert actual[field] == expected[field], f'{field} mismatch: {actual} != {expected}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--neverc', action='store_true',
                        help='qualify only the supported NeverC x64 source targets')
    parser.add_argument('--backend-llc', type=Path,
                        help='lower NeverC x64 IR through the full LLVM backend')
    parser.add_argument('--objects-from', type=Path,
                        help='replay unchanged objects from an earlier result manifest')
    args = parser.parse_args()
    assert not (args.backend_llc or args.neverc) or args.compiler
    args.neverc = args.neverc or bool(args.backend_llc)
    args.output.mkdir(parents=True, exist_ok=False)
    sources = sorted(args.exports.glob('vmfunc-*-high.c')) + sorted(args.exports.glob('vmfunc-*-llvm.c'))
    sources += sorted(args.exports.glob('effects-*.c'))
    assert len(sources) == 424, f'missing complete C exports: {len(sources)}'
    records, controls = [], []
    previous = {}
    if args.objects_from:
        prior_report = json.loads((args.objects_from / 'results.json').read_text())
        previous = {(r['source'], r['optimization']): r for r in prior_report['profiles']}
        assert prior_report['compiler_sha256'] == (digest(args.compiler) if args.compiler else None)
        assert prior_report['backend_sha256'] == (digest(args.backend_llc) if args.backend_llc else None)
    for source in sources:
        stem, route = source.stem.rsplit('-', 1)
        effects = stem.startswith('effects-')
        fields = stem.split('-')
        mode = int(fields[1])
        if args.neverc and mode != 64:
            continue
        format_id = 0 if effects else int(fields[2])
        if not args.compiler and (effects or route == 'llvm'):
            continue
        original = source.with_name(stem + '.bin')
        baselines = {seed: execute(original.read_bytes(), 0, mode, seed, effects) for seed in SEEDS}
        for seed, expected in baselines.items():
            leaf = 0 if effects or fields[-1] in ('0', '3') else 1 if fields[-1] == '1' else 0xffff0000
            index = seed if effects else int(fields[-2])
            assert expected['leaf'] == leaf and expected['index'] == index, 'raw fixture mismatch'
            assert expected['result'] == (seed ^ 0xaabbccdd if effects else index)
            assert expected['stack'] == mode // 8
            if effects:
                assert expected['prior_memory'] == (seed + 1) & 0xffffffff
        for level in ('O0', 'O2') if args.compiler else ('native',):
            output = args.output / (source.stem + '-' + level)
            commands = []
            ir = Path(str(output) + '.ll')
            if args.compiler:
                obj = Path(str(output) + '.o')
                triple = ('x86_64' if mode == 64 else 'i386') + '-' + TRIPLES[format_id]
                common = [str(args.compiler), '--target=' + triple, '-' + level,
                          '-std=gnu23', '-ffreestanding', '-fno-lto', '-fno-pic', '-fno-pie',
                          '-fno-stack-protector', '-fno-strict-aliasing', str(source)]
                if args.neverc:
                    common += ['-fno-builtin-mimalloc']
                commands.append(common + ['-S', '-emit-llvm', '-o', str(ir)])
                if args.backend_llc:
                    commands.append([str(args.backend_llc), '-mtriple=' + triple, '-' + level,
                                     '-verify-machineinstrs', '-relocation-model=static',
                                     '-filetype=obj', str(ir), '-o', str(obj)])
                else:
                    commands.append(common + ['-c', '-o', str(obj)])
                if args.objects_from:
                    prior = previous[(source.name, level)]
                    ir = args.objects_from / (source.stem + '-' + level + '.ll')
                    obj = args.objects_from / (source.stem + '-' + level + '.o')
                    commands = prior['commands']
                for step, command in enumerate([] if args.objects_from else commands):
                    result = subprocess.run(command, capture_output=True, text=True, timeout=COMPILER_TIMEOUT_SECONDS)
                    Path(str(output) + f'-{step}.log').write_text(result.stdout + result.stderr)
                    assert result.returncode == 0, result.stderr
                if args.neverc:
                    native_expected = effects or fields[-1] in ('0', '3')
                    text = ir.read_text()
                    assert ('call void @llvm.x86.vmfunc.eptp.switch(' in text) == native_expected, text
                    assert (' asm ' not in text) == native_expected, text
            else:
                obj = source.with_name(stem + '.o')
            if args.objects_from:
                prior = previous[(source.name, level)]
                assert prior['source_sha256'] == digest(source)
                assert prior['original_sha256'] == digest(original)
                assert prior['object_sha256'] == digest(obj)
            code, entry = object_code(obj)
            for seed, expected in baselines.items():
                compare(execute(code, entry, mode, seed, effects), expected)
            expected = baselines[SEEDS[1]]
            negatives = ['leaf', 'index', 'result', 'stack', 'callee_saved']
            if effects:
                negatives += ['prior_memory']
            elif mode == 64:
                negatives += ['result-high']
            for corruption in negatives:
                try:
                    compare(execute(code, entry, mode, SEEDS[1], effects, corruption), expected)
                except AssertionError as error:
                    assert str(error).startswith(corruption.split('-')[0] + ' mismatch:'), error
                else:
                    raise AssertionError(corruption + ' escaped the oracle')
                controls.append({'source': source.name, 'optimization': level, 'field': corruption})
            records.append({'source': source.name, 'optimization': level, 'mode': mode,
                            'format': TRIPLES[format_id], 'commands': commands,
                            'source_sha256': digest(source), 'original_sha256': digest(original),
                            'object_sha256': digest(obj), 'comparisons': len(SEEDS),
                            'observations': baselines})
    report = {'profiles': records, 'comparisons': sum(r['comparisons'] for r in records),
              'negative_controls': controls,
              'compiler_sha256': digest(args.compiler) if args.compiler else None,
              'backend_sha256': digest(args.backend_llc) if args.backend_llc else None,
              'privileged_hardware_execution': False,
              'replayed_manifest_sha256': digest(args.objects_from / 'results.json')
              if args.objects_from else None}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(records)} objects, {report["comparisons"]} comparisons, '
          f'{len(controls)} rejected corruption controls')


if __name__ == '__main__':
    main()
