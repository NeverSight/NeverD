"""Read-only XBEGIN observations at the two QEMU debugger stops."""

import struct
import unittest

from scripts.gdb_xbegin_capture import XbeginObservation


PC = 0xffff800100001030
HANDLER = 0xffff900100006780
OTHER = HANDLER + 0x100
IDT = 0x2210
RSP = 0x8018
FRAME_SP = (RSP & -16) - 40
RAX = 0xbbbb000011113333


def registers(pc=PC, sp=RSP, rax=RAX, cpu=3, idt=IDT, limit=0x2ff,
              flags=2, rights=0xaf9b):
    return (f"CPU#{cpu}\r\nRAX={rax:016x} RBX=0000000000000000\r\n"
            f"RSP={sp:016x}\r\nRIP={pc:016x} RFL={flags:08x} [-------] CPL=0\r\n"
            f"CS =0010 0000000000000000 ffffffff {rights << 8:08x} DPL=0 CS64 [-RA]\r\n"
            "SS =0018 0000000000000000 ffffffff 00cf9300 DPL=0 DS [-WA]\r\n"
            f"IDT=     {idt:016x} {limit:08x}\r\nCR4=00000020\r\nEFER=0000000000000500\r\n")


def gate(address):
    return struct.pack("<HHBBHII", address & 0xffff, 0x10, 0, 0x8e,
                       (address >> 16) & 0xffff, address >> 32, 0)


class GdbXbeginCaptureTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.memory = {
            PC: b"\xc7\xf8" + struct.pack("<i", -0x16),
            IDT: b"".join(gate(HANDLER if n == 6 else OTHER) for n in range(32)),
            FRAME_SP: struct.pack("<5Q", PC, 0x10, 0x10002, RSP, 0x18),
        }

    def read(self, address, size):
        self.calls.append((address, size))
        data = self.memory[address]
        self.assertEqual(len(data), size)
        return data

    def begin(self):
        return XbeginObservation.begin(registers(), self.read)

    def test_fault_transports_exact_registers_bytes_and_frame_at_their_own_addresses(self):
        observation = self.begin()
        self.assertEqual(observation.stops, {PC + 6, PC - 0x10, HANDLER, OTHER})
        record, _ = observation.finish(registers(HANDLER, FRAME_SP, rights=0xa09b),
                                       self.read, HANDLER, PC & ~0xffff)
        bank = {"rip": hex(PC), "rsp": hex(RSP), "rax": hex(RAX), "eflags": "0x2",
                "cs": "0x10", "ss": "0x18", "cs_ar": "0xaf9b",
                "cpu": 3, "cr4": "0x20", "efer": "0x500"}
        self.assertEqual(record, {"qemu_xbegin_fault_v1": {
            "before": bank, "after": {**bank, "rip": hex(HANDLER), "rsp": hex(FRAME_SP),
                                      "cs_ar": "0xa09b"},
            "bytes": self.memory[PC].hex(), "exception_idt": self.memory[IDT].hex(),
            "frame": self.memory[FRAME_SP].hex(), "load_base": hex(PC & ~0xffff)}})
        self.assertEqual(self.calls, [(PC, 6), (IDT, 512), (PC, 6), (IDT, 512), (FRAME_SP, 40)])

    def test_abort_fallthrough_and_other_landing_cannot_publish_a_fault_record(self):
        observation = self.begin()
        for pc, text in [(PC - 0x10, "EAX=0x78000001"), (PC + 6, "fallthrough"), (OTHER, "outside")]:
            with self.subTest(pc=pc):
                record, message = observation.finish(registers(pc, rax=0x78000001), self.read, pc)
                self.assertIsNone(record)
                self.assertIn(text, message)
        self.memory[PC] = b"\xc7\xf8\0\0\0\0"
        record, message = self.begin().finish(registers(PC + 6), self.read, PC + 6)
        self.assertIsNone(record)
        self.assertIn("undetermined", message)
        self.assertNotIn((FRAME_SP, 40), self.calls)

    def test_a_stale_monitor_rip_cannot_bind_a_different_landing(self):
        observation = self.begin()
        with self.assertRaisesRegex(ValueError, "disagrees with the GDB hardware breakpoint"):
            observation.finish(registers(HANDLER, FRAME_SP), self.read, observation.fallback)
        self.assertEqual(self.calls, [(PC, 6), (IDT, 512)])

    def test_capture_rejects_changed_vcpu_idtr_instruction_and_gates(self):
        observation = self.begin()
        for change in ({"cpu": 4}, {"idt": IDT + 8}, {"limit": 0x1ff}):
            with self.subTest(change=change), self.assertRaisesRegex(ValueError, "vCPU or IDTR"):
                observation.finish(registers(HANDLER, **change), self.read, HANDLER)
        for address in (PC, IDT):
            original = self.memory[address]
            self.memory[address] = original[:-1] + b"\x81"
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, "changed between stops"):
                observation.finish(registers(HANDLER), self.read, HANDLER)
            self.memory[address] = original

    def test_initial_stop_must_allow_an_isolated_64_bit_instruction(self):
        samples = [(registers() + registers(cpu=4), "one QEMU"),
                   (registers().replace("RIP=", "EIP="), "one QEMU"),
                   (registers(rights=0xc09b), "64-bit ring zero"),
                   (registers(flags=0x102), "IF and TF"), (registers(flags=0x202), "IF and TF"),
                   (registers(limit=0x1fe), "IDT limit")]
        for text, error in samples:
            with self.subTest(error=error), self.assertRaisesRegex(ValueError, error):
                XbeginObservation.begin(text, self.read)
        self.memory[PC] = bytes.fromhex("909090909090")
        with self.assertRaisesRegex(ValueError, "XBEGIN rel32"):
            self.begin()

    def test_all_possible_exits_must_have_unambiguous_hardware_stops(self):
        cases = [(6, bytes(16), "dedicated"), (9, gate(HANDLER), "dedicated"),
                 (3, gate(OTHER + 8), "hardware stops"), (7, gate(PC), "initial RIP"),
                 (6, gate(PC + 6), "aliases"), (2, gate(PC - 0x10), "aliases")]
        original = self.memory[IDT]
        for index, new_gate, error in cases:
            with self.subTest(index=index, error=error):
                start = index * 16
                self.memory[IDT] = original[:start] + new_gate + original[start + 16:]
                with self.assertRaisesRegex(ValueError, error):
                    self.begin()


if __name__ == "__main__":
    unittest.main()
