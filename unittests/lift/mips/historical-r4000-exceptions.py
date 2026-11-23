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


def boot_rom(order, prefix, instructions, return_offset, *, reset=False,
             return_words=(12,), wide_epc=False):
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
            place(return_offset, return_words)
        epc = BOOT + return_offset

    # Each possible entry stamps its own offset in the branch delay slot.
    # Installing just the expected handler lets an earlier vector run through
    # zero-filled ROM and impersonate the expected one.
    reader = 0x500
    for vector in ((0, 0x200, 0x280, 0x380) if reset else (0x200, 0x280, 0x380)):
        place(vector, [immediate(4, 0, 0, (reader - vector - 4) // 4),
                       immediate(13, 0, 27, vector)])
    handler = [immediate(15, 0, 26, 0xA000), immediate(13, 26, 26, 0x1000)]
    for index, register in enumerate((12, 13, 14, 8)):
        handler += [mfc0(8, register), *[0] * 4,
                    immediate(43, 26, 8, index * 4)]
    if wide_epc:
        handler += [0x40287000, *[0] * 4, 0x0008403e,  # DMFC0 t0,EPC; DSRL32 t0,t0,0.
                    immediate(43, 26, 8, 24)]
    handler += [immediate(43, 26, 27, 16),
                immediate(13, 0, 8, READY), immediate(43, 26, 8, 20),
                0x1000FFFF, 0]
    place(reader, handler)
    return rom, epc


def qmp(stream, command, **arguments):
    stream.write(json.dumps({'execute': command, 'arguments': arguments}).encode() + b'\n')
    while True:
        reply = json.loads(stream.readline())
        if 'error' in reply:
            raise RuntimeError(reply)
        if 'return' in reply:
            return reply['return']


def read_words(stream, address, count):
    text = qmp(stream, 'human-monitor-command',
               **{'command-line': f'xp /{count}wx 0x{address:x}'})
    return [int(word, 16) for word in re.findall(r'0x([0-9a-fA-F]{8})\b', text)]


def run_machine(executable, rom, *, wide_epc=False):
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
                            if read_words(stream, 0x1014, 1) == [READY]:
                                # A monitor dump reads words sequentially while
                                # the guest runs. Observe completion first, then
                                # freeze the VM before reading CP0's saved words.
                                qmp(stream, 'stop')
                                count = 7 if wide_epc else 6
                                words = read_words(stream, 0x1000, count)
                                if len(words) != count or words[5] != READY:
                                    raise RuntimeError(f'incomplete register dump: {words}')
                                if wide_epc:
                                    words[2] |= words[6] << 32
                                return words[:5]
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
              coprocessor=None, pending=0, return_offset=None, vector=0x380,
              return_words=(12,), trap_pc=None, wide_epc=False):
        rom, expected_epc = boot_rom(order, prefix, instructions, return_offset,
                                     return_words=return_words, wide_epc=wide_epc)
        if trap_pc is not None:
            expected_epc = trap_pc
        status, cause, epc, bad, observed = run_machine(executable, rom, wide_epc=wide_epc)
        row = dict(order=order, scenario=name, handler=BOOT + observed, status=status, cause=cause,
                   epc=epc, badvaddr=bad, expected_epc=expected_epc,
                   rom_sha256=hashlib.sha256(rom).hexdigest())
        assert observed == vector, row
        assert status & (BEV | 6) == BEV | 2, row
        assert (cause >> 2) & 31 == code and bool(cause >> 31) == slot, row
        assert epc == expected_epc and cause & 0x300 == pending, row
        assert address is None or bad == address, row
        assert coprocessor is None or (cause >> 28) & 3 == coprocessor, row
        rows.append(row)

    # Placing the same register reader at the reset vector observes Status
    # before any guest MTC0, and proves the first ROM fetch in QEMU.
    reset_rom, _ = boot_rom(order, [], [], None, reset=True)
    reset_status, _, _, _, observed = run_machine(executable, reset_rom)
    assert observed == 0, hex(observed)
    assert reset_status & (BEV | (3 << 20) | 4) == BEV | 4, hex(reset_status)
    rows.append(dict(order=order, scenario='cold-reset', pc=BOOT, status=reset_status))

    # Assign every tag explicitly. An invalid entry can still match, so a
    # freshly reset, otherwise unobserved TLB is not evidence of a refill.
    tlb = [mtc0(0, 5), mtc0(0, 2), mtc0(0, 3),
           immediate(15, 0, 10, 0x7100), immediate(9, 0, 8, 47)]
    loop = len(tlb)
    tlb += [mtc0(8, 0), mtc0(10, 10), *[0] * 6, 0x42000002,
            immediate(9, 10, 10, 0x2000), immediate(9, 8, 8, -1)]
    tlb += [immediate(1, 8, 1, loop - len(tlb) - 1), 0]  # BGEZ t0,loop.
    va = 0x1234000
    tlb += [immediate(15, 0, 10, va >> 16),
            immediate(13, 10, 10, va & 0xffff), mtc0(10, 10), *[0] * 6]
    for name, major, code in [('load', 35, 2), ('store', 43, 3)]:
        check('refill-' + name, tlb, [immediate(major, 10, 2, 0)],
              code, address=va, vector=0x200)
        indexed = [mtc0(0, 0), *[0] * 6, 0x42000002, *[0] * 6]
        check('invalid-' + name, tlb + indexed, [immediate(major, 10, 2, 0)],
              code, address=va)
    # Kernel references to low addresses where R4000 and QEMU agree on the
    # vector. Split KX/UX settings are covered by the native regression.
    for mode, vector in ((0x40, 0x200), (0xa0, 0x280)):
        prefix = [immediate(15, 0, 8, 0x40), immediate(13, 8, 8, mode),
                  mtc0(8, 12), *[0] * 6]
        check(f'refill-kernel-status-{mode:x}', tlb + prefix,
              [immediate(35, 10, 2, 0)], 2, address=va, vector=vector)
    clean = [immediate(13, 0, 8, 2), mtc0(8, 2), *indexed]
    check('clean-store', tlb + clean, [immediate(43, 10, 2, 0)], 1, address=va)

    # R4000PC/SC erratum 27. Probe first. A hit annuls the TLBWR, avoiding a
    # duplicate tag. On a miss its result is used after NOP and ERET. Keep
    # the instructions and data in two uncached physical frames distinct.
    frames = [immediate(15, 0, 9, 0xa000)]
    for frame, trap, marker in ((0x2000, 12, 0x1357), (0x4000, 13, 0x2468)):
        for offset, word in ((0, trap), (8, marker)):
            frames += [immediate(13, 0, 8, word), immediate(43, 9, 8, frame + offset)]
    for existing in (False, True):
        setup = tlb + frames
        if existing:
            setup += [immediate(13, 0, 8, (0x2000 >> 6) | 0x12), mtc0(8, 2), *indexed]
        setup += [immediate(13, 0, 8, (0x4000 >> 6) | 0x12), mtc0(8, 2),
                  *[0] * 6, 0x42000008, *[0] * 6, mfc0(27, 0)]  # TLBP, then Index.
        # The first fetch at va traps on a different opcode in each frame.
        # For a data access, return to unmapped ROM and read the mapped word
        # before any other data access. Unexpected values reach BREAK.
        for fetch in (True, False):
            dest = va if fetch else BOOT + 0x400
            prefix = setup + [immediate(15, 0, 8, dest >> 16),
                              immediate(13, 8, 8, dest), mtc0(8, 14),
                              immediate(15, 0, 8, 0x40), immediate(13, 8, 8, 2),
                              mtc0(8, 12), immediate(13, 0, 11, 0x1357 if existing else 0x2468),
                              *[0] * 6]
            tail = [immediate(1, 27, 2, 1), 0x42000006, 0, 0x42000018]  # BLTZL; TLBWR; NOP; ERET.
            name = 'tlbwr-' + ('annulled-' if existing else 'taken-')
            if fetch:
                check(name + 'fetch', prefix, tail, 8 if existing else 9, trap_pc=va)
            else:
                probe = [immediate(35, 10, 2, 8), 0, immediate(5, 2, 11, 2), 0, 12, 13]
                check(name + 'load', prefix, tail, 8, return_offset=0x400,
                      return_words=probe, trap_pc=dest + 16)

    # Two 4 KiB pages differ only in VPN2[39:32]. Prime both translations,
    # rewrite the high tag, then check the first mapped load and the first
    # fetch. KSEG1 hosts the probe so its own instruction fetches bypass TLB.
    high = (1 << 32) | va
    probe = [*tlb, *frames, immediate(15, 0, 8, 0x40), immediate(13, 8, 8, 0xa0),
             mtc0(8, 12), *[0] * 6,
             immediate(13, 0, 12, 1), 0x000c603c, (12 << 21) | (10 << 16) | (12 << 11) | 37]
    # t2 = low VA, t4 = high VA (DSLL32 followed by OR).
    def write_page(index, address_register, frame):
        return [immediate(13, 0, 8, index), mtc0(8, 0),
                0x40a00000 | (address_register << 16) | (10 << 11),  # DMTC0 EntryHi
                immediate(13, 0, 8, (frame >> 6) | 0x16), mtc0(8, 2),
                *[0] * 6, 0x42000002, *[0] * 6]  # TLBWI

    def check_marker(address_register, marker):
        return [immediate(35, address_register, 2, 8), immediate(13, 0, 11, marker),
                immediate(4, 2, 11, 2), 0, 0x34]  # Unexpected value reaches TEQ.

    probe += [*write_page(0, 10, 0x2000), *write_page(1, 12, 0x4000),
              *check_marker(10, 0x1357), *check_marker(12, 0x2468)]
    jump_to_probe = [0x08000000 | (((BOOT + 0x600) >> 2) & 0x03ffffff), 0]
    jump_high = [(12 << 21) | 8, 0]  # JR t4.
    check('xtlb-distinct-vpn-fetch', [], jump_to_probe, 9, return_offset=0x600,
          return_words=probe + jump_high, trap_pc=high, wide_epc=True)
    check('xtlb-replacement-first-load-and-fetch', [], jump_to_probe, 8, return_offset=0x600,
          return_words=probe + write_page(1, 12, 0x2000) + check_marker(12, 0x1357) + jump_high,
          trap_pc=high, wide_epc=True)

    for name, opcode, code in [('syscall', 12, 8), ('break', 13, 9)]:
        check(name, [], [opcode], code)
        for taken in (False, True):
            check(f'{name}-slot-{taken}', [immediate(13, 0, 9, int(not taken))],
                  [immediate(4, 9, 0, 2), opcode, 0], code, slot=True)
    # The expected non-trapping instruction reaches a distinct BREAK. Checking
    # its EPC detects an early Trap even when the branch-delay bit is clear.
    for selector, name, taken, clear in [
            (0, 'tge', 3, -3), (1, 'tgeu', -1, 3),
            (2, 'tlt', -3, 3), (3, 'tltu', 3, -1),
            (4, 'teq', -2, 3), (6, 'tne', 3, -2)]:
        for imm in (False, True):
            word = (immediate(1, 8, 8 + selector, -2) if imm else
                    (8 << 21) | (9 << 16) | (0x155 << 6) | (48 + selector))
            label = name + ('-immediate' if imm else '-register')
            common = [immediate(9, 0, 9, -2)]
            check(label, common + [immediate(9, 0, 8, taken)], [word], 13)
            check(label + '-clear', common + [immediate(9, 0, 8, clear), word], [13], 9)
    check('trap-high-half', [immediate(9, 0, 8, 1), (8 << 16) | (8 << 11) | 60],
          [(8 << 21) | 54], 13)  # DSLL32 t0,t0,0 then TNE t0,zero.
    for taken in (False, True):
        check(f'trap-slot-{taken}', [immediate(13, 0, 9, int(not taken))],
              [immediate(4, 9, 0, 2), 0x34, 0], 13, slot=True)
    check('annulled-trap', [immediate(21, 0, 0, 1), 0x34], [13], 9)
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
