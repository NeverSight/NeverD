"""Select and stage only the original XBEGIN. These checks do not start a VM."""

from copy import deepcopy
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from scripts import qemu_xbegin_probe as probe


SITE = 0xffff800132405ffd
CODE = bytes.fromhex("c7f8fa002000")


def unit(pc=SITE, code=CODE, native=True):
    return {"pc": hex(pc), "bytes": code.hex().upper(), "native_boundary": native}


def manifest(*units, **changes):
    request = {"profile": "kernel-events-v6", "paging_address_bits": 48, "mxcsr_mask": "0xffff",
               "instructions": [row["pc"] for row in units], "dispatch": "direct", **changes}
    return {"schema_version": 2, "profile": "kernel-events-v6", "paged_memory": False,
            "scheduled_events": False, "request": request, "units": list(units)}


def guest_read(ram, levels, address, size):
    """Walk each byte from CR3 using page-table indices independent of the writer."""
    result = bytearray()
    for va in range(address, address + size):
        physical = 0x1000
        page = va >> 12
        for level in reversed(range(levels)):
            index = (page >> (9 * level)) % 512
            entry = int.from_bytes(ram[physical + 8 * index:physical + 8 * (index + 1)], "little")
            if entry & 3 != 3 or entry & 0x80:
                raise AssertionError(f"unmapped or large page for {va:#x}")
            physical = (entry >> 12) << 12
        result.append(ram[physical + va % 4096])
    return bytes(result)


class QemuXbeginProbeTests(unittest.TestCase):
    def test_selects_only_the_unresolved_original_instruction(self):
        source = manifest(unit(SITE - 0x1000, native=False), unit(SITE - 0x20, b"\x90" * 6), unit())
        self.assertEqual(probe.select_site(source), ((SITE, CODE), source["request"]))
        source["units"].append(unit(SITE + 0x1000))
        with self.assertRaisesRegex(ValueError, "multiple unresolved"):
            probe.select_site(source)
        self.assertEqual(probe.select_site(source, SITE + 0x1000)[0], (SITE + 0x1000, CODE))
        with self.assertRaisesRegex(ValueError, "no unresolved"):
            probe.select_site(source, SITE - 0x1000)
        source["units"].append(unit())
        with self.assertRaisesRegex(ValueError, "unique site"):
            probe.select_site(source, SITE)

    def test_site_observation_preserves_automatic_selection_elsewhere(self):
        for source in (manifest(unit(), native_rtm_abort_steps="auto"),
                       manifest(unit(), unit(SITE - 16), native_rtm_abort_steps="auto")):
            original = deepcopy(source)
            site, request = probe.select_site(source, SITE)
            self.assertEqual(site, (SITE, CODE))
            self.assertEqual(request, original["request"])
            self.assertEqual(source, original)
        for r in (manifest(unit(), native_rtm_abort_steps={"max_basic_leaf": 7, "leaf7_ebx": 0}),
                  manifest(unit(), native_rtm_abort_steps=True),
                  manifest(unit(), native_rtm_abort_steps={}),
                  manifest(unit(), native_rtm_abort_regions=True),
                  manifest(unit(), native_rtm_regions=True),
                  manifest(unit(), qemu_xbegin_fault_v1={})):
            with self.subTest(request=r["request"]), self.assertRaisesRegex(ValueError, "binding"):
                probe.select_site(r, SITE)

    def test_incompatible_manifests_and_unsupported_bytes_stop_before_qemu(self):
        source = manifest(unit())
        for changed in ({"schema_version": 3}, {"profile": "kernel-events-v5"}, {"paged_memory": True},
                        {"scheduled_events": True}, {"units": None}, {"request": None},
                        {"request": source["request"] | {"paged_memory": True}}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                probe.select_site(source | changed)
        for data in (CODE[:-1], b"\x66" + CODE, b"\xc6\xf8" + bytes(4)):
            with self.subTest(data=data), self.assertRaisesRegex(ValueError, "no unresolved"):
                probe.select_site(manifest(unit(code=data)))
        for pc in (-1, True, 1 << 64):
            with self.subTest(pc=pc), self.assertRaisesRegex(ValueError, "unsigned"):
                probe.select_site(source | {"units": [unit() | {"pc": pc}]})

    def test_paging_preserves_cross_page_instruction_successors_and_fault_fixture(self):
        for bits, pc in ((48, SITE), (57, 0xff10000132405ffd), (48, 0x8ffd), (48, 0x1dffd)):
            with self.subTest(bits=bits, pc=pc):
                ram, rsp = probe.fixture_ram(pc, CODE, bits)
                levels = {48: 4, 57: 5}[bits]
                def read(address, size):
                    return guest_read(ram, levels, address, size)
                self.assertEqual(read(pc, 6), CODE)
                self.assertEqual(read(pc + 6, 1), b"\xf4")
                self.assertEqual(read(pc + 6 + 0x2000fa, 1), b"\xf4")
                self.assertEqual(read(rsp - 40, 40), bytes(40))
                self.assertEqual(struct.unpack("<QQ", read(0x300, 16)), (0x00af9b000000ffff, 0x00cf93000000ffff))
                handlers = []
                for vector in range(32):
                    lo, selector, ist, attr, mid, hi, zero = struct.unpack("<HHBBHII", read(vector * 16, 16))
                    self.assertEqual((selector, ist, attr, zero), (0x300, 0, 0x8e, 0))
                    handlers.append(lo | mid << 16 | hi << 32)
                self.assertEqual(len(set(handlers)), 2)
                self.assertEqual(handlers.count(handlers[6]), 1)
                self.assertTrue(all(read(target, 1) == b"\xf4" for target in set(handlers)))

    def test_negative_fallback_and_page_zero_outside_descriptors(self):
        code = b"\xc7\xf8" + struct.pack("<i", -0x76)
        ram, _ = probe.fixture_ram(0x580, code, 48)
        self.assertEqual(guest_read(ram, 4, 0x580, 6), code)
        self.assertEqual(guest_read(ram, 4, 0x510, 1), b"\xf4")
        for pc, displacement, bits in ((0x300, 7, 48), (0x2ff, 7, 48), (0x800, -0x506, 48),
                                      (0x2810, -6, 48), ((1 << 47) - 4, 0, 48),
                                      ((1 << 56) - 30, 50, 57), (0x2800, 7, 49)):
            with self.subTest(pc=pc, displacement=displacement, bits=bits), self.assertRaises(ValueError):
                probe.fixture_ram(pc, b"\xc7\xf8" + struct.pack("<i", displacement), bits)

    def test_writes_a_new_complete_request_only_after_a_fault(self):
        source = manifest(unit(), unit(SITE - 16, native=False), native_rtm_abort_steps="auto")
        original = deepcopy(source)
        record = {"frame": "fixture"}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bound request.json"
            with patch.object(probe, "collect_fault", side_effect=ValueError("abort target")):
                with self.assertRaisesRegex(ValueError, "abort target"):
                    probe.probe(source, path)
                self.assertFalse(path.exists())
            with patch.object(probe, "collect_fault", return_value=record) as capture:
                self.assertEqual(probe.probe(source, path), SITE)
                expected = source["request"] | {"qemu_xbegin_fault_v1": record}
                self.assertEqual(json.loads(path.read_text()), expected)
                with self.assertRaisesRegex(ValueError, "output already exists"):
                    probe.probe(source, path)
                capture.assert_called_once_with(SITE, CODE, 48)
            self.assertEqual(source, original)


if __name__ == "__main__":
    unittest.main()
