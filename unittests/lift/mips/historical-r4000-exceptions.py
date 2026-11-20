#!/usr/bin/env python3
"""Read R4000 exception state from both endian variants of QEMU MIPSsim.

The boot ROM enters kernel mode, installs an exception at a known instruction
and reads CP0 in the actual general-exception vector. Only the resulting JSON
is retained. Use --big/--little to select the qemu-system executables.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

BEV = 1 << 22
BOOT = 0xBFC00000
READY = 0x6AC1


def immediate(op, rs, rt, value):
    return (op << 26) | (rs << 21) | (rt << 16) | (value & 0xFFFF)


def mtc0(rt, rd):
    return 0x40800000 | rt << 16 | rd << 11


def mfc0(rt, rd):
    return 0x40000000 | rt << 16 | rd << 11


def boot_rom(order, prefix, instructions, return_offset):
    rom = bytearray(4096)

    def place(offset, words):
        for word in words:
            rom[offset:offset + 4] = word.to_bytes(4, order)
            offset += 4

    # The reset value of ERL is one. Clear it, keep BEV and allow enough
    # physical instructions for the profile's Status hazard to expire.
    start = [immediate(15, 0, 8, 0x40), mtc0(8, 12), *[0] * 6, *prefix]
    place(0, [*start, *instructions, 0x1000FFFF, 0])
    epc = BOOT + len(start) * 4
    if return_offset is not None:
        if return_offset % 4 == 0:
            place(return_offset, [12])  # SYSCALL at an aligned destination.
        epc = BOOT + return_offset

    handler = [immediate(15, 0, 26, 0xA000), immediate(13, 26, 26, 0x1000)]
    for index, register in enumerate((12, 13, 14, 8)):
        handler += [mfc0(8, register), *[0] * 4,
                    immediate(43, 26, 8, index * 4)]
    handler += [immediate(13, 0, 8, READY), immediate(43, 26, 8, 16),
                0x1000FFFF, 0]
    place(0x380, handler)
    return rom, epc


def qmp(stream, command, **arguments):
    stream.write(json.dumps({'execute': command, 'arguments': arguments}).encode() + b'\n')
    while True:
        reply = json.loads(stream.readline())
        if 'error' in reply:
            raise RuntimeError(reply)
        if 'return' in reply:
            return reply['return']


def run_machine(executable, rom):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        bios, monitor = root / 'rom', root / 'monitor'
        bios.write_bytes(rom)
        args = [executable, '-M', 'mipssim', '-cpu', 'R4000', '-m', '16M',
                '-bios', str(bios), '-display', 'none', '-serial', 'none',
                '-monitor', 'none', '-qmp', f'unix:{monitor},server=on,wait=off', '-S']
        with subprocess.Popen(args, stderr=subprocess.PIPE) as machine:
            try:
                deadline = time.monotonic() + 10
                while not monitor.exists():
                    if machine.poll() is not None:
                        raise RuntimeError(machine.stderr.read().decode())
                    if time.monotonic() > deadline:
                        raise TimeoutError('QEMU monitor did not start')
                    time.sleep(0.01)
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(10)
                    connection.connect(str(monitor))
                    with connection.makefile('rwb', buffering=0) as stream:
                        assert 'QMP' in json.loads(stream.readline())
                        qmp(stream, 'qmp_capabilities')
                        qmp(stream, 'cont')
                        while time.monotonic() < deadline:
                            text = qmp(stream, 'human-monitor-command',
                                       **{'command-line': 'xp /5wx 0x1000'})
                            words = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{8})\b', text)]
                            if len(words) == 5 and words[-1] == READY:
                                return words[:-1]
                            time.sleep(0.01)
                        registers = qmp(stream, 'human-monitor-command',
                                        **{'command-line': 'info registers'})
                        raise TimeoutError(registers)
            finally:
                machine.terminate()
                try:
                    machine.wait(2)
                except subprocess.TimeoutExpired:
                    machine.kill()


def qualify(executable, order):
    rows = []

    def check(name, prefix, instructions, code, *, slot=False, address=None,
              coprocessor=None, pending=0, return_offset=None):
        rom, expected_epc = boot_rom(order, prefix, instructions, return_offset)
        status, cause, epc, bad = run_machine(executable, rom)
        row = dict(order=order, scenario=name, status=status, cause=cause,
                   epc=epc, badvaddr=bad, expected_epc=expected_epc,
                   rom_sha256=hashlib.sha256(rom).hexdigest())
        assert status & (BEV | 6) == BEV | 2, row
        assert (cause >> 2) & 31 == code and bool(cause >> 31) == slot, row
        assert epc == expected_epc and cause & 0x300 == pending, row
        assert address is None or bad == address, row
        assert coprocessor is None or (cause >> 28) & 3 == coprocessor, row
        rows.append(row)

    for name, opcode, code in [('syscall', 12, 8), ('break', 13, 9)]:
        check(name, [], [opcode], code)
        for taken in (False, True):
            check(f'{name}-slot-{taken}', [immediate(13, 0, 9, int(not taken))],
                  [immediate(4, 9, 0, 2), opcode, 0], code, slot=True)
    check('annulled-syscall', [immediate(21, 0, 0, 1), 12], [13], 9)
    check('signed-add-overflow', [immediate(15, 0, 10, 0x7FFF),
                                  immediate(13, 10, 10, 0xFFFF)],
          [immediate(8, 10, 2, 1)], 12)
    for name, op, code in [('unaligned-load', 35, 4), ('unaligned-store', 43, 5)]:
        check(name, [immediate(15, 0, 10, 0x8000)], [immediate(op, 10, 2, 1)],
              code, address=0x80000001)
    check('cop1-unusable', [], [0x44020000], 11, coprocessor=1)
    check('software-pending-survives-exception',
          [immediate(13, 0, 9, 0x100), mtc0(9, 13), *[0] * 6], [12], 8, pending=0x100)
    check('eret-to-written-epc',
          [immediate(15, 0, 9, 0xBFC0), immediate(13, 9, 9, 0x100), mtc0(9, 14),
           *[0] * 6, immediate(15, 0, 8, 0x40), immediate(13, 8, 8, 2), mtc0(8, 12),
           *[0] * 6], [0x42000018, 0], 8, return_offset=0x100)
    for opcode, name in [(0x40800000, 'mtc0'), (0x40a00000, 'dmtc0')]:
        write_error_epc = opcode | 9 << 16 | 30 << 11
        check(f'eret-to-{name}-errorepc',
              [immediate(15, 0, 9, 0xBFC0), immediate(13, 9, 9, 0x120),
               write_error_epc, *[0] * 6, immediate(15, 0, 8, 0x40),
               immediate(13, 8, 8, 4), mtc0(8, 12), *[0] * 6],
              [0x42000018, 0], 8, return_offset=0x120)
    for reg, level in [(14, 2), (30, 4)]:
        check(f'eret-misaligned-register-{reg}',
              [immediate(15, 0, 9, 0xBFC0), immediate(13, 9, 9, 0x102),
               mtc0(9, reg), *[0] * 6, immediate(15, 0, 8, 0x40),
               immediate(13, 8, 8, level), mtc0(8, 12), *[0] * 6],
              [0x42000018, 0], 4, address=BOOT + 0x102, return_offset=0x102)
    return rows


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--big', default='qemu-system-mips64')
    parser.add_argument('--little', default='qemu-system-mips64el')
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    results = {}
    for endian, binary in [('big', options.big), ('little', options.little)]:
        version = subprocess.check_output([binary, '--version'], text=True).splitlines()[0]
        results[endian] = dict(version=version, executions=qualify(binary, endian))
    options.output.write_text(json.dumps(results, indent=2) + '\n')
