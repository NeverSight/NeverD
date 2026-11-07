"""Project captured x64 paging/debug banks with explicit unsaved inputs.

This offline adapter emits existing NeverD C++ state types. It does not create
an execution context, infer CPU features, walk pages or select pending events.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import sys

from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, _integer, _json_state
from scripts.qemu_save_x64 import PROFILE, _array, _captured_cpu, _subsection, _uint


@dataclass
class X64ControlProjection:
    """Typed bank images and their provenance, not runtime admission."""

    paging: dict
    debug: dict
    bindings: dict
    origins: dict
    environment: dict

    def summary(self) -> dict:
        return {
            "schema": 1, "kind": "x64_control_projection",
            "bindings": self.bindings, "origins": self.origins,
            "paging": _json_state(self.paging), "debug": _json_state(self.debug),
            "environment": _json_state(self.environment),
        }

    def cpp_initializer(self) -> str:
        """Assign existing fields by name; require constant initialization."""
        def bank(typename, name, fields):
            lines = [f"constexpr {typename} {name} = [] {{", f"    {typename} value{{}};"]
            for key, item in fields.items():
                value = ("{ " + ", ".join(f"UINT64_C(0x{x:x})" for x in item) + " }"
                         if isinstance(item, list) else f"UINT64_C(0x{item:x})")
                lines.append(f"    value.{key} = {value};")
            return "\n".join(lines + ["    return value;", "}();", ""])

        return (
            "/* Bank transport only. Bind CPU features, owned RAM and execution\n"
            " * policies separately, then use the existing runtime validators.\n"
            " * DebugCTL/PendingData are producer bindings, not captured fields.\n"
            f" * PDPTE origin: {self.origins['paging.PDPTE']}. */\n"
            '#include "neverd/translate/X86/X86Paging.h"\n'
            '#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"\n'
            "#include <cstdint>\n\n" +
            bank("neverd::translate::X86PagingState", "neverd_captured_paging", self.paging) +
            bank("neverd::analysis::X64PagedDebugState", "neverd_bound_debug", self.debug)
        )


def project_controls(snapshot: QemuSaveSnapshot, *, profile: str, cpu: int,
                     expected_pc: int, debugctl: int, pending_data: int,
                     missing_pdpte: list[int] | None = None) -> X64ControlProjection:
    """Retain captured bits; require explicit input for unsaved bank members.

    QEMU omits PKRU/PKRS iff zero. PDPTR omission instead means the cached
    bank is invalid, not that its bytes were zero. A caller must bind that
    inactive bank explicitly; a saved bank cannot be overridden. DebugCTL
    and pending data-breakpoint causes are not serialized by this profile.
    """
    record, _ = _captured_cpu(snapshot, profile, cpu, expected_pc)
    fields = record["fields"]
    if type(fields.get("env.a20_mask")) is not int or fields["env.a20_mask"] != -1:
        raise SnapshotError("control projection requires captured enabled A20")
    paging = {name: _uint(fields, f"env.cr[{index}]")
              for name, index in (("CR0", 0), ("CR3", 3), ("CR4", 4))}
    paging.update(EFER=_uint(fields, "env.efer"), RFlags=_uint(fields, "env.eflags"))
    origins = {f"paging.{name}": "captured" for name in paging}
    for name in ("PKRU", "PKRS"):
        key = name.lower()
        section = _subsection(record, f"cpu/{key}")
        paging[name] = 0 if section is None else _uint(section, f"env.{key}", 32)
        origins[f"paging.{name}"] = "omitted-zero" if section is None else "captured"
    pdptrs = _subsection(record, "cpu/pdptrs")
    if pdptrs is None:
        values = _array(missing_pdpte, 4, "explicit missing PDPTE binding")
        origins["paging.PDPTE"] = "producer-bound-inactive-bank"
    else:
        if missing_pdpte is not None:
            raise SnapshotError("cannot override captured PDPTE bank")
        values = _array(pdptrs.get("env.pdptrs"), 4, "captured PDPTE bank")
        origins["paging.PDPTE"] = "captured"
    paging["PDPTE"] = [_integer(value, "PDPTE", 0xffffffffffffffff) for value in values]
    registers = [_integer(value, "debug register", 0xffffffffffffffff)
                 for value in _array(fields.get("env.dr"), 8, "debug register bank")]
    debug = {
        "DR6": registers[6], "DR7": registers[7],
        "DebugCTL": _integer(debugctl, "producer DebugCTL binding", 0xffffffffffffffff),
        "DR": registers[:4],
        "PendingData": _integer(pending_data, "producer pending-data binding", 0xf),
    }
    origins.update({"debug.DR": "captured", "debug.DR6": "captured", "debug.DR7": "captured",
                    "debug.DebugCTL": "producer-bound", "debug.PendingData": "producer-bound"})
    return X64ControlProjection(
        paging, debug, {"profile": profile, "cpu_instance": cpu, "expected_pc": hex(expected_pc)},
        origins, {"cpu": record, "cpu_common": snapshot.cpu_common.get(cpu)})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--profile", required=True, choices=[PROFILE])
    parser.add_argument("--cpu", required=True, type=int)
    parser.add_argument("--expect-pc", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--debugctl", required=True, type=lambda value: int(value, 0),
                        help="producer-bound unsaved IA32_DEBUGCTL")
    parser.add_argument("--pending-data", required=True, type=lambda value: int(value, 0),
                        help="producer-bound unsaved pending data-breakpoint causes")
    parser.add_argument("--missing-pdpte", nargs=4, type=lambda value: int(value, 0),
                        help="explicit inactive bank, only when cpu/pdptrs is absent")
    parser.add_argument("--cpp-initializer", action="store_true")
    args = parser.parse_args(argv)
    try:
        with QemuSaveSnapshot(args.save) as snapshot:
            projection = project_controls(
                snapshot, profile=args.profile, cpu=args.cpu, expected_pc=args.expect_pc,
                debugctl=args.debugctl, pending_data=args.pending_data, missing_pdpte=args.missing_pdpte)
            if args.cpp_initializer:
                print(projection.cpp_initializer(), end="")
            else:
                json.dump(projection.summary(), sys.stdout, indent=2)
                print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-controls: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
