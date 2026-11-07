"""Device evidence over authored save sections; no device or guest execution."""

from contextlib import redirect_stderr, redirect_stdout
from dataclasses import replace
import io
import json
from pathlib import Path
import tempfile
import unittest

from scripts.qemu_save_devices import main
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, SnapshotLimits
from scripts.tests.test_qemu_save_snapshot import Fixture, PC, field, section, subsection


class DeviceFixture(Fixture):
    def __init__(self):
        super().__init__()
        self.config = bytes(range(256))
        self.device = {
            "name": "pci/bridge", "instance_id": 7, "vmsd_name": "mch", "version": 1,
            "fields": [field("parent_obj", "struct", 276, struct={
                "vmsd_name": "PCIDevice", "version": 2,
                "fields": [field("version_id", "int32 le", 4),
                           field("config[0]", "pci config", 256),
                           field("irq_state", "pci irq state", 16)],
            })],
            "subsections": [{"vmsd_name": "mch/extra", "version": 3,
                             "fields": [field("flag", "uint8", 1)]}],
        }
        self.description["devices"].extend([
            self.device, {"name": "pci/bridge", "instance_id": 2, "vmsd_name": "mch",
                          "version": 1, "fields": [field("opaque", "buffer", 3)]}])
        self.payload = b"\0\0\0\x02" + self.config + b"\0\0\0\x01" * 4
        self.payload += subsection("mch/extra", 3, b"\x80")

    def stream(self):
        return (super().stream()[:-1] +
                section(4, 93, self.payload, "pci/bridge", instance=7) +
                section(4, 72, b"\x11\x80\xff", "pci/bridge", instance=2) + b"\0")


class ObservedSnapshot(QemuSaveSnapshot):
    def __init__(self, *args, **kwargs):
        self.decoded_layouts = []
        super().__init__(*args, **kwargs)

    def _state(self, layout, keep):
        if keep:
            self.decoded_layouts.append(layout.name)
        return super()._state(layout, keep)


class QemuSaveDeviceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "input.save"
        self.fixture = DeviceFixture()

    def open(self, data=None, limits=SnapshotLimits()):
        self.path.write_bytes(self.fixture.save() if data is None else data)
        snapshot = ObservedSnapshot(self.path, limits)
        self.addCleanup(snapshot.close)
        return snapshot

    def test_inventory_preserves_device_instance_section_and_layout_identities(self):
        snapshot = self.open()
        self.assertNotIn("mch", snapshot.decoded_layouts)
        before = list(snapshot.decoded_layouts)
        inventory = snapshot.device_inventory()
        self.assertEqual([(row["name"], row["instance_id"]) for row in inventory],
                         [("apic", 3), ("cpu", 3), ("pci/bridge", 2), ("pci/bridge", 7)])
        self.assertEqual(inventory[-1], {"name": "pci/bridge", "instance_id": 7,
                                        "section_id": 93, "vmsd_name": "mch", "version": 1,
                                        "serialized_bytes": 292})
        self.assertEqual(snapshot.decoded_layouts, before)
        inventory[-1]["name"] = "changed"
        self.assertEqual(snapshot.device_inventory()[-1]["name"], "pci/bridge")

    def test_selected_device_preserves_pci_bytes_comparison_version_and_subsections(self):
        snapshot = self.open()
        record = snapshot.read_device("pci/bridge", 7)
        self.assertEqual(record["name"], "pci/bridge")
        self.assertEqual(record["instance_id"], 7)
        self.assertEqual(record["section_id"], 93)
        self.assertEqual(record["vmsd_name"], "mch")
        self.assertEqual(record["version"], 1)
        self.assertEqual(record["subsection_versions"], {"mch/extra": 3})
        parent = record["fields"]["parent_obj"]
        self.assertEqual(parent["version_id"], 2)
        self.assertEqual(parent["config[0]"], bytes(range(256)))
        self.assertEqual(parent["irq_state"], b"\0\0\0\x01" * 4)
        self.assertEqual(record["fields"]["subsections"]["mch/extra"]["flag"], 0x80)
        self.assertEqual(snapshot.decoded_layouts.count("mch"), 1)
        self.assertEqual(self.path.read_bytes(), self.fixture.save())

    def test_reads_interleave_with_ram_and_instances_without_cursor_or_cache_leaks(self):
        snapshot = self.open()
        original = snapshot.read_device("pci/bridge", 7)
        original["fields"]["parent_obj"]["version_id"] = 99
        original["subsection_versions"].clear()
        self.assertEqual(snapshot.read_ram("pc.ram", 4094, 4), b"\x01\0\x80\x80")
        self.assertEqual(snapshot.read_device("pci/bridge", 2)["fields"], {"opaque": b"\x11\x80\xff"})
        self.assertEqual(snapshot.read_ram("/rom@data", 0, 2), b"DD")
        record = snapshot.read_device("pci/bridge", 7)
        self.assertEqual(record["fields"]["parent_obj"]["version_id"], 2)
        self.assertEqual(record["subsection_versions"], {"mch/extra": 3})
        snapshot.cpus[3]["fields"]["env.eip"] = 0
        # read_device observes the save afresh, not a caller-mutated CPU dict.
        self.assertEqual(snapshot.read_device("cpu", 3)["fields"]["env.eip"], PC)

    def test_selector_never_falls_back_to_layout_name_section_id_or_ordinal(self):
        snapshot = self.open()
        for name, instance in (("mch", 7), ("pci/bridge", 0), ("pci/bridge", 93), ("ram", 0),
                               ("PCI/bridge", 7), ("../input.save", 7)):
            with self.subTest(name=name, instance=instance), self.assertRaisesRegex(SnapshotError, "absent"):
                snapshot.read_device(name, instance)
        for name, instance in (("", 7), ("pci\0bridge", 7), ("\ud800", 7), (True, 7),
                               ("pci/bridge", True), ("pci/bridge", -1),
                               ("pci/bridge", 1 << 32), ("pci/bridge", "7")):
            with self.subTest(name=name, instance=instance), self.assertRaises(SnapshotError):
                snapshot.read_device(name, instance)

    def test_unselected_device_headers_footers_and_metadata_still_validate_at_open(self):
        data = self.fixture.save(stream=self.fixture.stream().replace(
            b"\x7e\0\0\0\x5d", b"\x7e\0\0\0\x5c", 1))
        with self.assertRaisesRegex(SnapshotError, "footer"):
            self.open(data)
        data = self.fixture.save(stream=self.fixture.stream().replace(
            subsection("mch/extra", 3, b"\x80"), subsection("mch/extra", 2, b"\x80")))
        with self.assertRaisesRegex(SnapshotError, "subsection header"):
            self.open(data)
        self.fixture.device["version"] = 2
        with self.assertRaisesRegex(SnapshotError, "device section"):
            self.open()

    def test_auxiliary_layouts_share_aggregate_state_limits_before_lazy_reads(self):
        # The base fixture fits this ceiling, but its two extra devices do not.
        self.path.write_bytes(Fixture().save())
        limits = replace(SnapshotLimits(), state_values=40)
        with QemuSaveSnapshot(self.path, limits):
            pass
        with self.assertRaisesRegex(SnapshotError, "device states"):
            self.open(limits=limits)

    def test_cli_lists_or_selects_exact_devices_and_deduplicates_in_request_order(self):
        self.path.write_bytes(self.fixture.save())
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main([str(self.path)]), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result["devices"], [])
        self.assertEqual(len(result["inventory"]), 4)
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main([str(self.path), "--device", "pci/bridge", "7",
                                   "--device", "pci/bridge", "2", "--device", "pci/bridge", "0x7"]), 0)
        result = json.loads(output.getvalue())
        self.assertEqual([row["instance_id"] for row in result["devices"]], [7, 2])
        parent = result["devices"][0]["fields"]["parent_obj"]
        self.assertEqual(parent["version_id"], "0x2")
        self.assertEqual(parent["config[0]"], {"raw_hex": bytes(range(256)).hex()})
        self.assertEqual(result["devices"][0]["subsection_versions"], {"mch/extra": 3})

    def test_cli_failing_selection_never_publishes_successful_prefix(self):
        self.path.write_bytes(self.fixture.save())
        for name, instance in (("mch", "7"), ("pci/bridge", "unknown"),
                               ("pci/bridge", "-1"), ("pci/bridge", "0x100000000")):
            output, error = io.StringIO(), io.StringIO()
            with self.subTest(name=name, instance=instance), redirect_stdout(output), redirect_stderr(error):
                self.assertEqual(main([str(self.path), "--device", "pci/bridge", "7",
                                       "--device", name, instance]), 2)
            self.assertEqual(output.getvalue(), "")
            self.assertIn("qemu-save-devices:", error.getvalue())


if __name__ == "__main__":
    unittest.main()
