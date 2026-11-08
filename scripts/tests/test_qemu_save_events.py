"""Authored event save records, without guest or instruction execution."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts.qemu_save_events import PROFILE, main, project_events
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError
from scripts.qemu_save_x64 import project_v6
from scripts.tests.test_qemu_save_snapshot import PC, be, field, section, subsection
from scripts.tests.test_qemu_save_x64 import RegisterFixture, encode_fields, scalar


class EventFixture(RegisterFixture):
    def __init__(self):
        super().__init__()
        self.state.update({
            "env.hflags": scalar(0x40c2b0, 32), "env.hflags2": scalar(0x101, 32),
            "env.interrupt_injected": scalar(0xffffffff, 32),
            "env.exception_nr": scalar(0xffffffff, 32), "env.has_error_code": scalar(0, 8),
            "env.mp_state": scalar(0, 32), "env.sipi_vector": scalar(0x81, 32),
            "env.soft_interrupt": scalar(0, 8), "env.nmi_pending": scalar(0, 8),
        })
        self.cpu_sections = {}
        self.common = {"halted": scalar(0, 32), "interrupt_request": scalar(0x80000002, 32)}
        self.common_sections = {}
        self.apic = {"apicbase": scalar(0xfee00900, 32), "tpr": scalar(0xf0, 8),
                     "spurious_vec": scalar(0x1ff, 32),
                     "irr": [0, 0, 0, 0, 0, 0, 0x20000, 0],
                     "isr": [0, 0, 0, 0, 0, 0, 0x20000, 0],
                     "tmr": [0xffffffff, 0x80000000, 0, 0, 0, 0, 0, 1]}

    def stream(self):
        data = super().stream()
        cpu, apic, common = self.description["devices"]
        for item in cpu["fields"]:
            if item["name"] in ("env.interrupt_injected", "env.exception_nr"):
                item["type"] = "int32"

        def append_subsections(owner, values):
            result = b""
            for name, fields in values.items():
                members, payload = encode_fields(fields)
                for item in members:
                    if item["name"] == "env.error_code":
                        item["type"] = "int32"
                owner.setdefault("subsections", []).append({
                    "vmsd_name": name, "version": 1, "fields": members})
                result += subsection(name, 1, payload)
            return result

        encoded = encode_fields(self.state)[1]
        data = data.replace(section(4, 5, encoded, "cpu", instance=3, version=12),
                            section(4, 5, encoded + append_subsections(cpu, self.cpu_sections),
                                    "cpu", instance=3, version=12), 1)
        apic_fields, payload = [], b""
        for name, value in self.apic.items():
            if isinstance(value, list):
                apic_fields.append(field(name, "uint32", 4, array_len=len(value)))
                payload += b"".join(be(word, 4) for word in value)
            else:
                members, encoded = encode_fields({name: value})
                apic_fields.extend(members)
                payload += encoded
        apic["fields"] = apic_fields
        data = data.replace(section(4, 9, be(0xab, 4), "apic", instance=9, version=3),
                            section(4, 9, payload, "apic", instance=9, version=3), 1)
        common["fields"], payload = encode_fields(self.common)
        payload += append_subsections(common, self.common_sections)
        return data.replace(section(4, 20, be(1, 4), "cpu_common", instance=3),
                            section(4, 20, payload, "cpu_common", instance=3), 1)


class QemuSaveEventTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.fixture = EventFixture()
        self.arguments = dict(profile=PROFILE, cpu=3, apic=9, expected_pc=PC,
                              sti_blocked=1, ss_blocked=0, nmi_blocked_by_sti=1)

    def snapshot(self):
        with tempfile.NamedTemporaryFile(dir=self.directory.name, delete=False) as source:
            source.write(self.fixture.save())
        result = QemuSaveSnapshot(source.name)
        self.path = Path(source.name)
        self.addCleanup(result.close)
        return result

    def project(self, snapshot=None, **overrides):
        return project_events(snapshot or self.snapshot(), **(self.arguments | overrides))

    def test_exact_event_banks_without_ram_devices_or_acknowledgement(self):
        snapshot = self.snapshot()
        before = deepcopy((snapshot.cpus, snapshot.apics, snapshot.cpu_common))
        with patch.object(snapshot, "read_ram", side_effect=AssertionError("RAM observed")), \
                patch.object(snapshot, "read_device", side_effect=AssertionError("device observed")):
            projected = self.project(snapshot)
        self.assertEqual(projected.events, {
            "Exception": {"Vector": -1, "Pending": 0, "Injected": 0, "HasErrorCode": 0,
                          "ErrorCode": None, "HasPayload": None, "Payload": None},
            "Interrupt": {"Vector": -1, "Injected": 0, "Soft": 0},
            "NMI": {"Injected": 0, "Pending": 0, "Blocked": 0}, "TripleFaultPending": 0})
        self.assertEqual(projected.activity, {
            "Halted": 0, "InterruptRequest": 0x80000002, "MPState": 0, "SIPIVector": 0x81,
            "SMM": 0, "GuestMode": 0, "SMMInsideNMI": 0, "CrashOccurred": 0})
        self.assertEqual(projected.apic, {"Base": 0xfee00900, "TPR": 0xf0, "SpuriousVector": 0x1ff,
                                         **{k.upper(): self.fixture.apic[k] for k in ("irr", "isr", "tmr")}})
        # Pending IRR is retained even while TPR and ISR would block this vector.
        self.assertEqual(projected.apic["IRR"][6], 0x20000)
        self.assertEqual(projected.origins["exception.payload"], "not-saved")
        self.assertEqual((snapshot.cpus, snapshot.apics, snapshot.cpu_common), before)
        projected.apic["IRR"][6] = 0
        self.assertEqual(snapshot.apics[9]["fields"]["irr"][6], 0x20000)

    def test_omitted_exception_info_restores_injected_for_every_vector(self):
        snapshot = self.snapshot()
        for vector in range(256):
            snapshot.cpus[3]["fields"]["env.exception_nr"] = vector
            projected = self.project(snapshot)
            self.assertEqual(projected.events["Exception"]["Vector"], vector)
            self.assertEqual(projected.events["Exception"]["Injected"], 1)
            self.assertEqual(projected.events["Exception"]["Pending"], 0)
            self.assertEqual(projected.origins["exception.phase"], "qemu-post-load-injected")
        self.assertNotIn("env.exception_injected", snapshot.cpus[3]["fields"])

    def test_signed_error_code_and_stale_error_without_an_exception(self):
        self.fixture.state["env.has_error_code"] = scalar(1, 8)
        self.fixture.cpu_sections["cpu/error_code"] = {"env.error_code": scalar(0x80000005, 32)}
        snapshot = self.snapshot()
        result = self.project(snapshot)
        self.assertEqual(result.events["Exception"]["ErrorCode"], 0x80000005)
        self.assertEqual(result.events["Exception"]["Injected"], 0)
        self.assertEqual(result.events["Exception"]["Vector"], -1)
        snapshot.cpus[3]["fields"]["env.exception_nr"] = 14
        self.assertEqual(self.project(snapshot).events["Exception"]["Injected"], 1)

    def test_pending_exception_payload_and_post_load_fallback_are_distinct(self):
        self.fixture.state["env.exception_nr"] = scalar(14, 32)
        self.fixture.state["env.hflags"] = scalar(0x60c2b0, 32)
        self.fixture.cpu_sections["cpu/exception_info"] = {
            "env.exception_pending": scalar(1, 8), "env.exception_injected": scalar(0, 8),
            "env.exception_has_payload": scalar(1, 8), "env.exception_payload": scalar(0xfedcba9876543210)}
        snapshot = self.snapshot()
        projected = self.project(snapshot)
        self.assertEqual(projected.events["Exception"]["Payload"], 0xfedcba9876543210)
        self.assertEqual(projected.events["Exception"]["Pending"], 1)
        self.assertEqual(projected.events["Exception"]["Injected"], 0)
        self.assertEqual(projected.activity["GuestMode"], 1)
        self.assertEqual(projected.origins["exception.phase"], "captured")
        info = snapshot.cpus[3]["fields"]["subsections"]["cpu/exception_info"]
        info["env.exception_pending"] = 0
        self.assertEqual(self.project(snapshot).origins["exception.phase"], "qemu-post-load-injected")
        info["env.exception_injected"] = 1
        self.assertEqual(self.project(snapshot).origins["exception.phase"], "captured")
        snapshot.cpus[3]["fields"]["env.exception_nr"] = -1
        with self.assertRaisesRegex(SnapshotError, "no vector"):
            self.project(snapshot)

    def test_nmi_injected_pending_and_masked_are_independent(self):
        snapshot = self.snapshot()
        fields = snapshot.cpus[3]["fields"]
        for blocked in (0, 1):
            for injected in (0, 1):
                for pending in (0, 1, 2, 255):
                    fields.update({"env.hflags2": 0x101 | blocked << 2,
                                   "env.nmi_pending": pending, "env.nmi_injected": injected})
                    event = self.project(snapshot).events["NMI"]
                    self.assertEqual(event, {"Injected": injected, "Pending": pending, "Blocked": blocked})
                    bank = project_v6(snapshot, profile=PROFILE, cpu=3, apic=9,
                                      expected_pc=PC, undefined=[0] * 8)
                    self.assertEqual(bank.fields["NMIBlocked"], event["Blocked"])

    def test_soft_injected_interrupt_is_not_relabelled_as_new_hardware_irq(self):
        snapshot = self.snapshot()
        fields = snapshot.cpus[3]["fields"]
        for vector in (-1, 0, 2, 128, 255):
            fields.update({"env.interrupt_injected": vector, "env.soft_interrupt": 1})
            self.assertEqual(self.project(snapshot).events["Interrupt"],
                             {"Vector": vector, "Injected": int(vector >= 0), "Soft": 1})

    def test_halted_crash_triple_fault_and_unknown_mp_state_are_preserved(self):
        self.fixture.common["halted"] = scalar(1, 32)
        self.fixture.state["env.mp_state"] = scalar(0xffffffff, 32)
        self.fixture.state["env.hflags"] = scalar(0x28c2b0, 32)
        self.fixture.state["env.hflags2"] = scalar(0x115, 32)
        self.fixture.cpu_sections["cpu/triple_fault"] = {"env.triple_fault_pending": scalar(1, 8)}
        self.fixture.common_sections["cpu_common/crash_occurred"] = {"crash_occurred": scalar(1, 8)}
        projected = self.project()
        for name in ("Halted", "SMM", "GuestMode", "SMMInsideNMI", "CrashOccurred"):
            self.assertEqual(projected.activity[name], 1)
        self.assertEqual(projected.activity["MPState"], 0xffffffff)
        self.assertEqual(projected.events["TripleFaultPending"], 1)
        self.assertEqual(projected.origins["activity.CrashOccurred"], "captured")

    def test_unsaved_shadows_stay_unknown_even_when_hflags_is_clear(self):
        snapshot = self.snapshot()
        for flags in (0, 8):
            snapshot.cpus[3]["fields"]["env.hflags"] = flags
            self.assertEqual(self.project(snapshot).interruptibility,
                             {"STIBlocked": 1, "SSBlocked": 0, "NMIBlockedBySTI": 1})
            self.assertEqual(self.project(snapshot, sti_blocked=0, ss_blocked=1,
                                          nmi_blocked_by_sti=0).interruptibility,
                             {"STIBlocked": 0, "SSBlocked": 1, "NMIBlockedBySTI": 0})
        for name in ("sti_blocked", "ss_blocked", "nmi_blocked_by_sti"):
            for invalid in (-1, 2, False):
                with self.assertRaises(SnapshotError):
                    self.project(snapshot, **{name: invalid})
            arguments = dict(self.arguments)
            del arguments[name]
            projected = project_events(snapshot, **arguments)
            field_name = {"sti_blocked": "STIBlocked", "ss_blocked": "SSBlocked",
                          "nmi_blocked_by_sti": "NMIBlockedBySTI"}[name]
            self.assertIsNone(projected.interruptibility[field_name])
            self.assertEqual(projected.origins[f"interruptibility.{field_name}"], "not-saved")

    def test_required_field_shapes_and_cpu_apic_identity(self):
        snapshot = self.snapshot()
        fields = snapshot.cpus[3]["fields"]
        checks = {"env.exception_nr": [-2, 256, True], "env.interrupt_injected": [-2, 256],
                  "env.nmi_injected": [2], "env.soft_interrupt": [2], "env.has_error_code": [2],
                  "env.nmi_pending": [256], "env.hflags": [-1], "env.hflags2": [1 << 32],
                  "env.mp_state": [1 << 32], "env.sipi_vector": [-1]}
        for name, invalids in checks.items():
            original = fields.pop(name)
            with self.assertRaises(SnapshotError):
                self.project(snapshot)
            for invalid in invalids:
                fields[name] = invalid
                with self.assertRaises(SnapshotError):
                    self.project(snapshot)
            fields[name] = original
        for override in ({"profile": "unknown"}, {"cpu": 9}, {"apic": 3}, {"apic": True},
                         {"expected_pc": PC + 1}):
            with self.assertRaises(SnapshotError):
                self.project(snapshot, **override)
        for record, name in ((snapshot.cpus[3], "CPU"), (snapshot.apics[9], "APIC"),
                             (snapshot.cpu_common[3], "CPU-common")):
            version = record["version"]
            record["version"] = 99
            with self.assertRaisesRegex(SnapshotError, name):
                self.project(snapshot)
            record["version"] = version
        del snapshot.cpu_common[3]
        with self.assertRaisesRegex(SnapshotError, "CPU-common"):
            self.project(snapshot)

    def test_apic_banks_are_exact_and_bounded_without_priority_selection(self):
        snapshot = self.snapshot()
        fields = snapshot.apics[9]["fields"]
        for name in ("irr", "isr", "tmr"):
            original = fields[name]
            for invalid in (None, [], [0] * 7, [0] * 9, [0] * 7 + [1 << 32], [0] * 7 + [True]):
                fields[name] = invalid
                with self.assertRaises(SnapshotError):
                    self.project(snapshot)
            fields[name] = original
        for bit in range(256):
            fields["irr"] = [0] * 8
            fields["irr"][bit // 32] = 1 << (bit % 32)
            self.assertEqual(self.project(snapshot).apic["IRR"], fields["irr"])

    def test_subsection_versions_error_presence_and_malformed_values(self):
        snapshot = self.snapshot()
        record = snapshot.cpus[3]
        sections = record["fields"].setdefault("subsections", {})
        versions = record["subsection_versions"]
        for name, field_name in (("cpu/error_code", "env.error_code"),
                                 ("cpu/triple_fault", "env.triple_fault_pending"),
                                 ("cpu/exception_info", "env.exception_pending")):
            sections[name] = {}
            with self.assertRaisesRegex(SnapshotError, "mismatch"):
                self.project(snapshot)
            versions[name] = 2
            with self.assertRaisesRegex(SnapshotError, "subsection v1"):
                self.project(snapshot)
            versions[name] = 1
            if name == "cpu/error_code":
                record["fields"]["env.has_error_code"] = 1
            with self.assertRaises(SnapshotError):
                self.project(snapshot)
            sections[name][field_name] = 1 << 64
            with self.assertRaises(SnapshotError):
                self.project(snapshot)
            del sections[name]
            with self.assertRaisesRegex(SnapshotError, "mismatch"):
                self.project(snapshot)
            del versions[name]
            record["fields"]["env.has_error_code"] = 0
        record["fields"]["env.has_error_code"] = 1
        with self.assertRaisesRegex(SnapshotError, "flag/subsection"):
            self.project(snapshot)

    def test_kvm_common_rejects_tcg_exception_index(self):
        self.fixture.common_sections["cpu_common/exception_index"] = {"exception_index": scalar(1, 32)}
        with self.assertRaisesRegex(SnapshotError, "TCG"):
            self.project()

    def test_cli_keeps_provenance_and_fails_without_partial_output(self):
        snapshot = self.snapshot()
        args = [str(self.path), "--profile", PROFILE, "--cpu", "3", "--apic", "9",
                "--expect-pc", hex(PC), "--sti-blocked", "1", "--ss-blocked", "0",
                "--nmi-blocked-by-sti", "1"]
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(args), 0)
        self.assertEqual(json.loads(output.getvalue()), self.project(snapshot).summary())
        for suffix in (["--expect-pc", hex(PC + 1)], ["--ss-blocked", "2"], ["--apic", "3"]):
            output, errors = io.StringIO(), io.StringIO()
            with redirect_stdout(output), redirect_stderr(errors):
                self.assertEqual(main(args + suffix), 2)
            self.assertEqual(output.getvalue(), "")
            self.assertIn("qemu-save-events:", errors.getvalue())
        output = io.StringIO()
        with redirect_stdout(output):
            self.assertEqual(main(args[:-6]), 0)
        result = json.loads(output.getvalue())
        self.assertEqual(result["interruptibility"], {"STIBlocked": None, "SSBlocked": None,
                                                     "NMIBlockedBySTI": None})


if __name__ == "__main__":
    unittest.main()
