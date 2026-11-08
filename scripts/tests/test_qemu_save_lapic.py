"""APIC bank transport only; C++ owns arbitration and no guest is executed."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from scripts.qemu_save_lapic import DELIVERY_PROFILE, PROFILE, main, project_fixed_lapic
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError
from scripts.tests.test_qemu_save_events import EventFixture
from scripts.tests.test_qemu_save_snapshot import PC
from scripts.tests.test_qemu_save_x64 import scalar


class QemuSaveLocalApicTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.fixture = EventFixture()
        self.fixture.apic["tmr"] = [0xffff0000, 0x80000000, 0, 0, 0, 0, 0, 1]
        self.arguments = dict(profile=PROFILE, delivery_profile=DELIVERY_PROFILE,
                              cpu=3, apic=9, expected_pc=PC)

    def snapshot(self):
        with tempfile.NamedTemporaryFile(dir=self.directory.name, delete=False) as source:
            source.write(self.fixture.save())
        result = QemuSaveSnapshot(source.name)
        self.path = Path(source.name)
        self.addCleanup(result.close)
        return result

    def project(self, snapshot=None, **overrides):
        return project_fixed_lapic(snapshot or self.snapshot(), **(self.arguments | overrides))

    def test_exact_owned_bank_transport_without_selection_or_observations(self):
        snapshot = self.snapshot()
        before = deepcopy((snapshot.apics, snapshot.cpus, snapshot.cpu_common))
        with patch.object(snapshot, "read_ram", side_effect=AssertionError("RAM observed")), \
                patch.object(snapshot, "read_device", side_effect=AssertionError("device observed")):
            result = self.project(snapshot)
        self.assertEqual(result.fields, {
            "IRR": self.fixture.apic["irr"], "ISR": self.fixture.apic["isr"],
            "TMR": self.fixture.apic["tmr"], "TPR": 0xf0, "EOIBroadcastSuppressed": False})
        self.assertEqual(result.bindings, {**result.environment.bindings,
                                          "delivery_profile": DELIVERY_PROFILE,
                                          "delivery_profile_origin": "producer-bound"})
        self.assertEqual((snapshot.apics, snapshot.cpus, snapshot.cpu_common), before)
        result.fields["IRR"][6] = 0
        self.assertEqual(snapshot.apics[9]["fields"]["irr"][6], 0x20000)
        self.assertEqual(result.environment.apic["IRR"][6], 0x20000)

    def test_pending_injected_halted_and_unsaved_shadows_are_not_consumed(self):
        self.fixture.state["env.exception_nr"] = scalar(14, 32)
        self.fixture.state["env.nmi_pending"] = scalar(1, 8)
        self.fixture.common["halted"] = scalar(1, 32)
        result = self.project()
        self.assertEqual(result.environment.events["Exception"]["Injected"], 1)
        self.assertEqual(result.environment.events["NMI"]["Pending"], 1)
        self.assertEqual(result.environment.activity["Halted"], 1)
        self.assertEqual(result.environment.interruptibility,
                         {"STIBlocked": None, "SSBlocked": None, "NMIBlockedBySTI": None})
        self.assertEqual(result.fields["IRR"][6], 0x20000)

    def test_delivery_profile_and_captured_enable_bits_must_agree(self):
        snapshot = self.snapshot()
        for profile in (None, "", "auto-eoi", PROFILE):
            with self.assertRaisesRegex(SnapshotError, "delivery profile"):
                self.project(snapshot, delivery_profile=profile)
        fields = snapshot.apics[9]["fields"]
        for name, bit in (("apicbase", 0x800), ("spurious_vec", 0x100)):
            saved = fields[name]
            fields[name] &= ~bit
            with self.assertRaisesRegex(SnapshotError, "enabled"):
                self.project(snapshot)
            fields[name] = saved
        for override in ({"expected_pc": PC + 1}, {"cpu": 9}, {"apic": 3}, {"profile": "unknown"}):
            with self.assertRaises(SnapshotError):
                self.project(snapshot, **override)

    def test_owner_validates_reserved_vectors_instead_of_python_stripping_bits(self):
        snapshot = self.snapshot()
        snapshot.apics[9]["fields"]["irr"][0] = 0x8001
        result = self.project(snapshot)
        self.assertEqual(result.fields["IRR"][0], 0x8001)
        source = result.cpp_initializer()
        self.assertIn("0x00008001U", source)
        self.assertIn("X86LocalApicFixed::fromState", source)
        self.assertIn("static_assert(neverd_captured_lapic_fixed.has_value()", source)

    def test_suppression_comes_from_saved_svr_and_initialization_uses_owner_type(self):
        snapshot = self.snapshot()
        for suppressed in (False, True):
            snapshot.apics[9]["fields"]["spurious_vec"] = 0x1ff | (int(suppressed) << 12)
            result = self.project(snapshot)
            self.assertIs(result.fields["EOIBroadcastSuppressed"], suppressed)
            source = result.cpp_initializer()
            self.assertIn('#include "neverd/translate/X86/X86LocalApic.h"', source)
            self.assertIn("constexpr auto neverd_captured_lapic_fixed", source)
            self.assertIn("value.EOIBroadcastSuppressed = " + str(suppressed).lower(), source)
            self.assertNotIn("acceptFixedInterrupt(", source)

    def test_cli_json_cpp_and_atomic_binding_failure(self):
        snapshot = self.snapshot()
        args = [str(self.path), "--profile", PROFILE, "--delivery-profile", DELIVERY_PROFILE,
                "--cpu", "3", "--apic", "9", "--expect-pc", hex(PC)]
        result = self.project(snapshot)
        for suffix, expected in (([], json.dumps(result.summary(), indent=2) + "\n"),
                                 (["--cpp-initializer"], result.cpp_initializer())):
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(main(args + suffix), 0)
            self.assertEqual(output.getvalue(), expected)
        for suffix in (["--expect-pc", hex(PC + 1)], ["--apic", "3"]):
            output, errors = io.StringIO(), io.StringIO()
            with redirect_stdout(output), redirect_stderr(errors):
                self.assertEqual(main(args + suffix), 2)
            self.assertEqual(output.getvalue(), "")
            self.assertIn("qemu-save-lapic:", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
