#!/usr/bin/env python3
"""Check four-source register transport in ELF x64 native compiler probes.

The probe ABI is (pointer, i16 mask, accumulator, source0..3), returning the
first of three native calls. Calls use dynamic, zero, and all-one masks, and
zeroing 0,1,0. See LLVM's avx512-native-four-source.ll. All input vectors differ.
This bounded interpreter executes only the emitted moves, stack/mask operations
and returns. It records native operands and supplies a distinct result token;
it does not simulate FP arithmetic, processor feature checks or hardware faults.
Requires capstone and lief. Unrecognized instructions fail explicitly.
"""
import argparse
import json
import re
from pathlib import Path

import capstone
import lief

from verify_x86_memory_address import memory_address

NAMES = ('v4fmaddps', 'v4fnmaddps', 'v4fmaddss', 'v4fnmaddss', 'vp4dpwssd', 'vp4dpwssds')
OPCODES = (0x9a, 0xaa, 0x9b, 0xab, 0x52, 0x53)


def check(path, prefixes, stack_wide_args=False, pointer=0x12345678):
    assert 0 <= pointer < 1 << 64
    binary = lief.parse(str(path))
    section = next(s for s in binary.sections if s.name == '.text')
    code = bytes(section.content)
    functions = {s.name: s.value for s in binary.symbols if s.name in ('four_' + n for n in NAMES)}
    assert len(functions) == 6, (path, functions)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    decoder.detail = True
    result = []
    for kind, name in enumerate(NAMES):
        width = 16 if kind in (2, 3) else 64
        lane_mask = 1 if width == 16 else 0xffff
        inputs = [bytes((i*13 + v*41 + 7) & 255 for i in range(width)) for v in range(5)]
        stack_args = stack_wide_args and width == 64
        vectors = ({i: bytes([0xd0+i])*64 for i in range(5)} if stack_args else
                   {i: value + bytes(64-width) for i, value in enumerate(inputs)})
        gprs = {n: 0x500000 + i*16 for i, n in enumerate(('rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rbp', 'rsp', *(f'r{i}' for i in range(8,16))))}
        gprs.update(rsp=0x400038, rdi=pointer, rsi=0xa55b)
        saved = dict(gprs)
        masks, memory = {}, {}

        def write_memory(address, data):
            for i, b in enumerate(data):
                memory[address+i] = b

        def read_memory(address, size):
            assert all(address+i in memory for i in range(size)), (name, 'uninitialized memory', hex(address), size)
            return bytes(memory[address+i] for i in range(size))

        write_memory(gprs['rsp'], (0xffffffffffffffff).to_bytes(8, 'little'))
        if stack_args:
            for i, value in enumerate(inputs):
                write_memory(gprs['rsp']+8+i*width, value)

        def alias(reg):
            m = re.fullmatch(r'r(\d+)([dwb]?)', reg)
            if m:
                return 'r' + m[1], {'': 8, 'd': 4, 'w': 2, 'b': 1}[m[2]], 0
            aliases = {'eax': 'rax', 'ebx': 'rbx', 'ecx': 'rcx', 'edx': 'rdx',
                       'esi': 'rsi', 'edi': 'rdi', 'esp': 'rsp', 'ebp': 'rbp'}
            if reg in aliases:
                return aliases[reg], 4, 0
            if reg in gprs:
                return reg, 8, 0
            for base, low in (('rax', 'al'), ('rbx', 'bl'), ('rcx', 'cl'), ('rdx', 'dl'),
                              ('rsi', 'sil'), ('rdi', 'dil'), ('rbp', 'bpl'), ('rsp', 'spl')):
                if reg == low:
                    return base, 1, 0
                if reg == base[1:]:
                    return base, 2, 0
            raise AssertionError(('unknown GPR', reg))

        def reg_read(reg):
            if re.fullmatch(r'[xyz]mm\d+', reg):
                size = {'x': 16, 'y': 32, 'z': 64}[reg[0]]
                assert int(reg[3:]) in vectors, (name, 'uninitialized vector', reg)
                return vectors[int(reg[3:])][:size]
            if re.fullmatch('k[0-7]', reg):
                assert reg in masks, (name, 'uninitialized mask', reg)
                return masks[reg].to_bytes(8, 'little')
            base, size, shift = alias(reg)
            return ((gprs[base] >> shift) & ((1 << (size*8))-1)).to_bytes(size, 'little')

        def reg_write(reg, data, vex=False):
            if re.fullmatch(r'[xyz]mm\d+', reg):
                size = {'x': 16, 'y': 32, 'z': 64}[reg[0]]
                assert len(data) >= size
                index = int(reg[3:])
                upper = bytes(64-size) if vex else vectors.get(index, bytes(64))[size:]
                vectors[index] = data[:size] + upper
            elif re.fullmatch('k[0-7]', reg):
                masks[reg] = int.from_bytes(data, 'little')
            else:
                base, size, shift = alias(reg)
                value = int.from_bytes(data, 'little') & ((1 << (size*8))-1)
                gprs[base] = value if size >= 4 else (gprs[base] & ~(((1 << (size*8))-1) << shift)) | (value << shift)

        def address(ins, operand):
            m = operand.mem
            assert not m.segment, (name, 'unexpected ordinary segment access')
            base = int.from_bytes(reg_read(ins.reg_name(m.base)), 'little') if m.base else 0
            index = int.from_bytes(reg_read(ins.reg_name(m.index)), 'little') if m.index else 0
            return (base + index*m.scale + m.disp) & 0xffffffffffffffff

        def read(ins, operand):
            if operand.type == capstone.x86.X86_OP_REG:
                return reg_read(ins.reg_name(operand.reg))[:operand.size]
            if operand.type == capstone.x86.X86_OP_MEM:
                return read_memory(address(ins, operand), operand.size)
            if operand.type == capstone.x86.X86_OP_IMM:
                return (operand.imm & ((1 << (operand.size*8))-1)).to_bytes(operand.size, 'little')
            raise AssertionError(('unknown operand', operand.type))

        def write(ins, operand, data):
            if operand.type == capstone.x86.X86_OP_REG:
                reg_write(ins.reg_name(operand.reg), data, ins.mnemonic.startswith('v'))
            else:
                assert operand.type == capstone.x86.X86_OP_MEM
                write_memory(address(ins, operand), data[:operand.size])

        at, calls, steps, returned = functions['four_' + name], 0, 0, False
        expected_return = bytes((201+i) & 255 for i in range(width))
        if width == 16:
            expected_return = expected_return[:4] + inputs[0][4:]
        while steps < 1000:
            steps += 1
            start, prefix = at, 0
            if code[at] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65):
                prefix, at = code[at], at+1
            if code[at] == 0x62 and at+6 <= len(code):
                p0, p1, p2, opcode, modrm = code[at+1:at+6]
                if (p0 & 7) == 2 and (p1 & 3) == 3 and opcode in OPCODES:
                    assert OPCODES.index(opcode) == kind and calls < 3
                    end, stack, base, index, scale = memory_address(code, at+6, p0, modrm, 64)
                    assert prefix | (0x100 if stack else 0) == prefixes[calls]
                    names = ('rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi') + tuple(f'r{i}' for i in range(8,16))
                    native_address = gprs[names[base]]
                    if index is not None:
                        native_address += gprs[names[index]] * scale
                    assert native_address & 0xffffffffffffffff == saved['rdi']
                    source = ((~p1 >> 3) & 15) | ((~p2 & 8) << 1)
                    assert source & 3 == 0
                    destination = ((modrm >> 3) & 7) | ((~p0 & 0x10) >> 1) | ((~p0 & 0x80) >> 3)
                    k = p2 & 7
                    mask = (masks['k' + str(k)] if k else 0xffff) & lane_mask
                    assert mask == (0xa55b, 0, 0xffff)[calls] & lane_mask
                    assert bool(p2 & 0x80) == (calls == 1)
                    assert vectors[destination][:width] == inputs[0], (name, calls, 'accumulator')
                    for i in range(4):
                        observed = 4 if width == 16 else width
                        assert vectors[source+i][:observed] == inputs[i+1][:observed], (name, calls, 'source', i)
                    value = bytes((201 + calls*17 + i) & 255 for i in range(width))
                    if width == 16:
                        value = value[:4] + inputs[0][4:]
                    vectors[destination] = value + bytes(64-width)
                    calls += 1
                    at = end
                    continue
            ins = next(decoder.disasm(code[start:], start, count=1), None)
            assert ins, (name, start, code[start:start+16].hex())
            at = start + ins.size
            op = ins.operands
            mnemonic = ins.mnemonic
            if mnemonic in ('mov', 'movabs', 'vmovaps', 'vmovups', 'vmovdqa', 'vmovdqu', 'vmovdqa32', 'vmovdqa64', 'vmovdqu32', 'vmovdqu64', 'movaps', 'movups', 'movdqa', 'movdqu'):
                write(ins, op[0], read(ins, op[1]))
            elif mnemonic in ('kmovw', 'kmovd', 'kmovq', 'kmovb'):
                size = {'b': 1, 'w': 2, 'd': 4, 'q': 8}[mnemonic[-1]]
                data = read(ins, op[1])[:size]
                write(ins, op[0], data.ljust(op[0].size, b'\0'))
            elif mnemonic in ('kxorw', 'kxnorw'):
                # LLVM materializes zero/all-one masks from the same register;
                # the result does not depend on the previous mask's value.
                assert op[1].reg == op[2].reg
                reg_write(ins.reg_name(op[0].reg), (0 if mnemonic == 'kxorw' else 0xffff).to_bytes(8, 'little'))
            elif mnemonic in ('xor', 'and', 'sub', 'add'):
                left, right = (int.from_bytes(read(ins, x), 'little') for x in op)
                value = {'xor': lambda: left ^ right, 'and': lambda: left & right,
                         'sub': lambda: left - right, 'add': lambda: left + right}[mnemonic]()
                write(ins, op[0], (value & ((1 << (op[0].size*8))-1)).to_bytes(op[0].size, 'little'))
            elif mnemonic == 'lea':
                write(ins, op[0], address(ins, op[1]).to_bytes(op[0].size, 'little'))
            elif mnemonic == 'push':
                data = read(ins, op[0])
                gprs['rsp'] -= 8
                write_memory(gprs['rsp'], data)
            elif mnemonic == 'pop':
                write(ins, op[0], read_memory(gprs['rsp'], 8))
                gprs['rsp'] += 8
            elif mnemonic == 'vzeroupper':
                vectors = {i: v[:16] + bytes(48) for i, v in vectors.items()}
            elif mnemonic == 'ret':
                assert int.from_bytes(read_memory(gprs['rsp'], 8), 'little') == 0xffffffffffffffff
                returned = True
                break
            else:
                raise AssertionError((path, name, hex(start), mnemonic, ins.op_str))
        assert returned and calls == 3 and vectors[0][:width] == expected_return, (path, name, 'returned value', calls)
        assert all(gprs[n] == saved[n] for n in ('rbx', 'rbp', 'r12', 'r13', 'r14', 'r15'))
        result.append({'name': name, 'calls': calls, 'instructions': steps})
    return {'object': str(path), 'functions': result, 'stack_wide_arguments': stack_wide_args,
            'pointer': pointer,
            'native_arithmetic_executed': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('objects', type=Path, nargs='+')
    parser.add_argument('--prefixes', default='100,100,100')
    parser.add_argument('--pointer', type=lambda value: int(value, 0), default=0x12345678,
                        help='native address bits; the controlled instruction does not dereference them')
    parser.add_argument('--stack-wide-args', action='store_true',
                        help='C frontend ABI without a command-line AVX512 target')
    args = parser.parse_args()
    prefixes = tuple(map(int, args.prefixes.split(',')))
    assert len(prefixes) == 3
    print(json.dumps([check(path, prefixes, args.stack_wide_args, args.pointer) for path in args.objects], indent=2))
