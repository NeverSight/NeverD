"""Offline RTM evidence from serialized QEMU CPU records."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import call, patch

from scripts.qemu_save_rtm import PROFILE, main, project_rtm
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError
from scripts.tests.test_qemu_save_controls import ControlFixture
from scripts.tests.test_qemu_save_snapshot import PC, be, section, subsection
from scripts.tests.test_qemu_save_x64 import encode_fields, scalar


class QemuSaveRtmTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "offline.save"

    def save(self, value=None, bits=32):
        fixture = ControlFixture()
        if value is not None:
            fixture.control_sections["cpu/msr_tsx_ctrl"] = {"env.tsx_ctrl": scalar(value, bits)}
        self.path.write_bytes(fixture.save())

    def test_cli_transports_only_the_saved_word(self):
        for value in (None, 0, 1, 2, 3):
            with self.subTest(value=value):
                self.save(value)
                output = io.StringIO()
                base = PC & ~0xffff
                with redirect_stdout(output):
                    self.assertEqual(main([str(self.path), "--profile", PROFILE,
                                           "--expect-pc", hex(PC), "--load-base", hex(base)]), 0)
                self.assertEqual(json.loads(output.getvalue()), {"native_rtm_abort_steps": {
                    "qemu_tsx_ctrl_v1": {"cpu": 3, "rip": hex(PC), "value": value,
                                         "load_base": hex(base)}}})

    def test_cpu_and_rip_are_bound_before_reading_controls(self):
        self.save(3)
        with QemuSaveSnapshot(self.path) as snapshot, \
                patch.object(snapshot, "read_ram", side_effect=AssertionError("RAM read")):
            def project(pc=PC, **kwargs):
                return project_rtm(snapshot, profile=PROFILE, expected_pc=pc, **kwargs)

            with self.assertRaisesRegex(SnapshotError, "no saved vCPU"):
                project(PC + 1)
            with self.assertRaisesRegex(SnapshotError, "PC mismatch"):
                project(PC + 1, cpu=3)
            with self.assertRaisesRegex(SnapshotError, "absent"):
                project(cpu=0)
            with self.assertRaisesRegex(SnapshotError, "load base"):
                project(load_base=PC + 1)
            # A caller can freely edit public projections. The wire observation wins.
            cached = snapshot.cpus[3]["fields"]
            cached["env.eip"] = PC + 1
            cached["subsections"]["cpu/msr_tsx_ctrl"]["env.tsx_ctrl"] = 2
            with patch.object(snapshot, "read_device", wraps=snapshot.read_device) as read:
                site = project()["native_rtm_abort_steps"]["qemu_tsx_ctrl_v1"]
                read.assert_called_once_with("cpu", 3)
                self.assertEqual(site, {"cpu": 3, "rip": hex(PC), "value": 3})

    def test_two_saved_cpus_at_one_rip_require_an_instance(self):
        class TwoCpus(ControlFixture):
            def stream(self):
                data = super().stream()
                other = deepcopy(self.description["devices"][0])
                other["instance_id"] = 7
                self.description["devices"].append(other)
                payload = encode_fields(self.state)[1] + subsection("cpu/msr_tsx_ctrl", 1, be(2, 4))
                return data[:-1] + section(4, 22, payload, "cpu", instance=7, version=12) + data[-1:]

        fixture = TwoCpus()
        fixture.control_sections["cpu/msr_tsx_ctrl"] = {"env.tsx_ctrl": scalar(3, 32)}
        self.path.write_bytes(fixture.save())
        with QemuSaveSnapshot(self.path) as snapshot:
            with patch.object(snapshot, "read_device", side_effect=AssertionError("CPU decoded")), \
                    patch.object(snapshot, "_uint", wraps=snapshot._uint) as word, \
                    self.assertRaisesRegex(SnapshotError, "multiple saved vCPUs"):
                project_rtm(snapshot, profile=PROFILE, expected_pc=PC)
            self.assertEqual(word.call_args_list, [call(8), call(8)])
            with patch.object(snapshot, "read_device", wraps=snapshot.read_device) as read:
                result = project_rtm(snapshot, profile=PROFILE, expected_pc=PC, cpu=7)
                read.assert_called_once_with("cpu", 7)
            self.assertEqual(result["native_rtm_abort_steps"]["qemu_tsx_ctrl_v1"]["value"], 2)

    def test_wrong_rip_wire_width_cannot_bind_even_an_explicit_cpu(self):
        fixture = ControlFixture()
        fixture.state["env.eip"] = scalar(0x1234, 32)
        self.path.write_bytes(fixture.save())
        with QemuSaveSnapshot(self.path) as snapshot:
            for cpu in (None, 3):
                with self.assertRaisesRegex(SnapshotError, "saved x64 uint64 RIP"):
                    project_rtm(snapshot, profile=PROFILE, expected_pc=0x1234, cpu=cpu)

    def test_rejects_a_different_wire_field_even_when_the_word_fits(self):
        self.save(3, bits=64)
        output, errors = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(errors):
            self.assertEqual(main([str(self.path), "--profile", PROFILE,
                                   "--expect-pc", hex(PC)]), 2)
        self.assertFalse(output.getvalue())
        self.assertIn("one uint32", errors.getvalue())
