#!/usr/bin/env python3
"""A legacy disassembly provider must not truncate a large unsized body."""
from pathlib import Path
import sys
import tempfile

from transport_test import BASE, Client


def run(worker):
    start = int(BASE, 16)
    count = 65536 + 129
    with tempfile.TemporaryDirectory(prefix='neverd-unsized-') as directory:
        binary = Path(directory) / 'large-unsized.bin'
        binary.write_bytes(b'\x90' * (count - 1) + b'\xc3')
        with Client(worker) as client:
            opened = client.call('open', {'path': str(binary), 'analysis': False})
            assert opened['status'] == 'ok', opened
            for address in [start, start + count - 1, start + 65535, start + 65536]:
                response = client.call('listing', {'address': hex(address), 'before': 0, 'after': 48}, timeout=30)
                assert response['status'] == 'ok', response
                instruction = next((row for row in response['payload']['lines']
                                    if row['kind'] == 'insn' and int(row['address'], 16) == address), None)
                assert instruction is not None, (hex(address), response)
                assert instruction['function_address'] == BASE, instruction
                if address == start + count - 1:
                    assert 'ret' in instruction['text'], instruction
    print('legacy listing: 65665 instructions retain the unsized owner and terminal RET')


if __name__ == '__main__':
    run(sys.argv[1])
