"""Q35 main-RAM constraints over authored wire bytes; no guest/device execution."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts.qemu_save_memory import QemuPhysicalMemory
from scripts.qemu_save_q35 import PROFILE, check_main_ram, main
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError
from scripts.tests.test_qemu_save_memory import region
from scripts.tests.test_qemu_save_snapshot import Fixture, be, field, page, section, string


class Q35Fixture(Fixture):
    def __init__(self):
        super().__init__()
        self.machine = "pc-q35-10.2"
        self.ram_size = 32 << 20
        self.config = bytearray(256)
        self.config[:4] = b"\x86\x80\xc0\x29"
        self.config[0x50:0x52] = b"\x04\0"
        self.config[0x60:0x68] = bytes.fromhex("010000e000000000")
        self.config[0x9c:0x9f] = b"\x02\x1a\x3f"
        self.pci_version = 2
        self.mch = {
            "name": "pci/mch", "instance_id": 7, "vmsd_name": "mch", "version": 1,
            "fields": [field("parent_obj", "struct", 276, struct={
                "vmsd_name": "PCIDevice", "version": 2,
                "fields": [field("version_id", "int32 le", 4),
                           field("config[0]", "pci config", 256),
                           field("irq_state", "pci irq state", 16)],
            }), field("unused", "unused_buffer", 1)],
        }
        self.description["devices"].append(self.mch)

    def save(self, **kwargs):
        self.description["configuration"]["fields"][1]["size"] = len(self.machine)
        return super().save(**kwargs)

    def stream(self):
        self.inventory = (be((self.ram_size + 4096) | 4, 8) + string("pc.ram") +
                          be(self.ram_size, 8) + string("/rom@data") + be(4096, 8))
        self.first = page(0x100000, 0x11, "pc.ram") + page(0, 0x44, "/rom@data")
        self.last = page(0x1000000, 0x33, "pc.ram")
        data = super().stream().replace(be(6, 4) + b"pc-q35",
                                        be(len(self.machine), 4) + self.machine.encode(), 1)
        payload = be(self.pci_version, 4) + self.config + bytes(17)
        return data[:-1] + section(4, 93, payload, "pci/mch", instance=7,
                                  version=self.mch["version"]) + b"\0"


class QemuSaveQ35Tests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "input.save"
        self.map_path = Path(self.directory.name) / "map.json"
        self.fixture = Q35Fixture()
        self.mapping = {
            "schema": 1, "machine": self.fixture.machine,
            "ram_blocks": {"pc.ram": self.fixture.ram_size},
            "regions": [region(0x100000, 4096, 0x100000),
                        region(0x100000000, 4096, 0x1000000)],
        }
        self.bindings = {
            "profile": PROFILE, "mch_name": "pci/mch", "mch_instance": 7,
            "ram_block": "pc.ram", "below_4g": 0x1000000,
            "above_4g_start": 0x100000000, "smm_ranges": True, "extended_tseg_mib": 4,
        }

    def open(self):
        # Each open reader keeps an immutable save, including subtest loops.
        with tempfile.NamedTemporaryFile(dir=self.directory.name, delete=False) as source:
            source.write(self.fixture.save())
        snapshot = QemuSaveSnapshot(source.name)
        self.addCleanup(snapshot.close)
        return snapshot

    def check(self, snapshot, mapping=None, **bindings):
        memory = QemuPhysicalMemory(snapshot, self.mapping if mapping is None else mapping)
        return check_main_ram(snapshot, memory, **(self.bindings | bindings))

    def one_byte(self, address):
        offset = address if address < 0x100000000 else 0x1000000 + address - 0x100000000
        return self.mapping | {"regions": [region(address, 1, offset)]}

    def cli(self):
        return [str(self.path), "--map", str(self.map_path), "--profile", PROFILE,
                "--mch", "pci/mch", "7", "--ram-block", "pc.ram",
                "--below-4g", "0x1000000", "--above-4g-start", "0x100000000",
                "--smm-ranges", "on", "--extended-tseg-mib", "4"]

    def test_check_preserves_map_and_reads_without_rechecking_or_observing_ram(self):
        snapshot = self.open()
        memory = QemuPhysicalMemory(snapshot, self.mapping)
        original = memory.summary()
        with patch.object(snapshot, "read_ram", side_effect=AssertionError("RAM observed")):
            result = check_main_ram(snapshot, memory, **self.bindings)
        self.assertEqual(result["scope"], "main-ram-mch-consistency")
        self.assertEqual(result["mch"]["instance_id"], 7)
        self.assertEqual(result["mch"]["section_id"], 93)
        self.assertEqual(result["tseg_bytes"], "0x400000")
        self.assertEqual(result["checked_regions"], 2)
        result["bindings"]["below_4g"] = "0x0"
        with patch.object(snapshot, "read_device", side_effect=AssertionError("device rechecked")):
            self.assertEqual(memory.read_physical(0x100000, 4), b"\x11" * 4)
            self.assertEqual(memory.read_physical(0x100000000, 4), b"\x33" * 4)
        self.assertEqual(memory.summary(), original)

    def test_tseg_modes_reject_first_byte_and_crossing_but_keep_previous_byte(self):
        for control, size in ((0x39, 1 << 20), (0x3b, 2 << 20),
                              (0x3d, 8 << 20), (0x3f, 4 << 20)):
            with self.subTest(control=control):
                self.fixture.config[0x9e] = control
                snapshot = self.open()
                first = 0x1000000 - size
                self.check(snapshot, self.one_byte(first - 1))
                for address, length in ((first, 1), (0xffffff, 1), (first - 1, 2)):
                    mapping = self.mapping | {"regions": [region(address, length, address)]}
                    with self.assertRaisesRegex(SnapshotError, "overlaps Q35 tseg"):
                        self.check(snapshot, mapping)

    def test_tseg_requires_both_saved_enable_bits_and_bound_smm_ranges(self):
        for smram, esmramc, enabled, extended in ((0x12, 0x3f, True, 4),
                                                (0x1a, 0x3e, True, 4),
                                                (0x1a, 0x3f, False, 4),
                                                (0x1a, 0x3f, True, 0)):
            with self.subTest(smram=smram, esmramc=esmramc, enabled=enabled, extended=extended):
                self.fixture.config[0x9d:0x9f] = bytes([smram, esmramc])
                result = self.check(self.open(), self.one_byte(0xffffff),
                                    smm_ranges=enabled, extended_tseg_mib=extended)
                self.assertEqual(result["tseg_bytes"], "0x0")

    def test_extended_tseg_uses_machine_property_not_guest_writable_query_register(self):
        for query in (0, 1, 0xffff, 0x1234):
            with self.subTest(query=query):
                self.fixture.config[0x50:0x52] = query.to_bytes(2, "little")
                snapshot = self.open()
                self.assertEqual(self.check(snapshot)["tseg_bytes"], "0x400000")
                with self.assertRaisesRegex(SnapshotError, "overlaps Q35 tseg"):
                    self.check(snapshot, self.one_byte(0xc00000))

    def test_legacy_first_mib_is_not_certified_by_ram_inventory_or_pam_bits(self):
        self.fixture.config[0x90:0x97] = b"\x33" * 7
        self.fixture.config[0x9c:0x9f] = b"\0\x4a\x38"
        snapshot = self.open()
        for address in (0, 0x30000, 0x9ffff, 0xa0000, 0xf0000, 0xfffff):
            with self.subTest(address=address), self.assertRaisesRegex(SnapshotError, "legacy-first-mib"):
                self.check(snapshot, self.one_byte(address))
        self.check(snapshot, self.one_byte(0x100000))

    def test_ecam_uses_qemu_profile_masks_lengths_and_enable_bit(self):
        self.fixture.ram_size = 512 << 20
        self.mapping["ram_blocks"]["pc.ram"] = self.fixture.ram_size
        for register, start, size in ((0x107000001, 0x100000000, 0x10000000),
                                      (0x107000003, 0x106000000, 0x08000000),
                                      (0x107000005, 0x102000000, 0x04000000)):
            with self.subTest(register=register):
                self.fixture.config[0x60:0x68] = register.to_bytes(8, "little")
                snapshot = self.open()
                for address in (start, start + size - 1):
                    with self.assertRaisesRegex(SnapshotError, "pcie-ecam"):
                        self.check(snapshot, self.one_byte(address))
                if start > 0x100000000:
                    self.check(snapshot, self.one_byte(start - 1))
                self.check(snapshot, self.one_byte(start + size))
                mapping = self.one_byte(start + size - 1)
                mapping["regions"][0]["size"] = 2
                with self.assertRaisesRegex(SnapshotError, "pcie-ecam"):
                    self.check(snapshot, mapping)
                self.fixture.config[0x60:0x68] = (register - 1).to_bytes(8, "little")
                self.check(self.open(), self.one_byte(start))

    def test_reserved_ecam_length_retains_unknown_history_even_when_disabled(self):
        for register in (0xe0000006, 0xe0000007):
            with self.subTest(register=register):
                self.fixture.config[0x60:0x68] = register.to_bytes(8, "little")
                with self.assertRaisesRegex(SnapshotError, "mapping history unknown"):
                    self.check(self.open(), self.mapping | {"regions": []})

    def test_mapping_rejects_wrong_offsets_other_blocks_gap_and_unbound_high_base(self):
        snapshot = self.open()
        cases = [([region(0x100000, 1, 0)], "wrong original"),
                 ([region(0x100000000, 1, 0)], "wrong original"),
                 ([region(0x1000000, 1, 0x1000000)], "outside bound"),
                 ([region(0x200000000, 1, 0x1000000)], "outside bound"),
                 ([region(0xffffff, 2, 0)], "outside bound"),
                 ([region(0x100000, 1, 0, "/rom@data")], "different RAM block")]
        for rows, message in cases:
            mapping = self.mapping | {"ram_blocks": self.mapping["ram_blocks"] | {"/rom@data": 4096},
                                      "regions": rows}
            with self.subTest(rows=rows), self.assertRaisesRegex(SnapshotError, message):
                self.check(snapshot, mapping)
        high_base = 1 << 40
        mapping = self.mapping | {"regions": [region(high_base + 7, 1, 0x1000007)]}
        self.check(snapshot, mapping, above_4g_start=high_base)

    def test_profile_identity_versions_and_required_mch_bytes(self):
        snapshot = self.open()
        for bindings, message in (({"profile": "guessed"}, "profile/machine"),
                                   ({"mch_name": "mch"}, "absent"),
                                   ({"mch_instance": 93}, "absent"),
                                   ({"mch_instance": True}, "device instance")):
            with self.subTest(bindings=bindings), self.assertRaisesRegex(SnapshotError, message):
                self.check(snapshot, **bindings)
        self.fixture.mch["version"] = 2
        with self.assertRaisesRegex(SnapshotError, "layout version"):
            self.check(self.open())
        self.fixture.mch["version"] = 1
        self.fixture.pci_version = 1
        with self.assertRaisesRegex(SnapshotError, "layout version"):
            self.check(self.open())
        self.fixture.pci_version = 2
        self.fixture.mch["fields"][0]["struct"]["fields"][1]["name"] = "unknown_config"
        with self.assertRaisesRegex(SnapshotError, "PCI configuration"):
            self.check(self.open())
        self.fixture.mch["fields"][0]["struct"]["fields"][1]["name"] = "config[0]"
        self.fixture.config[0] = 0
        with self.assertRaisesRegex(SnapshotError, "PCI configuration"):
            self.check(self.open())
        self.fixture.machine = "pc-q35-10.1"
        mapping = self.mapping | {"machine": self.fixture.machine}
        with self.assertRaisesRegex(SnapshotError, "profile/machine"):
            self.check(self.open(), mapping)

    def test_machine_bindings_validate_extents_types_and_tseg_size(self):
        snapshot = self.open()
        for bindings in ({"below_4g": True}, {"below_4g": 0}, {"below_4g": 0x100001},
                         {"below_4g": 0xc0000000}, {"below_4g": 0x4000000},
                         {"above_4g_start": False}, {"above_4g_start": 0},
                         {"above_4g_start": 0x100000001}, {"above_4g_start": (1 << 64) - 4096},
                         {"smm_ranges": 1}, {"extended_tseg_mib": True},
                         {"extended_tseg_mib": -1}, {"extended_tseg_mib": 0x1000},
                         {"ram_block": "missing"}):
            with self.subTest(bindings=bindings), self.assertRaises(SnapshotError):
                self.check(snapshot, **bindings)
        with self.assertRaisesRegex(SnapshotError, "TSEG exceeds"):
            self.check(snapshot, extended_tseg_mib=17)

    def test_map_and_device_evidence_cannot_come_from_different_readers(self):
        snapshot = self.open()
        other = self.open()
        memory = QemuPhysicalMemory(other, self.mapping)
        with self.assertRaisesRegex(SnapshotError, "same snapshot"):
            check_main_ram(snapshot, memory, **self.bindings)

    def test_cli_checks_entire_map_before_optional_read_and_publishes_no_partial_result(self):
        self.path.write_bytes(self.fixture.save())
        self.map_path.write_text(json.dumps(self.mapping))
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(self.cli()), 0)
        self.assertNotIn("read", json.loads(output.getvalue()))
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(self.cli() + ["--physical", "0x100000000", "4"]), 0)
        self.assertEqual(json.loads(output.getvalue())["read"]["hex"], "33333333")
        invalid = deepcopy(self.mapping)
        invalid["regions"].append(region(0xc00000, 1, 0xc00000))
        self.map_path.write_text(json.dumps(invalid))
        output, error = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(error):
            self.assertEqual(main(self.cli() + ["--physical", "0x100000000", "4"]), 2)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("tseg", error.getvalue())

    def test_cli_missing_saved_page_and_bad_instance_do_not_publish_success(self):
        self.path.write_bytes(self.fixture.save())
        self.map_path.write_text(json.dumps(self.mapping))
        missing_page = self.mapping | {"regions": [region(0x101000, 1, 0x101000)]}
        for mapping, suffix, message in ((missing_page, ["--physical", "0x101000", "1"], "absent"),
                                         (self.mapping, ["--mch", "pci/mch", "bad"], "MCH instance")):
            self.map_path.write_text(json.dumps(mapping))
            output, error = io.StringIO(), io.StringIO()
            with self.subTest(message=message), redirect_stdout(output), redirect_stderr(error):
                self.assertEqual(main(self.cli() + suffix), 2)
            self.assertEqual(output.getvalue(), "")
            self.assertIn(message, error.getvalue())


if __name__ == "__main__":
    unittest.main()
