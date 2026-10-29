#!/usr/bin/env python3
"""Complete CLI source selection over independently encoded x64 PE/ELF calls."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile


TIMEOUT_SECONDS = 60
STRIDE = 32


def run(command, success=True):
    result = subprocess.run(command, capture_output=True, text=True,
                            timeout=TIMEOUT_SECONDS)
    assert (result.returncode == 0) == success, (command, result)
    return result


def bodies(count, cycle=False, stale=None):
    code = bytearray(b'\xcc' * (STRIDE * (count + 1)))
    sizes = []
    for i in range(count + 1):
        if i < count - 1 or (cycle and i == count - 1):
            target = i + 1 if i < count - 1 else 0
            # sub rsp,40; call next; add eax,1; add rsp,40; ret
            body = (b'\x48\x83\xec\x28\xe8' +
                    struct.pack('<i', STRIDE * (target - i) - 9) +
                    b'\x83\xc0\x01\x48\x83\xc4\x28\xc3')
            if i == stale:
                # Observe the previous CALL's physical return slot.
                body = body[:9] + b'\x48\x8b\x44\x24\xf8' + body[12:]
        else:
            body = b'\xb8\x25\x00\x00\x00\xc3'  # mov eax,37; ret
        code[STRIDE * i:STRIDE * i + len(body)] = body
        sizes.append(len(body))
    return bytes(code), sizes


def image_pe(code, sizes):
    raw_size = (len(code) + 511) & ~511
    pdata_rva = (0x1000 + raw_size + 4095) & ~4095
    pdata = b''.join(struct.pack('<III', 0x1000 + i * STRIDE,
                                0x1000 + i * STRIDE + size,
                                pdata_rva + 0x1000 + (8 if size == 6 else 0))
                     for i, size in enumerate(sizes))
    pdata_size = (len(pdata) + 511) & ~511
    assert pdata_size <= 0x1000
    data = bytearray(0x200 + raw_size + pdata_size + 0x200)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 0x84, 0x8664, 3, 0, 0, 0, 0xf0, 0x22)
    optional = 0x98
    struct.pack_into('<H', data, optional, 0x20b)
    struct.pack_into('<I', data, optional + 4, raw_size)
    struct.pack_into('<IIQ', data, optional + 16, 0x1000, 0x1000, 0x180000000)
    struct.pack_into('<II', data, optional + 32, 0x1000, 0x200)
    struct.pack_into('<II', data, optional + 56, pdata_rva + 0x2000, 0x200)
    struct.pack_into('<H', data, optional + 68, 3)
    struct.pack_into('<I', data, optional + 108, 16)
    struct.pack_into('<II', data, optional + 112 + 3 * 8, pdata_rva, len(pdata))
    sections = [(b'.text', 0x1000, 0x200, raw_size, code, 0x60000020),
                (b'.pdata', pdata_rva, 0x200 + raw_size, pdata_size, pdata, 0x40000040),
                (b'.xdata', pdata_rva + 0x1000, 0x200 + raw_size + pdata_size,
                 0x200, b'\x01\x04\x01\x00\x04\x42\x00\x00\x01\x00\x00\x00', 0x40000040)]
    for i, (name, rva, offset, raw, payload, flags) in enumerate(sections):
        section = optional + 0xf0 + i * 40
        struct.pack_into('<8sIIIIIIHHI', data, section, name, len(payload),
                         rva, raw, offset, 0, 0, 0, 0, flags)
        data[offset:offset + len(payload)] = payload
    return bytes(data), 0x180001000


def image_elf(code, sizes):
    data = bytearray(0x1000)
    data[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<HHIQQQIHHHHHH', data, 16, 2, 62, 1, 0x401000, 64,
                     0, 0, 64, 56, 1, 64, 0, 0)
    struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0x400000, 0x400000,
                     len(data) + len(code), len(data) + len(code), 0x1000)
    data.extend(code)
    # The ELF entry may be a process startup stub; declare the callable
    # functions explicitly, as .pdata does for the PE control.
    names = bytearray(b'\0')
    symbols = bytearray(24)
    for i, size in enumerate(sizes):
        symbols.extend(struct.pack('<IBBHQQ', len(names), 0x12, 0, 1,
                                   0x401000 + i * STRIDE, size))
        names.extend(f'sub_{0x401000 + i * STRIDE:X}'.encode() + b'\0')
    symbol_offset = len(data)
    data.extend(symbols)
    name_offset = len(data)
    data.extend(names)
    section_names = b'\0.text\0.symtab\0.strtab\0.shstrtab\0'
    section_name_offset = len(data)
    data.extend(section_names)
    data.extend(bytes((-len(data)) % 8))
    struct.pack_into('<Q', data, 40, len(data))
    struct.pack_into('<HH', data, 60, 5, 4)
    data.extend(bytes(64))
    for row in ((1, 1, 6, 0x401000, 0x1000, len(code), 0, 0, 16, 0),
                (7, 2, 0, 0, symbol_offset, len(symbols), 3, 1, 8, 24),
                (15, 3, 0, 0, name_offset, len(names), 0, 0, 1, 0),
                (23, 3, 0, 0, section_name_offset, len(section_names), 0, 0, 1, 0)):
        data.extend(struct.pack('<IIQQQQIIQQ', *row))
    return bytes(data), 0x401000


def definitions(source):
    return set(re.findall(r'^\S[^\n]*\b(sub_[0-9A-F]+)\([^\n]*\) \{',
                          source, re.MULTILINE))


def check(args, directory, kind, make_image):
    def fixture(count, **options):
        payload, entry = make_image(*bodies(count, **options))
        path = directory / ('input.' + kind)
        path.write_bytes(payload)
        return path, entry

    def decompile(path, entry, callees=True, success=True, named=False):
        output = directory / 'output.c'
        output.write_text('existing output must survive\n')
        command = [str(args.neverd), 'decompile', str(path), '--no-debug',
                   '--func=' + (f'sub_{entry:X}' if named else hex(entry)),
                   '-o', str(output)]
        if callees:
            command.append('--callees')
        result = run(command, success)
        text = output.read_text()
        if not success:
            assert text == 'existing output must survive\n', text
        return text, result

    path, entry = fixture(3)
    names = {f'sub_{entry + STRIDE * i:X}' for i in range(3)}
    tree = json.loads(run([str(args.neverd), 'calltree', str(path), '--no-debug',
                          '--func=' + hex(entry), '--max-depth=64', '--json']).stdout)
    assert tree['complete'] and not tree['depth_limited'], tree
    assert tree['bottom_up'] == [f'0x{entry + STRIDE * i:X}' for i in (2, 1, 0)], tree
    text, _ = decompile(path, entry)
    assert definitions(text) == names, text
    assert 'neverd.not-decompiled' not in text, text
    named, _ = decompile(path, entry, named=True)
    assert definitions(named) == names, named
    single, _ = decompile(path, entry, callees=False)
    assert definitions(single) == {f'sub_{entry:X}'}, single
    if args.compiler:
        harness = directory / 'harness.c'
        harness.write_text(text + f'\nint main(void) {{ return sub_{entry:X}() != 39; }}\n')
        for optimization in ('-O0', '-O2'):
            exe = directory / ('control-' + kind + optimization + '.exe')
            run([str(args.compiler), '-std=gnu11', optimization, str(harness), '-o', str(exe)])
            run([str(exe)])
    # The closure ends exactly at the public depth ceiling, then exceeds it.
    path, entry = fixture(65)
    text, _ = decompile(path, entry)
    assert len(definitions(text)) == 65, text
    path, entry = fixture(66)
    _, result = decompile(path, entry, success=False)
    assert 'call-depth limit' in result.stderr, result
    # A cycle terminates the graph query. Its source is never executed.
    path, entry = fixture(3, cycle=True)
    text, _ = decompile(path, entry)
    assert definitions(text) == {f'sub_{entry + STRIDE * i:X}' for i in range(3)}, text
    # A refused root cannot overwrite the caller's existing output.
    path, entry = fixture(2, stale=0)
    _, result = decompile(path, entry, success=False)
    assert 'physical CALL return slot' in result.stderr, result
    path, entry = fixture(3, stale=1)
    text, _ = decompile(path, entry)
    assert definitions(text) == {f'sub_{entry:X}', f'sub_{entry + 2 * STRIDE:X}'}, text
    assert text.startswith(f'/* neverd.not-decompiled: sub_{entry + STRIDE:X} at '), text
    assert 'physical CALL return slot' in text, text
    print(kind + ': closure, names, single selection, depth boundary, cycle and refusals passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--neverd', type=Path, required=True)
    parser.add_argument('--compiler', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='neverd-callee-source-') as name:
        for kind, maker in (('exe', image_pe), ('elf', image_elf)):
            check(args, Path(name), kind, maker)
    if not args.compiler:
        print('SKIP: compiled C controls require --compiler')


if __name__ == '__main__':
    main()
