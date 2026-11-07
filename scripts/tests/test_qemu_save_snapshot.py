"""Independent byte fixtures for the read-only save reader; no guest execution."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
from dataclasses import replace
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, SnapshotLimits, main


PAGE = 4096
PC = 0xfffff80112345678


def be(value, size):
    return value.to_bytes(size, "big")


def string(value):
    encoded = value.encode()
    return bytes([len(encoded)]) + encoded


def field(name, kind, size, **kwargs):
    return {"name": name, "type": kind, "size": size, **kwargs}


def subsection(name, version, payload):
    return b"\x05" + string(name) + be(version, 4) + payload


def section(kind, identifier, payload, name=None, instance=0, version=1):
    result = bytes([kind]) + be(identifier, 4)
    if kind in (1, 4):
        result += string(name) + be(instance, 4) + be(version, 4)
    return result + payload + b"\x7e" + be(identifier, 4)


def page(offset, value, block=None):
    flags = (8 if isinstance(value, bytes) else 2) | (0 if block else 0x20)
    payload = value if isinstance(value, bytes) else bytes([value])
    return be(offset | flags, 8) + (string(block) if block else b"") + payload


class Fixture:
    """Authored protocol data, independent of the production parser/layouts."""

    def __init__(self):
        self.xml = b"<domain/>\0"
        self.cookie = b"<cookie/>\0\0"
        self.endian = "<"
        self.compression = 0
        self.magic = b"LibvirtQemudSave"
        self.raw = bytes(range(256)) * 16
        cpu = {
            "name": "cpu", "instance_id": 3, "version": 12,
            "fields": [
                field("env.regs", "uint64", 8, array_len=16),
                field("env.eip", "uint64", 8),
                field("signed", "int32", 4),
                field("bounded", "int32 le", 4),
                field("seg", "struct", 15, struct={
                    "vmsd_name": "segment", "version": 1,
                    "fields": [field("base", "uint64", 8)],
                    "subsections": [{"vmsd_name": "s", "version": 1, "fields": []}],
                }),
                field("fp", "tmp", 10, vmsd_name="fp_tmp", version=0,
                      fields=[field("mant", "uint64", 8), field("exp", "uint16", 2)]),
                field("opaque", "device-specific", 3),
                field("indexed", "uint8", 1, index=2),
            ],
            "subsections": [{"vmsd_name": "cpu/extra", "version": 2,
                             "fields": [field("flag", "uint16 equal", 2)]}],
        }
        self.description = {
            "page_size": PAGE,
            "configuration": {"vmsd_name": "configuration", "version": 1,
                              "fields": [field("len", "uint32", 4), field("name", "buffer", 6)]},
            "devices": [cpu, {"name": "apic", "instance_id": 3, "version": 3,
                              "fields": [field("tpr", "uint32", 4)]}],
        }
        self.cpu = (b"".join(be(0x8000000000000000 + i, 8) for i in range(16)) +
                    be(PC, 8) + b"\xff\xff\xff\xfe" + b"\x12\x34\x56\x78" +
                    be(0xffff800012340000, 8) + subsection("s", 1, b"") +
                    be(0xf123456789abcdef, 8) + be(0x4321, 2) + b"\x00\x80\xff" +
                    b"\x91" + subsection("cpu/extra", 2, b"\x12\x34"))
        self.inventory = be(6 * PAGE | 4, 8) + string("pc.ram") + be(5 * PAGE, 8)
        self.inventory += string("/rom@data") + be(PAGE, 8)
        self.first = (page(0, self.raw, "pc.ram") + page(PAGE, 0x80) +
                      page(2 * PAGE, self.raw) + page(3 * PAGE, 0xff) +
                      page(0, 0x44, "/rom@data"))
        self.last = page(2 * PAGE, 0, "pc.ram") + page(0, self.raw[::-1])
        self.command = b"\x08\x00\x0b\x00\x00"
        self.eos = be(0x10, 8)

    def stream(self):
        return (b"QEVM\0\0\0\x03\x07" + be(6, 4) + b"pc-q35" +
                section(1, 41, self.inventory + self.eos, "ram", version=4) +
                section(4, 5, self.cpu, "cpu", instance=3, version=12) +
                section(4, 9, be(0xf0, 4), "apic", instance=3, version=3) +
                self.command + section(2, 41, self.first + self.eos) +
                section(3, 41, self.last + self.eos) + b"\0")

    def save(self, stream=None, description=None):
        metadata = self.xml + self.cookie
        header = self.magic + struct.pack(self.endian + "19I", 2, len(metadata), 0,
                                         self.compression, len(self.xml), *([0] * 14))
        description = json.dumps(self.description).encode() if description is None else description
        return (header + metadata + (self.stream() if stream is None else stream) +
                b"\x06" + be(len(description), 4) + description)


class QemuSaveSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "input.save"
        self.fixture = Fixture()

    def open(self, data=None, limits=SnapshotLimits()):
        self.path.write_bytes(self.fixture.save() if data is None else data)
        snapshot = QemuSaveSnapshot(self.path, limits)
        self.addCleanup(snapshot.close)
        return snapshot

    def rejects(self, data, message, limits=SnapshotLimits()):
        self.path.write_bytes(data)
        with self.assertRaisesRegex(SnapshotError, message):
            QemuSaveSnapshot(self.path, limits)

    def test_registers_keep_full_bits_arrays_nested_fields_and_instance_identity(self):
        snapshot = self.open()
        self.assertEqual(list(snapshot.cpus), [3])  # Not section ID 5 or ordinal 0.
        self.assertEqual(snapshot.cpus[3]["subsection_versions"], {"cpu/extra": 2})
        cpu = snapshot.cpus[3]["fields"]
        self.assertEqual(cpu["env.regs"], [0x8000000000000000 + i for i in range(16)])
        self.assertEqual(cpu["env.eip"], PC)
        self.assertEqual(cpu["signed"], -2)
        self.assertEqual(cpu["bounded"], 0x12345678)
        self.assertEqual(cpu["seg"]["base"], 0xffff800012340000)
        self.assertEqual(cpu["fp"], {"mant": 0xf123456789abcdef, "exp": 0x4321})
        self.assertEqual(cpu["opaque"], b"\0\x80\xff")
        self.assertEqual(cpu["indexed[2]"], 0x91)
        self.assertEqual(cpu["subsections"]["cpu/extra"]["flag"], 0x1234)
        self.assertEqual(snapshot.apics[3]["fields"]["tpr"], 0xf0)
        snapshot.expect_pc(3, PC)

    def test_comparison_integer_tags_keep_big_endian_wire_encoding(self):
        cpu = self.fixture.description["devices"][0]
        cpu["fields"] = [field("bounded", "int32 le", 4, array_len=2),
                         field("equal", "int64 equal", 8),
                         field("high", "uint16 equal", 2),
                         field("unknown", "uint32 le", 4)]
        cpu["subsections"] = []
        # QEMU's `int32 le` means a destination-side <= check, not little endian.
        self.fixture.cpu = (b"\0\0\0\x02\x12\x34\x56\x78" + b"\xff" * 7 + b"\xfd" +
                            b"\x80\x01" + b"\x01\x02\x03\x04")
        saved = self.open().cpus[3]["fields"]
        self.assertEqual(saved["bounded"], [2, 0x12345678])
        self.assertEqual(saved["equal"], -3)
        self.assertEqual(saved["high"], 0x8001)
        self.assertEqual(saved["unknown"], b"\x01\x02\x03\x04")

    def test_big_endian_libvirt_header_cookie_and_xml(self):
        self.fixture.endian = ">"
        snapshot = self.open()
        self.assertEqual(snapshot.domain_xml, "<domain/>")
        self.assertEqual(snapshot.cookie_xml, "<cookie/>")
        self.assertFalse(snapshot.was_running)
        self.assertEqual(snapshot.machine, "pc-q35")

    def test_last_update_zero_overwrite_unsigned_fill_and_cross_page_read(self):
        snapshot = self.open()
        self.assertEqual(snapshot.read_ram("pc.ram", 0, PAGE), self.fixture.raw[::-1])
        self.assertEqual(snapshot.read_ram("pc.ram", PAGE, 6), b"\x80" * 6)
        self.assertEqual(snapshot.read_ram("pc.ram", PAGE - 3, 6), b"\x02\x01\x00\x80\x80\x80")
        self.assertEqual(snapshot.read_ram("pc.ram", 2 * PAGE, PAGE), bytes(PAGE))
        self.assertEqual(snapshot.read_ram("pc.ram", 3 * PAGE, 6), b"\xff" * 6)
        self.assertEqual(snapshot.read_ram("/rom@data", 0, 2), b"DD")
        block = snapshot.summary()["ram_blocks"]["pc.ram"]
        self.assertEqual(block, {"size": 5 * PAGE, "pages_present": 4,
                                 "page_records": 6, "index_bytes": 5 * 8})
        self.assertEqual(snapshot.read_ram("pc.ram", 5 * PAGE, 0), b"")

    def test_absent_pages_and_out_of_range_requests_never_become_zero(self):
        snapshot = self.open()
        for offset, size, message in ((4 * PAGE, 1, "absent"),
                                      (4 * PAGE - 1, 2, "absent"),
                                      (5 * PAGE, 1, "outside"),
                                      (-1, 1, "offset"), (0, -1, "size")):
            with self.subTest(offset=offset, size=size):
                with self.assertRaisesRegex(SnapshotError, message):
                    snapshot.read_ram("pc.ram", offset, size)
        with self.assertRaisesRegex(SnapshotError, "unknown RAM block"):
            snapshot.read_ram("not-a-physical-address", 0, 1)
        with self.assertRaisesRegex(SnapshotError, "size"):
            snapshot.read_ram("pc.ram", 0, snapshot.limits.read_bytes + 1)

    def test_input_unchanged_and_pc_mismatch_does_not_retarget_state(self):
        data = self.fixture.save()
        snapshot = self.open(data)
        with self.assertRaisesRegex(SnapshotError, "PC mismatch"):
            snapshot.expect_pc(3, PC + 1)
        with self.assertRaisesRegex(SnapshotError, "absent"):
            snapshot.expect_pc(0, PC)
        self.assertEqual(snapshot.cpus[3]["fields"]["env.eip"], PC)
        self.assertEqual(self.path.read_bytes(), data)
        self.assertEqual(list(Path(self.directory.name).iterdir()), [self.path])

    def test_cli_requires_exact_pc_before_emitting_data(self):
        self.path.write_bytes(self.fixture.save())
        output, error = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(error):
            self.assertEqual(main([str(self.path), "--cpu", "3", "--expect-pc", hex(PC + 1),
                                   "--ram", "pc.ram", "0", "4"]), 2)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("PC mismatch", error.getvalue())
        with redirect_stdout(output), redirect_stderr(error):
            self.assertEqual(main([str(self.path), "--cpu", "3", "--expect-pc", hex(PC),
                                   "--ram", "pc.ram", "0", "4"]), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result["cpus"]["3"]["fields"]["env.eip"], hex(PC))
        self.assertEqual(result["read"]["hex"], "fffefdfc")

    def test_headers_truncation_and_trailer_must_be_complete(self):
        data = self.fixture.save()
        self.rejects(data[:40], "truncated")
        self.rejects(data[:-1], "trailer length")
        self.fixture.magic = b"LibvirtQemudPart"
        self.rejects(self.fixture.save(), "completed")
        self.fixture.magic = b"LibvirtQemudSave"
        self.fixture.compression = 1
        self.rejects(self.fixture.save(), "compressed")
        self.fixture.compression = 0
        self.rejects(self.fixture.save(stream=self.fixture.stream()[:-2] + b"\0"), "truncated|footer")

    def test_unknown_commands_flags_and_bad_section_footer_fail(self):
        self.fixture.command = b"\x08\x00\x04\x00\x00"
        self.rejects(self.fixture.save(), "command")
        self.fixture.command = b""
        self.fixture.first = be(0x40, 8)
        self.rejects(self.fixture.save(), "RAM flags")
        self.fixture.first = page(0, 0x80, "pc.ram")
        stream = self.fixture.stream().replace(b"\x7e\0\0\0\x29", b"\x7e\0\0\0\x28", 1)
        self.rejects(self.fixture.save(stream=stream), "footer")

    def test_ram_extent_duplicate_inventory_and_missing_continue_owner_fail(self):
        self.fixture.first = page(5 * PAGE, 0x80, "pc.ram")
        self.rejects(self.fixture.save(), "outside")
        self.fixture.first = page(0, 0x80)
        self.rejects(self.fixture.save(), "missing RAM block")
        self.fixture.first = self.fixture.inventory
        self.rejects(self.fixture.save(), "inventory")
        self.fixture.inventory = be(PAGE | 4, 8) + string("pc.ram") + be(2 * PAGE, 8)
        self.rejects(self.fixture.save(), "extent")

    def test_schema_section_versions_subsections_and_array_bounds(self):
        self.fixture.description["devices"][0]["version"] = 11
        self.rejects(self.fixture.save(), "device section")
        self.fixture.description["devices"][0]["version"] = 12
        self.fixture.cpu = self.fixture.cpu.replace(subsection("cpu/extra", 2, b"\x12\x34"),
                                                    subsection("cpu/extra", 1, b"\x12\x34"))
        self.rejects(self.fixture.save(), "subsection header")
        self.fixture.description["devices"][0]["fields"][0]["array_len"] = 1_000_001
        self.rejects(self.fixture.save(), "array length")

    def test_malformed_metadata_and_resource_limits(self):
        for description, message in ((b'{"page_size":4096,"page_size":4096}', "duplicate JSON"),
                                     (b'[]', "document"), (b'{', "JSON"),
                                     (b'{"value":NaN}', "non-finite JSON")):
            with self.subTest(description=description):
                self.rejects(self.fixture.save(description=description), message)
        self.rejects(self.fixture.save(), "oversized RAM",
                     replace(SnapshotLimits(), ram_bytes=5 * PAGE))
        self.rejects(self.fixture.save(), "VMState exceeds|array length|device states",
                     replace(SnapshotLimits(), state_values=20))
        self.rejects(self.fixture.save(), "nesting", replace(SnapshotLimits(), nesting=1))
        self.fixture.description["devices"][0]["fields"][0]["size"] = 4
        self.rejects(self.fixture.save(), "integer width")

    def test_description_length_can_contain_the_trailer_marker(self):
        description = json.dumps(self.fixture.description).encode()
        description += b" " * (0x60000 - len(description))
        snapshot = self.open(self.fixture.save(description=description))
        snapshot.expect_pc(3, PC)

    def test_invalid_unicode_metadata_is_a_boundary_error(self):
        self.fixture.description["devices"][0]["fields"][0]["name"] = "\ud800"
        self.rejects(self.fixture.save(), "UTF-8 field name")

    def test_device_byte_bound_applies_across_cpu_instances(self):
        self.fixture.description["devices"][0]["fields"].append(field("pad", "buffer", 8000))
        suffix = subsection("cpu/extra", 2, b"\x12\x34")
        self.fixture.cpu = self.fixture.cpu[:-len(suffix)] + bytes(8000) + suffix
        second = deepcopy(self.fixture.description["devices"][0])
        second["instance_id"] = 4
        self.fixture.description["devices"].append(second)
        stream = self.fixture.stream()[:-1] + section(4, 7, self.fixture.cpu, "cpu", 4, 12) + b"\0"
        data = self.fixture.save(stream=stream)
        self.assertEqual(list(self.open(data).cpus), [3, 4])
        self.rejects(data, "device states", replace(SnapshotLimits(), metadata_bytes=12000))

    def test_unknown_machine_extensions_and_unclosed_ram_section_fail(self):
        stream = self.fixture.stream().replace(b"\x03\0\0\0\x29", b"\x02\0\0\0\x29")
        self.rejects(self.fixture.save(stream=stream), "unfinished")
        self.fixture.description["configuration"]["subsections"] = [
            {"vmsd_name": "configuration/capabilities", "version": 1, "fields": []}]
        self.rejects(self.fixture.save(), "configuration/extensions")


if __name__ == "__main__":
    unittest.main()
