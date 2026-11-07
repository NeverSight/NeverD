"""Explicit physical-map observations over authored save bytes; no execution."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest

from scripts.qemu_save_memory import QemuPhysicalMemory, main
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, SnapshotLimits
from scripts.tests.test_qemu_save_snapshot import Fixture


def region(address, size, offset=0, block="pc.ram"):
    return {"physical_start": address, "size": size,
            "ram_block": block, "block_offset": offset}


class QemuPhysicalMemoryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "input.save"
        self.map_path = Path(self.directory.name) / "map.json"
        self.fixture = Fixture()
        self.path.write_bytes(self.fixture.save())
        self.description = {
            "schema": 1, "machine": "pc-q35",
            "ram_blocks": {"pc.ram": "0x5000", "/rom@data": 4096},
            "regions": [region("0x100000000", "0x4000"),
                        region(0x1000, 0x5000),
                        region(0xa0000, 4096, block="/rom@data")],
        }

    def open(self, limits=SnapshotLimits()):
        snapshot = QemuSaveSnapshot(self.path, limits)
        self.addCleanup(snapshot.close)
        return snapshot

    def test_aliases_keep_block_offsets_latest_records_and_zero_fills(self):
        snapshot = self.open()
        memory = QemuPhysicalMemory(snapshot, self.description)
        self.assertEqual(memory.read_physical(0x1000, 4), b"\xff\xfe\xfd\xfc")
        self.assertEqual(memory.read_physical(0x1ffe, 6), b"\x01\0\x80\x80\x80\x80")
        self.assertEqual(memory.read_physical(0x100000ffe, 6), b"\x01\0\x80\x80\x80\x80")
        self.assertEqual(memory.read_physical(0x3000, 4), b"\0" * 4)
        self.assertEqual(memory.read_physical(0x100002000, 4), b"\0" * 4)
        self.assertEqual(memory.read_physical(0x4000, 4), b"\xff" * 4)
        self.assertEqual(memory.read_physical(0xa0000, 4), b"\x44" * 4)
        self.assertEqual(self.path.read_bytes(), self.fixture.save())

    def test_unaligned_adjacent_mappings_can_join_different_blocks(self):
        self.description["regions"] = [region(0x100000009, 2, 8192),
                                       region(0x100000001, 3, 4093),
                                       region(0x100000004, 5, 17, "/rom@data")]
        memory = QemuPhysicalMemory(self.open(), self.description)
        self.assertEqual(memory.read_physical(0x100000001, 10), b"\x02\x01\0" + b"D" * 5 + b"\0\0")
        self.assertEqual(memory.read_physical(0x100000002, 8), b"\x01\0" + b"D" * 5 + b"\0")

    def test_holes_do_not_read_underlying_ram_or_skip_to_the_next_mapping(self):
        self.description["regions"] = [region(0x1000, 4), region(0x1008, 4, 8)]
        memory = QemuPhysicalMemory(self.open(), self.description)
        for address, size in ((0, 1), (0xfff, 2), (0x1002, 8), (0x1004, 1), (0x100b, 2)):
            with self.subTest(address=address, size=size), self.assertRaisesRegex(
                    SnapshotError, "unmapped physical address"):
                memory.read_physical(address, size)
        self.assertEqual(memory.read_physical(0x1008, 4), b"\xf7\xf6\xf5\xf4")

    def test_declaring_a_region_does_not_supply_missing_pages(self):
        memory = QemuPhysicalMemory(self.open(), self.description)
        for address, size in ((0x5000, 1), (0x4fff, 2)):
            with self.subTest(address=address), self.assertRaisesRegex(SnapshotError, "page.*absent"):
                memory.read_physical(address, size)
        self.assertEqual(memory.read_physical(0x4fff, 1), b"\xff")

    def test_overlaps_require_flattening_even_when_the_backing_agrees(self):
        snapshot = self.open()
        for second in (region(0x1000, 4), region(0x1002, 4, 2),
                       region(0x1003, 4, 0, "/rom@data")):
            with self.subTest(second=second), self.assertRaisesRegex(SnapshotError, "overlapping"):
                QemuPhysicalMemory(snapshot, {**self.description,
                                             "regions": [region(0x1000, 4), second]})

    def test_machine_and_block_bindings_are_required_and_checked(self):
        snapshot = self.open()
        for field, value, message in (
                ("machine", "another-machine", "machine mismatch"),
                ("ram_blocks", {"pc.ram": 8192}, "binding mismatch"),
                ("ram_blocks", {"unknown": 4096}, "binding mismatch"),
                ("ram_blocks", {}, "no size binding")):
            with self.subTest(field=field, value=value), self.assertRaisesRegex(SnapshotError, message):
                QemuPhysicalMemory(snapshot, {**self.description, field: value})

    def test_schema_rejects_unknown_keys_missing_keys_and_bad_shapes(self):
        snapshot = self.open()
        invalid = [None, [], {**self.description, "schema": True},
                   {**self.description, "schema": 2}, {**self.description, "extra": 0},
                   {key: value for key, value in self.description.items() if key != "regions"},
                   {**self.description, "regions": {}}, {**self.description, "ram_blocks": []},
                   {**self.description, "regions": [None]},
                   {**self.description, "regions": [{**region(0, 1), "priority": 1}]},
                   {**self.description, "regions": [{"physical_start": 0}]}]
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(SnapshotError):
                QemuPhysicalMemory(snapshot, value)

    def test_region_integer_name_and_extent_validation(self):
        snapshot = self.open()
        for field in ("physical_start", "size", "block_offset"):
            for value in (-1, True, 1.0, None, 1 << 64, "10", "-0x1", "0x10000000000000000"):
                bad = {**region(0, 1), field: value}
                with self.subTest(field=field, value=value), self.assertRaises(SnapshotError):
                    QemuPhysicalMemory(snapshot, {**self.description, "regions": [bad]})
        for bad in (region(0, 0), region(0xffffffffffffffff, 2),
                    region(0, 2, 0x4fff), region(0, 1, 0x5001),
                    region(0, 1, block=True), region(0, 1, block=""),
                    region(0, 1, block="pc.ram\0")):
            with self.subTest(bad=bad), self.assertRaises(SnapshotError):
                QemuPhysicalMemory(snapshot, {**self.description, "regions": [bad]})

    def test_final_uint64_byte_and_empty_reads_have_explicit_boundaries(self):
        self.description["regions"] = [region(0xffffffffffffffff, 1, 0x1000)]
        memory = QemuPhysicalMemory(self.open(), self.description)
        self.assertEqual(memory.read_physical(0xffffffffffffffff, 1), b"\x80")
        self.assertEqual(memory.read_physical(0xffffffffffffffff, 0), b"")
        self.assertEqual(memory.read_physical(0, 0), b"")
        with self.assertRaisesRegex(SnapshotError, "overflows"):
            memory.read_physical(0xffffffffffffffff, 2)
        for address, size in ((-1, 0), (1 << 64, 0), (True, 1), (0.0, 1),
                              ("0x0", 1), (0, -1), (0, True), (0, 1.0)):
            with self.subTest(address=address, size=size), self.assertRaises(SnapshotError):
                memory.read_physical(address, size)

    def test_limits_cover_total_read_size_metadata_and_region_count(self):
        snapshot = self.open(SnapshotLimits(read_bytes=4, physical_regions=2, metadata_bytes=4096))
        with self.assertRaisesRegex(SnapshotError, "region count"):
            QemuPhysicalMemory(snapshot, self.description)
        self.description["regions"] = [region(0, 4), region(4, 4, 0x1000)]
        memory = QemuPhysicalMemory(snapshot, self.description)
        self.assertEqual(memory.read_physical(2, 4), b"\xfd\xfc\x80\x80")
        with self.assertRaisesRegex(SnapshotError, "read size"):
            memory.read_physical(2, 5)
        self.map_path.write_bytes(b" " * 4097)
        with self.assertRaisesRegex(SnapshotError, "JSON exceeds"):
            QemuPhysicalMemory.from_file(snapshot, self.map_path)

    def test_empty_map_has_no_implicit_identity_mapping(self):
        memory = QemuPhysicalMemory(self.open(), {**self.description, "ram_blocks": {}, "regions": []})
        self.assertEqual(memory.read_physical(0, 0), b"")
        with self.assertRaisesRegex(SnapshotError, "unmapped"):
            memory.read_physical(0, 1)

    def test_map_is_detached_from_caller_mutation_and_normalized_json_round_trips(self):
        snapshot = self.open()
        memory = QemuPhysicalMemory(snapshot, self.description)
        self.description["regions"][1]["block_offset"] = 8192
        self.description["ram_blocks"]["pc.ram"] = 1
        summary = memory.summary()
        self.assertEqual(summary["regions"][0], region("0x1000", "0x5000", "0x0"))
        self.map_path.write_text(json.dumps(summary))
        other = QemuPhysicalMemory.from_file(snapshot, self.map_path)
        self.assertEqual(other.summary(), memory.summary())
        summary["regions"].clear()
        self.assertEqual(memory.read_physical(0x1000, 4), b"\xff\xfe\xfd\xfc")
        self.assertEqual(other.read_physical(0x1000, 4), b"\xff\xfe\xfd\xfc")

    def test_invalid_json_duplicate_keys_unicode_and_numeric_overflow_fail(self):
        snapshot = self.open()
        valid = json.dumps(self.description).encode()
        for data in (b"{", b"\xff", valid.replace(b'"schema": 1', b'"schema": 1, "schema": 1'),
                     valid.replace(b'"size": 4096', b'"size": 4096, "size": 4096'),
                     valid.replace(b'"schema": 1', b'"schema": NaN'),
                     b"[" * 2000 + b"]" * 2000, b"1" * 10000):
            with self.subTest(data=data[:80]), self.assertRaisesRegex(SnapshotError, "physical map"):
                self.map_path.write_bytes(data)
                QemuPhysicalMemory.from_file(snapshot, self.map_path)

    def test_cli_publishes_a_complete_read_and_mapping_or_no_output(self):
        self.map_path.write_text(json.dumps(self.description))
        args = [str(self.path), "--map", str(self.map_path), "--physical"]
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main([*args, "0x1ffe", "6"]), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result["read"], {"physical_address": "0x1ffe", "size": 6,
                                          "hex": "010080808080"})
        self.assertEqual(result["mapping"]["ram_blocks"]["pc.ram"], "0x5000")
        for address, size in (("0xfff", "2"), ("0x4fff", "2"), ("0x20000", "1"),
                              ("0xffffffffffffffff", "2")):
            output, errors = io.StringIO(), io.StringIO()
            with self.subTest(address=address), redirect_stdout(output), redirect_stderr(errors):
                self.assertEqual(main([*args, address, size]), 2)
                self.assertEqual(output.getvalue(), "")
                self.assertIn("qemu-save-memory:", errors.getvalue())
        broken = deepcopy(self.description)
        broken["regions"].append(region(0x1000, 1))
        self.map_path.write_text(json.dumps(broken))
        output, errors = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(errors):
            self.assertEqual(main([*args, "0x1000", "1"]), 2)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("overlapping", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
