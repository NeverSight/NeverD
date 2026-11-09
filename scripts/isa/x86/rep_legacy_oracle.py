# NeverC
# Copyright (C) 2026 NeverC contributors.
# Project and source: https://github.com/NeverSight/NeverC
#
# NeverC is a C23 compiler built on LLVM. The repository's default license is
# the GNU Affero General Public License, version 3 (see LICENSE). Explicit
# file and component licenses continue to apply to the material they cover.
#
# LLVM-origin material retains its upstream copyrights and license notices,
# including the Apache License 2.0 with LLVM Exceptions and applicable legacy
# notices reproduced in llvm/LICENSE.TXT. Other third-party material retains
# its own copyright holders and licenses. NeverC does not claim authorship of
# that upstream material. Credit NeverC for NeverC contributions and retain
# the relevant upstream credits when reusing material containing both.
# LLVM project: https://llvm.org/
# LLVM source: https://github.com/llvm/llvm-project
#
# This file supplies project attribution and provenance information; it does
# not modify any license. Preserve the notices required by the applicable
# license, including when code is copied or adapted with AI/LLM tools.
# ATTRIBUTION.md provides citation guidance and examples. CITATION.cff
# provides machine-readable project citation metadata.
#
# License: GNU AGPL version 3 (see ../../../LICENSE).
# Original source: https://github.com/NeverSight/NeverC/blob/641cd0abc32bf919555ab747844b27a2d3d840da/tests/verify-rep-state.py
# Changes (2026-11-09): moved the legacy witness into NeverD and supplied
# shared x64 metadata and memory semantics through the caller.

"""Legacy REP witnesses for NeverD's Clang and LLVM source routes.

Moved from NeverSight/NeverC tests/verify-rep-state.py at 641cd0abc32b;
NeverC no longer accepts legacy source targets. Shared form metadata and the
sequential memory oracle are supplied by the caller's x64 oracle module.
Only ordinary instructions execute in Unicorn; port completions are controlled.
"""

import itertools


def forms(oracle):
    for op in oracle.OPS:
        for width, address, segment, repeat in itertools.product(
                (1, 2, 4), (16, 32),
                (0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65) if op in oracle.SOURCES else (0,),
                (0, 1) if op in oracle.COMPARES else (0,)):
            yield oracle.Form(op, width, address, segment, repeat)


def emulated32_suite(oracle, matrix, code, corrupt=None, directions=(0, 255)):
    import struct
    import unicorn
    import unicorn.x86_const as regs

    code_base, stack, state, gdt = 0x100000, 0x180000, 0x40000, 0x1b0000
    data_base, size = 0x2000, 4 * oracle.PAGE
    stop = code_base + 0xfff
    sentinel = oracle.SENTINEL & 0xffffffff
    pack = lambda values: struct.pack('<' + 'I' * len(values), *values)
    def descriptor(base, access):
        return struct.pack('<Q', 0xffff | ((base & 0xffffff) << 16) |
                           (access << 40) | (0xf << 48) | (0xc << 52) | ((base >> 24) << 56))
    cases = ports = 0
    for form in matrix:
        raw, offset, length = code[form]
        for count, direction, pattern in itertools.product((0, 1, 7), directions, ('unequal', 'equal', 'late', 'overlap')):
            backward = bool(direction)
            machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
            for address, length_map in ((code_base, oracle.PAGE), (stack, oracle.PAGE), (state, oracle.PAGE),
                                        (gdt, oracle.PAGE), (data_base, size)):
                machine.mem_map(address, length_map)
            machine.mem_write(code_base, raw)
            machine.mem_write(gdt, bytes(8) + descriptor(0, 0x9b) + descriptor(0, 0x93) +
                              descriptor(data_base, 0x93) + descriptor(data_base, 0x93))
            machine.reg_write(regs.UC_X86_REG_GDTR, (0, gdt, 39, 0))
            for reg, selector in ((regs.UC_X86_REG_CS, 8), (regs.UC_X86_REG_DS, 16),
                                   (regs.UC_X86_REG_ES, 16), (regs.UC_X86_REG_SS, 16),
                                   (regs.UC_X86_REG_FS, 24), (regs.UC_X86_REG_GS, 32)):
                machine.reg_write(reg, selector)
            source, destination = oracle.PAGE + 128, 2 * oracle.PAGE + 128
            if pattern == 'overlap':
                destination = source + 1
            memory = bytearray((i * 37 + 19) & 255 for i in range(size))
            accumulator = 0x93827160
            step = -form.width if backward else form.width
            if form.compare and pattern in ('equal', 'late'):
                for index in range(count):
                    si, di = source + index * step, destination + index * step
                    value = memory[si:si + form.width] if form.op == 'cmps' else \
                            (accumulator & ((1 << (form.width * 8)) - 1)).to_bytes(form.width, 'little')
                    memory[di:di + form.width] = value
                if pattern == 'late' and count:
                    memory[destination + (count - 1) * step] ^= 0x80
            machine.mem_write(data_base, bytes(memory))
            segment_base = data_base if form.segment in (0x64, 0x65) else 0
            si, di, cx = data_base + source - segment_base, data_base + destination, count
            if form.address == 16:
                si |= 0x12340000
                di |= 0x23450000
                cx |= 0x34560000
            inputs = [si, di, cx] if form.op in ('movs', 'cmps') else \
                     [si, cx, accumulator] if form.op == 'lods' else \
                     [si if form.op == 'outs' else di, cx, 0xf123 if form.port else accumulator]
            values = inputs + [direction, oracle.FLAG_MASK | 0x2afe]
            machine.mem_write(state, pack(values))
            machine.mem_write(state + 128, pack([sentinel] * 4))
            sp = stack + oracle.PAGE - 32
            machine.reg_write(regs.UC_X86_REG_ESP, sp)
            machine.mem_write(sp, pack([stop, state, state + 128]))
            saved = {reg: 0x87654321 + i for i, reg in enumerate((regs.UC_X86_REG_EBX,
                      regs.UC_X86_REG_EBP, regs.UC_X86_REG_ESI, regs.UC_X86_REG_EDI))}
            for reg, value in saved.items():
                machine.reg_write(reg, value)
            machine.emu_start(code_base, code_base + offset, count=1000)
            assert machine.reg_read(regs.UC_X86_REG_EIP) == code_base + offset, form
            mutations = {'input count': regs.UC_X86_REG_ECX,
                         'input source': regs.UC_X86_REG_ESI,
                         'input destination': regs.UC_X86_REG_EDI,
                         'input accumulator': regs.UC_X86_REG_EAX,
                         'input port': regs.UC_X86_REG_EDX}
            if corrupt in mutations:
                register = mutations[corrupt]
                machine.reg_write(register, machine.reg_read(register) ^
                                  (1 if corrupt == 'input port' else 0x80000000))
            if corrupt in ('input direction', 'input flags'):
                machine.reg_write(regs.UC_X86_REG_EFLAGS,
                                  machine.reg_read(regs.UC_X86_REG_EFLAGS) ^
                                  (0x400 if corrupt == 'input direction' else 1))
            if form.op in ('stos', 'lods', 'scas'):
                assert machine.reg_read(regs.UC_X86_REG_EAX) == accumulator, (form, 'input accumulator')
            assert machine.reg_read(regs.UC_X86_REG_ECX) == cx, (form, 'input count')
            if form.op in oracle.SOURCES:
                assert machine.reg_read(regs.UC_X86_REG_ESI) == si, (form, 'input source')
            if form.op not in ('lods', 'outs'):
                assert machine.reg_read(regs.UC_X86_REG_EDI) == di, (form, 'input destination')
            assert bool(machine.reg_read(regs.UC_X86_REG_EFLAGS) & 0x400) == bool(backward), (form, 'input direction')
            if form.compare:
                flags = machine.reg_read(regs.UC_X86_REG_EFLAGS)
                assert (((flags & 0xd5) << 8) | ((flags >> 11) & 1)) == oracle.FLAG_MASK, (form, 'input flags')
            if form.port:
                assert machine.reg_read(regs.UC_X86_REG_DX) == 0xf123, (form, 'input port')
                machine.reg_write(regs.UC_X86_REG_EDI if form.op == 'ins' else regs.UC_X86_REG_ESI, 0xa1b2c3d4)
                machine.reg_write(regs.UC_X86_REG_ECX, 0x98765432)
                machine.reg_write(regs.UC_X86_REG_EIP, code_base + offset + length)
                expected = [0xa1b2c3d4, 0x98765432, sentinel, sentinel]
                ports += 1
            else:
                done, ax, flags = oracle.memory_oracle(form, memory, source, destination, count,
                                                 accumulator, backward, oracle.FLAG_MASK)
                mask = (1 << form.address) - 1
                update = lambda value, delta: (value & ~mask) | ((value + delta) & mask)
                final_si, final_di, final_cx = update(si, done * step), update(di, done * step), update(cx, -done)
                expected = [final_si, final_di, final_cx] if form.op in ('movs', 'cmps') else \
                           [final_si, final_cx, ax] if form.op == 'lods' else [final_di, final_cx, sentinel]
                expected += [flags if form.compare else sentinel]
                cases += 1
            machine.emu_start(machine.reg_read(regs.UC_X86_REG_EIP), stop, count=10000)
            assert machine.reg_read(regs.UC_X86_REG_EIP) == stop, form
            if corrupt == 'output state':
                value = struct.unpack('<I', machine.mem_read(state + 128, 4))[0]
                machine.mem_write(state + 128, pack([value ^ 0x80000000]))
            if corrupt == 'stack':
                machine.reg_write(regs.UC_X86_REG_ESP, machine.reg_read(regs.UC_X86_REG_ESP) ^ 4)
            if corrupt == 'callee saved':
                machine.reg_write(regs.UC_X86_REG_EBX, machine.reg_read(regs.UC_X86_REG_EBX) ^ 1)
            if corrupt == 'return direction':
                machine.reg_write(regs.UC_X86_REG_EFLAGS, machine.reg_read(regs.UC_X86_REG_EFLAGS) | 0x400)
            if corrupt == 'memory':
                machine.mem_write(data_base, bytes([memory[0] ^ 1]))
            assert machine.reg_read(regs.UC_X86_REG_ESP) == sp + 4, (form, 'stack')
            actual = list(struct.unpack('<4I', machine.mem_read(state + 128, 16)))
            assert actual == expected, (form, 'output state', count, backward, pattern, actual, expected)
            assert not machine.reg_read(regs.UC_X86_REG_EFLAGS) & 0x400, (form, 'return direction')
            for reg, value in saved.items():
                assert machine.reg_read(reg) == value, (form, 'callee saved', reg)
            assert bytes(machine.mem_read(data_base, size)) == bytes(memory), (form, 'memory')
    return cases, ports


def emulated32_negative_controls(oracle, matrix, code):
    controls = []
    for form in matrix:
        if form.width != 1 or form.segment or form.repeat:
            continue
        fields = ['input count', 'input direction', 'output state', 'stack',
                  'callee saved', 'return direction', 'memory']
        if form.op in oracle.SOURCES:
            fields.append('input source')
        if form.op not in ('lods', 'outs'):
            fields.append('input destination')
        if form.op in ('stos', 'lods', 'scas'):
            fields.append('input accumulator')
        if form.compare:
            fields.append('input flags')
        if form.port:
            fields.append('input port')
        for field in fields:
            try:
                emulated32_suite(oracle, [form], code, corrupt=field)
            except AssertionError as error:
                assert isinstance(error.args[0], tuple) and error.args[0][1] == field, error
            else:
                raise AssertionError((form, field, 'corruption escaped oracle'))
            controls.append({'form': form.name, 'field': field})
    return controls
