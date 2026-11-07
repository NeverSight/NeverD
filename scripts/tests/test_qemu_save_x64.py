"""Offline register projection fixtures; never execute a guest or emitted C."""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
import tempfile
import unittest

from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError
from scripts.qemu_save_x64 import PROFILE, main, project_v6
from scripts.tests.test_qemu_save_snapshot import Fixture, PC, be, field, section, subsection


def scalar(value, bits=64):
    return (f"uint{bits}", value)


def vector(register, first, count):
    return {f"_q_ZMMReg[{lane}]": scalar(0xe1d2c3b400000000 | register << 16 |
                                        lane << 8 | (register ^ 0xff))
            for lane in range(first, first + count)}


def encode_fields(values):
    """Encode test values without the reader's layout machinery."""
    fields, payload = [], b""
    for key, value in values.items():
        attributes = {}
        if isinstance(value, tuple):
            kind, number = value
            size = int(kind[4:]) // 8
            data = be(number, size)
        elif isinstance(value, dict):
            members, data = encode_fields(value)
            size, kind = len(data), "struct"
            attributes["struct"] = {"vmsd_name": "fixture", "version": 1, "fields": members}
        else:
            attributes["array_len"] = len(value)
            if isinstance(value[0], dict):
                members, _ = encode_fields(value[0])
                chunks = [encode_fields(item)[1] for item in value]
                size, kind = len(chunks[0]), "struct"
                attributes["struct"] = {"vmsd_name": "fixture", "version": 1, "fields": members}
            else:
                kind, size = "uint64", 8
                chunks = [be(item, 8) for item in value]
            data = b"".join(chunks)
        fields.append(field(key, kind, size, **attributes))
        payload += data
    return fields, payload


class RegisterFixture(Fixture):
    def __init__(self):
        super().__init__()
        self.xml = b'<domain type="kvm"><os><type arch="x86_64">hvm</type></os></domain>\0'

        def segment(index, flags=0xc09300):
            return {"selector": scalar(0x10 + 8 * index, 32),
                    "base": scalar(0xffff800080000000 + 0x10000 * index),
                    "limit": scalar(0xfff80 + index, 32), "flags": scalar(flags, 32)}

        segments = [segment(index) for index in range(6)]
        segments[1] = segment(1, 0xa09b00)
        self.state = {
            "env.regs": [0xfedcba9800000000 + index for index in range(16)],
            "env.eip": scalar(PC), "env.eflags": scalar(0x40246),
            "env.segs": segments, "env.ldt": segment(6, 0xc00000),
            "env.tr": segment(7, 0x8b00),
            "env.gdt": {"base": scalar(0xffff800012340000), "limit": scalar(0x57, 32)},
            "env.idt": {"base": scalar(0xffff800098760000), "limit": scalar(0xfff, 32)},
            "env.mxcsr": scalar(0x5f80, 32), "env.cr[2]": scalar(0xf123456789abcdef),
            "env.cr[3]": scalar(0x123000), "env.cr[4]": scalar(0x3506f8),
            "env.efer": scalar(0xd01), "env.star": scalar(0x23001000000000),
            "env.kernelgsbase": scalar(0x9876543210), "env.hflags2": scalar(0x105, 32),
            "env.nmi_pending": scalar(1, 8), "env.nmi_injected": scalar(0, 8),
            "env.xcr0": scalar(7),
            "env.xmm_regs[0]": [vector(reg, 0, 2) for reg in range(16)],
            "env.xmm_regs[1]": [vector(reg, 2, 2) for reg in range(16)],
        }
        self.avx512 = None
        self.apic_instance = 9  # Must not infer CPU instance 3 => APIC 3.

    def stream(self):
        description, encoded = encode_fields(self.state)
        cpu = {"name": "cpu", "instance_id": 3, "version": 12, "fields": description}
        if self.avx512 is not None:
            members, data = encode_fields(self.avx512)
            cpu["subsections"] = [{"vmsd_name": "cpu/avx512", "version": 1, "fields": members}]
            encoded += subsection("cpu/avx512", 1, data)
        self.description["devices"] = [
            cpu, {"name": "apic", "instance_id": self.apic_instance, "version": 3,
                  "fields": [field("tpr", "uint32", 4)]},
            {"name": "cpu_common", "instance_id": 3, "version": 1,
             "fields": [field("halted", "uint32", 4)]},
        ]
        return (b"QEVM\0\0\0\x03\x07" + be(6, 4) + b"pc-q35" +
                section(1, 41, self.inventory + self.eos, "ram", version=4) +
                section(4, 5, encoded, "cpu", instance=3, version=12) +
                section(4, 9, be(0xab, 4), "apic", instance=self.apic_instance, version=3) +
                section(4, 20, be(1, 4), "cpu_common", instance=3) +
                section(3, 41, self.first + self.last + self.eos) + b"\0")

    def save(self):
        # stream() creates the matching serialized description before save().
        return super().save(stream=self.stream())


class QemuSaveX64Tests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "offline.save"
        self.fixture = RegisterFixture()
        self.choices = [0xaabbccdd00000000 + index for index in range(8)]

    def snapshot(self):
        self.path.write_bytes(self.fixture.save())
        result = QemuSaveSnapshot(self.path)
        self.addCleanup(result.close)
        return result

    def project(self, snapshot=None, **overrides):
        arguments = dict(profile=PROFILE, cpu=3, apic=9, expected_pc=PC, undefined=self.choices)
        arguments.update(overrides)
        return project_v6(snapshot or self.snapshot(), **arguments)

    def test_original_banks_and_explicit_choices(self):
        before = self.fixture.save()
        projection = self.project()
        fields = projection.fields
        self.assertEqual(fields["GPR"], self.fixture.state["env.regs"])
        self.assertEqual(fields["PC"], PC)
        self.assertEqual(fields["RFlags"], 0x40246)
        self.assertEqual(fields["CR8"], 10)
        self.assertEqual(fields["CR2"], 0xf123456789abcdef)
        self.assertEqual(fields["KernelGSBase"], 0x9876543210)
        self.assertEqual(fields["MXCSR"], 0x5f80)
        self.assertEqual(fields["STAR"], 0x23001000000000)
        self.assertEqual(fields["Undefined"], self.choices)
        self.assertIsNot(fields["Undefined"], self.choices)
        self.assertEqual(self.path.read_bytes(), before)

    def test_segments_tables_and_single_fs_gs_base(self):
        fields = self.project().fields
        self.assertEqual(fields["CS"], {"Selector": 0x18, "Base": 0xffff800080010000,
                                        "Limit": 0xfff81, "Attributes": 0xa09b})
        for name, index in (("ES", 0), ("SS", 2), ("DS", 3), ("LDTR", 6), ("TR", 7)):
            self.assertEqual(fields[name]["Selector"], 0x10 + 8 * index)
            self.assertEqual(fields[name]["Limit"], 0xfff80 + index)
        for name, index in (("FS", 4), ("GS", 5)):
            self.assertNotIn("Base", fields[name])
            self.assertEqual(fields[name + "Base"], 0xffff800080000000 + 0x10000 * index)
        self.assertEqual(fields["GDTR"]["Base"], 0xffff800012340000)
        self.assertEqual(fields["GDTR"]["Limit"], 0x57)
        self.assertEqual(fields["IDTR"]["Limit"], 0xfff)
        self.assertEqual(fields["Unusable"], 1 << 6)
        for name in ("ReservedV2", "ReservedTailV2", "ReservedV4", "ReservedV5"):
            self.assertEqual(fields[name], 0)
        self.assertEqual(fields["ReservedV6"], bytes(14))

    def test_unusable_bit_order_preserves_ignored_cache(self):
        snapshot = self.snapshot()
        state = snapshot.cpus[3]["fields"]
        caches = [state["env.segs"][index] for index in (1, 2, 3, 0, 4, 5)]
        caches += [state["env.ldt"], state["env.tr"]]
        for cache in caches:
            cache["flags"] |= 0x8000
        for index, cache in enumerate(caches):
            with self.subTest(index=index):
                cache["flags"] &= ~0x8000
                result = self.project(snapshot).fields
                self.assertEqual(result["Unusable"], 1 << index)
                if index == 6:
                    self.assertEqual(result["LDTR"]["Base"], cache["base"])
                    self.assertEqual(result["LDTR"]["Limit"], cache["limit"])
                cache["flags"] |= 0x8000

    def test_vector_lanes_and_proven_omitted_avx512(self):
        fields = self.project().fields
        self.assertEqual(len(fields["ZMM"]), 32)
        self.assertEqual(fields["ZMM"][9][8:16], bytes.fromhex("f6010900b4c3d2e1"))
        self.assertEqual(fields["ZMM"][15][24:32], bytes.fromhex("f0030f00b4c3d2e1"))
        self.assertEqual(fields["ZMM"][15][32:], bytes(32))
        self.assertEqual(fields["ZMM"][31], bytes(64))

    def test_present_avx512_and_disabled_xcr0_retain_all_bytes(self):
        self.fixture.state["env.xcr0"] = scalar(1)
        self.fixture.avx512 = {
            "env.opmask_regs": [1] * 8,
            "env.xmm_regs[0]": [vector(reg, 4, 4) for reg in range(16)],
            "env.xmm_regs[1]": [vector(reg, 0, 8) for reg in range(16, 32)],
        }
        projection = self.project()
        self.assertEqual(projection.fields["ZMM"][9][56:], bytes.fromhex("f6070900b4c3d2e1"))
        self.assertEqual(projection.fields["ZMM"][31][56:], bytes.fromhex("e0071f00b4c3d2e1"))
        self.assertEqual(projection.fields["ZMM"][1][24:32], bytes.fromhex("fe030100b4c3d2e1"))
        saved = projection.environment["cpu"]["fields"]["subsections"]["cpu/avx512"]
        self.assertEqual(saved["env.opmask_regs"], [1] * 8)

    def test_avx512_layout_version_is_not_inferred_from_cpu_version(self):
        self.fixture.avx512 = {
            "env.xmm_regs[0]": [vector(reg, 4, 4) for reg in range(16)],
            "env.xmm_regs[1]": [vector(reg, 0, 8) for reg in range(16, 32)],
        }
        snapshot = self.snapshot()
        versions = snapshot.cpus[3]["subsection_versions"]
        self.assertEqual(versions, {"cpu/avx512": 1})
        versions["cpu/avx512"] = 2
        with self.assertRaisesRegex(SnapshotError, "subsection v1"):
            self.project(snapshot)
        del versions["cpu/avx512"]
        with self.assertRaisesRegex(SnapshotError, "version mismatch"):
            self.project(snapshot)
        versions["cpu/avx512"] = 1
        del snapshot.cpus[3]["fields"]["subsections"]
        with self.assertRaisesRegex(SnapshotError, "version mismatch"):
            self.project(snapshot)

    def test_pending_and_halted_state_are_not_lost_or_used_as_nmi_block(self):
        snapshot = self.snapshot()
        fields = snapshot.cpus[3]["fields"]
        for masked, pending, injected in ((0, 1, 1), (1, 0, 0), (1, 1, 1)):
            fields.update({"env.hflags2": 0x101 | masked * 4,
                           "env.nmi_pending": pending, "env.nmi_injected": injected})
            projection = self.project(snapshot)
            self.assertEqual(projection.fields["NMIBlocked"], masked)
            self.assertEqual(projection.environment["cpu"]["fields"]["env.nmi_pending"], pending)
            self.assertEqual(projection.environment["cpu_common"]["fields"]["halted"], 1)
            self.assertEqual(projection.environment["cpu"]["fields"]["env.cr[3]"], 0x123000)

    def test_pc_and_apic_binding_never_fallback(self):
        snapshot = self.snapshot()
        for overrides in ({"expected_pc": PC + 1}, {"cpu": 0}, {"apic": 3},
                          {"apic": True}, {"profile": "future"}, {"undefined": [0] * 7},
                          {"undefined": [0] * 7 + [True]}, {"undefined": [0] * 7 + [1 << 64]}):
            with self.subTest(overrides=overrides), self.assertRaises(SnapshotError):
                self.project(snapshot, **overrides)

    def test_incomplete_banks_never_become_zero(self):
        snapshot = self.snapshot()
        original = deepcopy(snapshot.cpus[3]["fields"])
        for key in ("env.regs", "env.eip", "env.eflags", "env.mxcsr", "env.cr[2]",
                    "env.xmm_regs[0]", "env.xmm_regs[1]", "env.segs", "env.gdt",
                    "env.idt", "env.ldt", "env.tr", "env.hflags2", "env.efer",
                    "env.kernelgsbase", "env.star"):
            snapshot.cpus[3]["fields"] = deepcopy(original)
            del snapshot.cpus[3]["fields"][key]
            with self.subTest(key=key), self.assertRaises(SnapshotError):
                self.project(snapshot)

    def test_malformed_register_extents_and_values_fail(self):
        snapshot = self.snapshot()
        original = deepcopy(snapshot.cpus[3]["fields"])
        mutations = (
            ("env.regs", [0] * 15), ("env.regs", [False] * 16),
            ("env.xmm_regs[0]", [{}] * 16), ("env.xmm_regs[1]", [{}] * 17),
            ("env.mxcsr", 1 << 32), ("env.eflags", -1),
            ("env.gdt", {"base": 0, "limit": 0x10000}),
            ("env.ldt", {"base": 0, "limit": 0, "selector": 0, "flags": 1}),
            ("subsections", {"cpu/avx512": {}}),
        )
        for key, value in mutations:
            snapshot.cpus[3]["fields"] = deepcopy(original)
            snapshot.cpus[3]["fields"][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(SnapshotError):
                self.project(snapshot)

    def test_profiles_and_code_mode_fail_closed(self):
        snapshot = self.snapshot()
        for xml in ("<domain/>", "<bad", self.fixture.xml.decode().rstrip("\0").replace("kvm", "qemu"),
                    self.fixture.xml.decode().rstrip("\0").replace("x86_64", "aarch64")):
            snapshot.domain_xml = xml
            with self.subTest(xml=xml), self.assertRaises(SnapshotError):
                self.project(snapshot)
        snapshot.domain_xml = self.fixture.xml.decode().rstrip("\0")
        for owner in (snapshot.cpus[3], snapshot.apics[9]):
            original = owner["version"]
            owner["version"] = 0
            with self.assertRaises(SnapshotError):
                self.project(snapshot)
            owner["version"] = original
        for key, value in (("env.efer", 0x901), ("env.segs", [dict(selector=0, base=0, limit=0, flags=0)] * 6)):
            saved = snapshot.cpus[3]["fields"][key]
            snapshot.cpus[3]["fields"][key] = value
            with self.subTest(key=key), self.assertRaises(SnapshotError):
                self.project(snapshot)
            snapshot.cpus[3]["fields"][key] = saved

    def test_initializer_uses_exported_type_without_offset_duplication(self):
        projection = self.project()
        source = projection.c_initializer()
        self.assertIn("const neverd_x64_state_v6 neverd_captured_state_v6", source)
        self.assertIn(f".PC = UINT64_C({PC:#x})", source)
        self.assertIn(".Undefined = {", source)
        self.assertIn(".NMIBlocked = UINT64_C(0x1)", source)
        self.assertNotIn("typedef", source)
        self.assertNotIn("__builtin_trap", source)
        summary = projection.summary()
        self.assertEqual(summary["fields"]["ZMM"][9]["raw_hex"][16:32], "f6010900b4c3d2e1")
        self.assertEqual(summary["bindings"]["apic_instance"], 9)
        self.assertEqual(summary["kind"], "register_projection")

    def test_cli_outputs_only_after_projection_succeeds(self):
        self.snapshot()
        arguments = [str(self.path), "--profile", PROFILE, "--cpu", "3", "--apic", "9",
                     "--expect-pc", hex(PC), "--undefined"] + [str(v) for v in self.choices]
        for suffix in ([], ["--c-initializer"]):
            output, error = io.StringIO(), io.StringIO()
            with redirect_stdout(output), redirect_stderr(error):
                self.assertEqual(main(arguments + suffix), 0)
            self.assertEqual(error.getvalue(), "")
            if suffix:
                self.assertIn("neverd_captured_state_v6", output.getvalue())
            else:
                self.assertEqual(json.loads(output.getvalue())["fields"]["PC"], hex(PC))
        arguments[arguments.index(hex(PC))] = hex(PC + 1)
        output, error = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(error):
            self.assertEqual(main(arguments), 2)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("PC mismatch", error.getvalue())


if __name__ == "__main__":
    unittest.main()
