"""Project saved QEMU/KVM x64 register banks into NeverD's V6 source layout.

This offline adapter does not restore a guest, map RAM or execute instructions.
Its explicit QEMU profile interprets serialized fields, not the host CPU. See
docs/qemu-save-snapshot.md for the format references and remaining environment
obligations. The implementation is independently authored from those formats.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

from scripts.qemu_save_snapshot import (
    QemuSaveSnapshot, SnapshotError, _integer, _json_state, _list, _object,
)


PROFILE = "qemu-10.2-kvm-x64"


def _uint(fields: dict, name: str, bits: int = 64) -> int:
    return _integer(fields.get(name), name, (1 << bits) - 1)


def _array(value, count: int, label: str) -> list:
    value = _list(value, label)
    if len(value) != count:
        raise SnapshotError(f"{label} requires exactly {count} elements")
    return value


def _segment(value) -> dict:
    value = _object(value, "x86 segment cache")
    flags = _uint(value, "flags", 32)
    if flags & ~0x00f0ff00:
        raise SnapshotError("unsupported QEMU segment flags")
    return {
        "Base": _uint(value, "base"),
        "Limit": _uint(value, "limit", 32),
        "Selector": _uint(value, "selector", 16),
        "Attributes": (flags >> 8) & 0xf0ff,
    }


def _vectors(fields: dict, versions: dict) -> list[bytes]:
    vectors = [bytearray(64) for _ in range(32)]

    def lanes(owner, key, first_register, first_lane, count):
        registers = _array(owner.get(key), 16, key)
        for index, register in enumerate(registers, first_register):
            register = _object(register, "saved vector")
            for lane in range(first_lane, first_lane + count):
                value = _uint(register, f"_q_ZMMReg[{lane}]")
                vectors[index][lane * 8:(lane + 1) * 8] = value.to_bytes(8, "little")

    # These two fields are mandatory in the selected CPU version, even when
    # XCR0 disables AVX. Their serialized array indices are not register IDs.
    lanes(fields, "env.xmm_regs[0]", 0, 0, 2)
    lanes(fields, "env.xmm_regs[1]", 0, 2, 2)
    subsections = _object(fields.get("subsections", {}), "CPU subsections")
    if ("cpu/avx512" in subsections) != ("cpu/avx512" in versions):
        raise SnapshotError("AVX-512 subsection/version mismatch")
    if "cpu/avx512" in subsections:
        if versions["cpu/avx512"] != 1:
            raise SnapshotError("register profile requires AVX-512 subsection v1")
        avx512 = _object(subsections["cpu/avx512"], "AVX-512 subsection")
        lanes(avx512, "env.xmm_regs[0]", 0, 4, 4)
        lanes(avx512, "env.xmm_regs[1]", 16, 0, 8)
    # Under this QEMU profile avx512_needed emits the subsection if ANY upper
    # lane, high register or opmask is nonzero. Its absence proves these bytes
    # zero; an absent mandatory XMM/YMM field never does.
    return [bytes(vector) for vector in vectors]


@dataclass
class V6RegisterProjection:
    """Captured banks plus explicit noncaptured choices, not a runnable VM."""

    fields: dict
    bindings: dict
    environment: dict

    def summary(self) -> dict:
        return {
            "schema": 1, "kind": "register_projection", "state_version": 6,
            "bindings": self.bindings, "fields": _json_state(self.fields),
            "environment": _json_state(self.environment),
        }

    def c_initializer(self) -> str:
        """Use the source export's declaration; duplicate no layout offsets."""
        def value(item):
            if isinstance(item, dict):
                return "{ " + ", ".join(f".{key} = {value(v)}" for key, v in item.items()) + " }"
            if isinstance(item, bytes):
                return "{ " + ", ".join(f"0x{byte:02x}" for byte in item) + " }"
            if isinstance(item, list):
                return "{\n        " + ",\n        ".join(value(v) for v in item) + "\n    }"
            return f"UINT64_C(0x{item:x})"

        fields = ",\n".join(f"    .{key} = {value(v)}" for key, v in self.fields.items())
        return (
            "/* Register banks only; RAM, paging, pending events and other\n"
            " * environment state still require their own binding. Include the\n"
            " * exported neverd_x64_state_v6 declaration before this initializer.\n"
            " * Undefined contains caller choices, not captured register data. */\n"
            "const neverd_x64_state_v6 neverd_captured_state_v6 = {\n" + fields + "\n};\n"
        )


def _captured_cpu(snapshot: QemuSaveSnapshot, profile: str, cpu: int,
                  expected_pc: int) -> tuple[dict, list[dict]]:
    """Share the producer, CPU version and original code-coordinate checks."""
    if profile != PROFILE:
        raise SnapshotError("unsupported QEMU x64 register profile")
    try:
        domain = ET.fromstring(snapshot.domain_xml)
    except ET.ParseError as error:
        raise SnapshotError("invalid saved domain XML") from error
    machine = domain.find("os/type")
    if (domain.tag != "domain" or domain.get("type") != "kvm" or
            machine is None or machine.get("arch") != "x86_64"):
        raise SnapshotError("register projection requires a saved x86_64 KVM domain")
    snapshot.expect_pc(cpu, expected_pc)
    record = snapshot.cpus[cpu]
    if record["version"] != 12:
        raise SnapshotError("register profile requires CPU v12")
    fields = record["fields"]
    segments = [_segment(v) for v in _array(fields.get("env.segs"), 6, "segment bank")]
    # A saved 32-bit env.eip is an offset, not the V6 linear PC. CS.L and
    # EFER.LMA authenticate the coordinate system; no rebasing is performed.
    if not segments[1]["Attributes"] & 0x2000 or not _uint(fields, "env.efer") & 0x400:
        raise SnapshotError("V6 register projection requires captured 64-bit code")
    return record, segments


def project_v6(snapshot: QemuSaveSnapshot, *, profile: str, cpu: int, apic: int,
               expected_pc: int, undefined: list[int]) -> V6RegisterProjection:
    """Bind a captured CPU and its caller-identified APIC without moving PC/SP.

    The producer supplies the CPU/APIC association and QEMU interpretation
    profile. A projection preserves bank values, not all prerequisites for
    invoking a continuation; halted/pending state and other banks remain in
    environment. Undefined is an explicit choice for the first instruction.
    """
    record, segments = _captured_cpu(snapshot, profile, cpu, expected_pc)
    apic = _integer(apic, "APIC instance", 0xffffffff)
    if apic not in snapshot.apics:
        raise SnapshotError(f"APIC {apic} is absent")
    if snapshot.apics[apic]["version"] != 3:
        raise SnapshotError("register profile requires CPU v12 and APIC v3")
    fields = record["fields"]
    apic_fields = snapshot.apics[apic]["fields"]
    choices = [_integer(v, "undefined choice", 0xffffffffffffffff)
               for v in _array(undefined, 8, "undefined choices")]
    result = {
        "GPR": [_integer(v, "GPR", 0xffffffffffffffff)
                for v in _array(fields.get("env.regs"), 16, "GPR bank")],
        "RFlags": _uint(fields, "env.eflags"), "PC": expected_pc,
        "FSBase": segments[4]["Base"], "GSBase": segments[5]["Base"],
        "ZMM": _vectors(fields, snapshot.cpus[cpu]["subsection_versions"]),
        "Undefined": choices,
        "CR8": _uint(apic_fields, "tpr", 8) >> 4,
        "KernelGSBase": _uint(fields, "env.kernelgsbase"),
        "MXCSR": _uint(fields, "env.mxcsr", 32),
        "ReservedV2": 0, "ReservedTailV2": 0,
        "CR2": _uint(fields, "env.cr[2]"), "ReservedV4": 0,
        "STAR": _uint(fields, "env.star"),
        "CS": segments[1], "SS": segments[2], "ReservedV5": 0,
    }
    for name, source in (("GDTR", "env.gdt"), ("IDTR", "env.idt")):
        table = _object(fields.get(source), source)
        result[name] = {"Base": _uint(table, "base"), "Limit": _uint(table, "limit", 16),
                        "Reserved": 0, "ReservedTail": 0}
    result.update({"LDTR": _segment(fields.get("env.ldt")),
                   "TR": _segment(fields.get("env.tr")),
                   "DS": segments[3], "ES": segments[0]})
    for name, segment in (("FS", segments[4]), ("GS", segments[5])):
        result[name] = {key: value for key, value in segment.items() if key != "Base"}
    # QEMU's KVM save/restore representation collapses !present and unusable.
    # Use its restoration-equivalent bit, preserving the ignored cache bytes.
    caches = [result[name] for name in ("CS", "SS", "DS", "ES", "FS", "GS", "LDTR", "TR")]
    result["Unusable"] = sum(1 << bit for bit, seg in enumerate(caches)
                             if not seg["Attributes"] & 0x80)
    # NMI masking is separate from an injected or pending NMI.
    result["NMIBlocked"] = (_uint(fields, "env.hflags2", 32) >> 2) & 1
    result["ReservedV6"] = bytes(14)
    return V6RegisterProjection(
        result,
        {"profile": profile, "cpu_instance": cpu, "apic_instance": apic,
         "expected_pc": hex(expected_pc), "undefined_source": "caller_choices"},
        {"cpu": snapshot.cpus[cpu], "apic": snapshot.apics[apic],
         "cpu_common": snapshot.cpu_common.get(cpu)},
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--profile", choices=[PROFILE], required=True)
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--apic", type=int, required=True,
                        help="producer-bound APIC migration instance for this CPU")
    parser.add_argument("--expect-pc", type=lambda v: int(v, 0), required=True)
    parser.add_argument("--undefined", type=lambda v: int(v, 0), nargs=8, required=True,
                        help="explicit arbitrary-effect choices, not captured inputs")
    parser.add_argument("--c-initializer", action="store_true")
    args = parser.parse_args(argv)
    try:
        with QemuSaveSnapshot(args.save) as snapshot:
            projection = project_v6(snapshot, profile=args.profile, cpu=args.cpu,
                                    apic=args.apic, expected_pc=args.expect_pc,
                                    undefined=args.undefined)
            if args.c_initializer:
                print(projection.c_initializer(), end="")
            else:
                json.dump(projection.summary(), sys.stdout, indent=2)
                print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-x64: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
