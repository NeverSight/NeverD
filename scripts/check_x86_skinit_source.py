#!/usr/bin/env python3
"""Compare complete i386 SKINIT C exports with their original machine code.

Link the unmodified NeverC and Clang objects retained by NeverDX86SkinitTests.
Only ordinary instructions before SKINIT run in Unicorn. Check raw EAX and the
preceding image store; no secure launch, reset or architectural fault executes.
Requires capstone, lief, unicorn and an ELF i386 linker.
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


PAGE, ENTRY, STACK = 0x1000, 0x1000, 0x700000
INSTRUCTION_BUDGET = 10000
VALUES = (0, 1, 0xFFFF, 0x10000, 0x4000, 0x7FFFFFFC,
          0x80000000, 0x8765CDEF, 0xFFFFFFFC)
IMAGE = b'\x31' * 64 + b'\x42' * 64


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def launch_address(code, address, original=False):
    decoded = list(capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
                   .disasm(code, address))
    assert sum(i.size for i in decoded) == len(code), 'incomplete instruction decode'
    launch = [i for i in decoded if i.mnemonic == 'skinit']
    assert len(launch) == 1 and bytes(launch[0].bytes) == b'\x0f\x01\xde'
    if not original:
        assert not any(i.mnemonic.startswith(('ret', 'vmrun')) for i in decoded), \
            'old continuation survived'
    return launch[0].address


def execute(original, linked, kind, incoming, corruption=None):
    machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
    pages = set()

    def map_range(address, size):
        assert address + size <= 1 << 32
        for page in range(address & -PAGE, (address + size + PAGE - 1) & -PAGE, PAGE):
            if page not in pages:
                machine.mem_map(page, PAGE)
                pages.add(page)

    map_range(0x4000, len(IMAGE))
    machine.mem_write(0x4000, IMAGE)
    destination = 0x4040
    if linked is None:
        code, entry = original, ENTRY
        boundary = launch_address(code, entry, original=True)
        map_range(entry, len(code))
        machine.mem_write(entry, code)
    else:
        image = lief.parse(str(linked))
        assert isinstance(image, lief.ELF.Binary)
        for segment in image.segments:
            if segment.type == lief.ELF.Segment.TYPE.LOAD:
                map_range(segment.virtual_address, segment.virtual_size)
                machine.mem_write(segment.virtual_address, bytes(segment.content))
        entry = image.get_symbol('secure_launch').value
        text = image.get_section('.text')
        boundary = launch_address(bytes(text.content), text.virtual_address)
        if kind:
            symbol = image.get_symbol('dword_4040')
            if symbol is not None:
                destination = symbol.value
            else:
                symbol = image.get_symbol('dword_4000' if kind == 1 else 'unk_4000')
                assert symbol is not None and symbol.size == len(IMAGE), \
                    'missing image-store observation'
                destination = symbol.value + 64
        assert not [s.name for s in image.symbols if s.name and s.shndx == 0], \
            'unresolved helper in linked source'
    # Patch only the independently decoded launch; stop at its first byte.
    machine.mem_write(boundary, b'\x90' * 3)
    map_range(STACK, 16 * PAGE)
    sp = STACK + 14 * PAGE + 12
    machine.reg_write(regs.UC_X86_REG_ESP, sp)
    machine.mem_write(sp, struct.pack('<II', 0xDEAD0000, incoming))
    machine.reg_write(regs.UC_X86_REG_EFLAGS, 0x246)
    for n, reg in enumerate((regs.UC_X86_REG_EAX, regs.UC_X86_REG_EBX,
                             regs.UC_X86_REG_ECX, regs.UC_X86_REG_EDX,
                             regs.UC_X86_REG_ESI, regs.UC_X86_REG_EDI,
                             regs.UC_X86_REG_EBP)):
        machine.reg_write(reg, (0x87654321 + n * 0x11111111) & 0xFFFFFFFF)
    if kind == 2:
        map_range(incoming, 4)
        if not 0x4000 <= incoming <= 0x407C:
            machine.mem_write(incoming, struct.pack('<I', incoming ^ 0xA5963C69))
    reached = []

    def stop_at_launch(uc, address, size, unused):
        if address == boundary:
            reached.append(address)
            uc.emu_stop()

    machine.hook_add(unicorn.UC_HOOK_CODE, stop_at_launch)
    machine.emu_start(entry, 0, count=INSTRUCTION_BUDGET)
    assert reached == [boundary] and machine.reg_read(regs.UC_X86_REG_EIP) == boundary, \
        'did not reach the launch boundary'
    if corruption == 'eax':
        machine.reg_write(regs.UC_X86_REG_EAX, machine.reg_read(regs.UC_X86_REG_EAX) ^ 1)
    if corruption == 'image_store':
        value = int.from_bytes(machine.mem_read(destination, 4), 'little') ^ 1
        machine.mem_write(destination, struct.pack('<I', value))
    return {'eax': machine.reg_read(regs.UC_X86_REG_EAX),
            'image_store': bytes(machine.mem_read(destination, 4)).hex() if kind else None}


def compare(actual, expected):
    for field in ('eax', 'image_store'):
        assert actual[field] == expected[field], f'{field} mismatch: {actual} != {expected}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exports', type=Path, required=True,
                        help='NeverCI386EmitsNativeLaunchWithoutOldContinuation export directory')
    parser.add_argument('--linker', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    profiles = sorted(args.exports.glob('*/profile.json'))
    assert len(profiles) == 6, 'expected six i386 source profiles'
    records, controls, variants = [], [], set()
    for profile in profiles:
        meta = json.loads(profile.read_text())
        assert meta['arch'] == 2 and meta['format'] == 0
        kind = meta['kind']
        assert kind in (0, 1, 2) and isinstance(meta['no_opt'], bool)
        variants.add((kind, meta['no_opt']))
        original_path = profile.with_name('original.bin')
        original = original_path.read_bytes()
        baselines = {value: execute(original, None, kind, value) for value in VALUES}
        for value, baseline in baselines.items():
            assert baseline['eax'] == (value if kind == 2 else 0x4000 if kind == 1 else 0x8765CDEF)
            if kind:
                expected = 0x31313131 if kind == 1 or value == 0x4000 else value ^ 0xA5963C69
                assert baseline['image_store'] == struct.pack('<I', expected).hex()
        for route in ('high', 'llvm-c'):
            source = profile.with_name(route + '.c')
            for compiler in ('neverc', 'clang'):
                for level in ('O0', 'O2'):
                    stem = ('native-' if compiler == 'neverc' else '') + route + '-' + level
                    obj = profile.with_name(stem + '.o')
                    assert obj.is_file(), obj
                    output = args.output / (profile.parent.name + '-' + stem)
                    linked = Path(str(output) + '.elf')
                    command = [str(args.linker), '-m', 'elf_i386', '-static',
                               '--entry=secure_launch', '-Ttext=0x10000000', str(obj), '-o', str(linked)]
                    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                    Path(str(output) + '.log').write_text(result.stdout + result.stderr)
                    assert result.returncode == 0, result.stderr
                    for value, baseline in baselines.items():
                        compare(execute(original, linked, kind, value), baseline)
                    for corruption in (('eax', 'image_store') if kind else ('eax',)):
                        try:
                            compare(execute(original, linked, kind, VALUES[0], corruption), baselines[VALUES[0]])
                        except AssertionError as error:
                            assert str(error).startswith(corruption + ' mismatch:'), error
                        else:
                            raise AssertionError('corrupted ' + corruption + ' escaped the oracle')
                        controls.append({'profile': profile.parent.name, 'object': obj.name,
                                         'field': corruption})
                    records.append({'profile': meta, 'fixture': profile.parent.name,
                                    'route': route, 'compiler': compiler, 'optimization': level,
                                    'source_sha256': digest(source), 'original_sha256': digest(original_path),
                                    'object_sha256': digest(obj), 'linked_sha256': digest(linked),
                                    'command': command, 'comparisons': len(VALUES)})
    assert variants == {(kind, no_opt) for kind in range(3) for no_opt in (False, True)}
    report = {'profiles': records, 'comparisons': sum(r['comparisons'] for r in records),
              'negative_controls': controls, 'linker_sha256': digest(args.linker),
              'actual_skinit_execution': False, 'successful_launch_qualified': False}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(records)} objects; {report["comparisons"]} complete C original-byte comparisons; '
          f'{len(controls)} negative controls rejected')


if __name__ == '__main__':
    main()
