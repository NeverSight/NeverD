"""Control-bank transport over authored saves; no instruction execution."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts.qemu_save_controls import PROFILE, main, project_controls
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError
from scripts.qemu_save_x64 import project_v6
from scripts.tests.test_qemu_save_snapshot import PC, section, subsection
from scripts.tests.test_qemu_save_x64 import RegisterFixture, encode_fields, scalar


class ControlFixture(RegisterFixture):
    def __init__(self):
        super().__init__()
        self.state.update({
            "env.cr[0]": scalar(0x80050033), "env.a20_mask": scalar(0xffffffff, 32),
            "env.dr": [0xfedcba9876543000 + index for index in range(6)] + [0xffff0ff1, 0x401],
        })
        self.control_sections = {}

    def stream(self):
        data = super().stream()
        cpu = self.description["devices"][0]
        for item in cpu["fields"]:
            if item["name"] == "env.a20_mask":
                item["type"] = "int32"
        encoded = encode_fields(self.state)[1]
        selected = section(4, 5, encoded, "cpu", instance=3, version=12)
        suffix = b""
        for name, fields in self.control_sections.items():
            members, payload = encode_fields(fields)
            cpu.setdefault("subsections", []).append({"vmsd_name": name, "version": 1,
                                                       "fields": members})
            suffix += subsection(name, 1, payload)
        # Subsections follow the fields, before the enclosing section footer.
        return data.replace(selected, section(4, 5, encoded + suffix, "cpu", instance=3, version=12), 1)


class QemuSaveControlTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.fixture = ControlFixture()
        self.pdpte = [0xabcde00000000000 + index for index in range(4)]
        self.arguments = {"profile": PROFILE, "cpu": 3, "expected_pc": PC,
                          "debugctl": 0xfedcba9876543210, "pending_data": 9,
                          "missing_pdpte": self.pdpte}

    def snapshot(self):
        with tempfile.NamedTemporaryFile(dir=self.directory.name, delete=False) as source:
            source.write(self.fixture.save())
        snapshot = QemuSaveSnapshot(source.name)
        self.addCleanup(snapshot.close)
        return snapshot

    def project(self, snapshot=None, **overrides):
        return project_controls(snapshot or self.snapshot(), **(self.arguments | overrides))

    def test_original_controls_debug_indices_and_explicit_unobserved_inputs(self):
        snapshot = self.snapshot()
        before = deepcopy(snapshot.cpus)
        with patch.object(snapshot, "read_ram", side_effect=AssertionError("RAM observed")), \
                patch.object(snapshot, "read_device", side_effect=AssertionError("device observed")):
            projected = self.project(snapshot)
        self.assertEqual(projected.paging, {
            "CR0": 0x80050033, "CR3": 0x123000, "CR4": 0x3506f8,
            "EFER": 0xd01, "RFlags": 0x40246, "PKRU": 0, "PKRS": 0, "PDPTE": self.pdpte,
        })
        self.assertEqual(projected.debug, {
            "DR6": 0xffff0ff1, "DR7": 0x401, "DR": self.fixture.state["env.dr"][:4],
            "DebugCTL": 0xfedcba9876543210, "PendingData": 9,
        })
        self.assertEqual(projected.origins["paging.PKRU"], "omitted-zero")
        self.assertEqual(projected.origins["paging.PKRS"], "omitted-zero")
        self.assertEqual(projected.origins["paging.PDPTE"], "producer-bound-inactive-bank")
        self.assertEqual(projected.origins["debug.DebugCTL"], "producer-bound")
        self.assertEqual(projected.origins["debug.PendingData"], "producer-bound")
        self.assertEqual(snapshot.cpus, before)

    def test_saved_rights_and_cached_pdptrs_preserve_all_bits_with_features_disabled(self):
        self.fixture.state["env.cr[4]"] = scalar(0x20)
        self.fixture.control_sections = {
            "cpu/pkru": {"env.pkru": scalar(0xffffffff, 32)},
            "cpu/pkrs": {"env.pkrs": scalar(0x80000001, 32)},
            "cpu/pdptrs": {"env.pdptrs": [0, 1, 0xffffffffffffffff, 0x8000000000000000]},
        }
        projected = self.project(missing_pdpte=None)
        self.assertEqual(projected.paging["PKRU"], 0xffffffff)
        self.assertEqual(projected.paging["PKRS"], 0x80000001)
        self.assertEqual(projected.paging["PDPTE"], [0, 1, 0xffffffffffffffff, 0x8000000000000000])
        for field in ("PKRU", "PKRS", "PDPTE"):
            self.assertEqual(projected.origins[f"paging.{field}"], "captured")
        with self.assertRaisesRegex(SnapshotError, "override captured"):
            self.project()

    def test_absent_pdptrs_requires_binding_and_never_reads_cr3_ram(self):
        snapshot = self.snapshot()
        for value in (None, [], [0] * 3, [0] * 5, [0] * 3 + [True], [0] * 3 + [-1],
                      [0] * 3 + [1 << 64]):
            with self.subTest(value=value), self.assertRaises(SnapshotError), \
                    patch.object(snapshot, "read_ram", side_effect=AssertionError("CR3 read")):
                self.project(snapshot, missing_pdpte=value)

    def test_present_subsection_must_have_its_own_version_and_required_field(self):
        for name, key in (("cpu/pkru", "env.pkru"), ("cpu/pkrs", "env.pkrs"),
                          ("cpu/pdptrs", "env.pdptrs")):
            self.fixture.control_sections = {name: {key: [1, 2, 3, 4] if key.endswith("pdptrs")
                                                    else scalar(0x12345678, 32)}}
            snapshot = self.snapshot()
            missing = None if name.endswith("pdptrs") else self.pdpte
            versions = snapshot.cpus[3]["subsection_versions"]
            fields = snapshot.cpus[3]["fields"]["subsections"]
            versions[name] = 2
            with self.subTest(name=name), self.assertRaisesRegex(SnapshotError, "subsection v1"):
                self.project(snapshot, missing_pdpte=missing)
            del versions[name]
            with self.assertRaisesRegex(SnapshotError, "version mismatch"):
                self.project(snapshot, missing_pdpte=missing)
            versions[name] = 1
            del fields[name][key]
            with self.assertRaises(SnapshotError):
                self.project(snapshot, missing_pdpte=missing)
            del fields[name]
            with self.assertRaisesRegex(SnapshotError, "version mismatch"):
                self.project(snapshot, missing_pdpte=missing)

    def test_missing_or_malformed_mandatory_fields_never_default(self):
        snapshot = self.snapshot()
        original = deepcopy(snapshot.cpus[3]["fields"])
        for key in ("env.cr[0]", "env.cr[3]", "env.cr[4]", "env.efer", "env.eflags",
                    "env.dr", "env.a20_mask"):
            snapshot.cpus[3]["fields"] = deepcopy(original)
            del snapshot.cpus[3]["fields"][key]
            with self.subTest(key=key), self.assertRaises(SnapshotError):
                self.project(snapshot)
        for key, value in (("env.cr[0]", -1), ("env.cr[3]", True), ("env.cr[4]", 1 << 64),
                           ("env.dr", [0] * 7), ("env.dr", [0] * 7 + [1 << 64]),
                           ("env.dr", [False] * 8), ("env.a20_mask", 0xffefffff),
                           ("env.a20_mask", 0xffffffff), ("env.a20_mask", False)):
            snapshot.cpus[3]["fields"] = deepcopy(original)
            snapshot.cpus[3]["fields"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(SnapshotError):
                self.project(snapshot)

    def test_present_key_widths_and_pdptr_array_shape_are_checked(self):
        snapshot = self.snapshot()
        record = snapshot.cpus[3]
        for name, key in (("cpu/pkru", "env.pkru"), ("cpu/pkrs", "env.pkrs")):
            for value in (-1, True, 1 << 32):
                record["subsection_versions"] = {name: 1}
                record["fields"]["subsections"] = {name: {key: value}}
                with self.subTest(name=name, value=value), self.assertRaises(SnapshotError):
                    self.project(snapshot)
        record["subsection_versions"] = {"cpu/pdptrs": 1}
        for value in ([0] * 3, [0] * 5, [0] * 3 + [True], [0] * 3 + [1 << 64]):
            record["fields"]["subsections"] = {"cpu/pdptrs": {"env.pdptrs": value}}
            with self.subTest(value=value), self.assertRaises(SnapshotError):
                self.project(snapshot, missing_pdpte=None)

    def test_producer_debug_inputs_validate_without_inventing_admission(self):
        snapshot = self.snapshot()
        for arguments in ({"debugctl": -1}, {"debugctl": 1 << 64}, {"debugctl": True},
                          {"pending_data": -1}, {"pending_data": 16}, {"pending_data": False}):
            with self.subTest(arguments=arguments), self.assertRaises(SnapshotError):
                self.project(snapshot, **arguments)
        # Arbitrary saved/explicit control bits are transported, not normalized
        # to a supported runtime debug profile. The existing owner checks that.
        projected = self.project(snapshot, debugctl=(1 << 64) - 1, pending_data=15)
        self.assertEqual(projected.debug["DebugCTL"], (1 << 64) - 1)
        self.assertEqual(projected.debug["PendingData"], 15)

    def test_control_banks_share_original_cpu_coordinate_and_profile_checks(self):
        snapshot = self.snapshot()
        for arguments in ({"profile": "guessed"}, {"cpu": 5}, {"cpu": True}, {"expected_pc": PC + 1}):
            with self.subTest(arguments=arguments), self.assertRaises(SnapshotError):
                self.project(snapshot, **arguments)
        for xml in ('<domain type="qemu"><os><type arch="x86_64"/></os></domain>',
                    '<domain type="kvm"><os><type arch="i686"/></os></domain>', '<domain>'):
            snapshot.domain_xml = xml
            with self.subTest(xml=xml), self.assertRaises(SnapshotError):
                self.project(snapshot)
        snapshot = self.snapshot()
        snapshot.cpus[3]["version"] = 13
        with self.assertRaisesRegex(SnapshotError, "CPU v12"):
            self.project(snapshot)
        snapshot.cpus[3]["version"] = 12
        snapshot.cpus[3]["fields"]["env.segs"][1]["flags"] &= ~0x200000
        with self.assertRaisesRegex(SnapshotError, "64-bit code"):
            self.project(snapshot)

    def test_pending_halted_state_and_source_banks_remain_distinct(self):
        snapshot = self.snapshot()
        projected = self.project(snapshot)
        source = project_v6(snapshot, profile=PROFILE, cpu=3, apic=9, expected_pc=PC, undefined=[0] * 8)
        self.assertEqual(projected.paging["RFlags"], source.fields["RFlags"])
        self.assertEqual(projected.environment["cpu_common"]["fields"]["halted"], 1)
        self.assertEqual(projected.environment["cpu"]["fields"]["env.nmi_pending"], 1)
        self.assertNotIn("STIBlocked", projected.debug)
        self.assertNotIn("NMIBlocked", projected.debug)
        self.assertNotIn("runnable", projected.summary())
        projected.paging["PDPTE"][0] = 0
        projected.debug["DR"][0] = 0
        self.assertEqual(self.pdpte[0], 0xabcde00000000000)
        self.assertEqual(snapshot.cpus[3]["fields"]["env.dr"][0], 0xfedcba9876543000)

    def test_cli_json_cpp_and_failure_output(self):
        path = Path(self.directory.name) / "cli.save"
        path.write_bytes(self.fixture.save())
        args = [str(path), "--profile", PROFILE, "--cpu", "3", "--expect-pc", hex(PC),
                "--debugctl", "2", "--pending-data", "5"]
        fallback = ["--missing-pdpte", "0", "1", "2", "3"]
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(args + fallback), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result["debug"]["DebugCTL"], "0x2")
        self.assertEqual(result["paging"]["PDPTE"], ["0x0", "0x1", "0x2", "0x3"])
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(args + fallback + ["--cpp-initializer"]), 0)
        self.assertIn("constexpr neverd::translate::X86PagingState", output.getvalue())
        self.assertIn("producer-bound-inactive-bank", output.getvalue())
        for suffix in ([], fallback + ["--expect-pc", hex(PC + 1)],
                       fallback + ["--pending-data", "16", "--cpp-initializer"]):
            output, error = io.StringIO(), io.StringIO()
            with self.subTest(suffix=suffix), redirect_stdout(output), redirect_stderr(error):
                self.assertEqual(main(args + suffix), 2)
            self.assertEqual(output.getvalue(), "")
            self.assertIn("qemu-save-controls:", error.getvalue())


if __name__ == "__main__":
    unittest.main()
