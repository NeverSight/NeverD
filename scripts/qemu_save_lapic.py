"""Project saved local APIC banks into the owned fixed-interrupt component.

This is bank transport, not a complete event producer or an executable capture.
Arbitration and bank transitions have one owner in X86LocalApic.h; this Python
adapter never selects or acknowledges a vector. See docs/qemu-save-snapshot.md.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import sys

from scripts.qemu_save_events import X64EventProjection, project_events
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, _json_state
from scripts.qemu_save_x64 import PROFILE


DELIVERY_PROFILE = "enabled-lapic-fixed-normal-eoi"


@dataclass
class LocalApicFixedProjection:
    fields: dict
    bindings: dict
    environment: X64EventProjection

    def summary(self) -> dict:
        return {"schema": 1, "kind": "local_apic_fixed_projection",
                "fields": _json_state(self.fields), "bindings": self.bindings,
                "environment": self.environment.summary()}

    def cpp_initializer(self) -> str:
        """Use the owning type/validator, with no duplicated offsets or rules."""
        lines = [
            "/* Captured banks; the producer separately binds enabled fixed",
            " * delivery with ordinary EOI, CPU admission, TPR/CR8 updates and",
            " * external EOI routing. Retain the JSON environment evidence.",
            " * No IRQ is selected/acknowledged by this constant initializer. */",
            '#include "neverd/translate/X86/X86LocalApic.h"', "",
            "constexpr auto neverd_captured_lapic_fixed =",
            "    neverd::translate::X86LocalApicFixed::fromState([] {",
            "        neverd::translate::X86LocalApicFixedState value{};",
        ]
        for name in ("IRR", "ISR", "TMR"):
            values = ", ".join(f"0x{value:08x}U" for value in self.fields[name])
            lines.append(f"        value.{name} = {{ {values} }};")
        lines += [f"        value.TPR = 0x{self.fields['TPR']:02x};",
                  "        value.EOIBroadcastSuppressed = " +
                  ("true;" if self.fields["EOIBroadcastSuppressed"] else "false;"),
                  "        return value;", "    }());",
                  'static_assert(neverd_captured_lapic_fixed.has_value(),',
                  '              "unsupported captured fixed-interrupt APIC bank");', ""]
        return "\n".join(lines)


def project_fixed_lapic(snapshot: QemuSaveSnapshot, *, profile: str, delivery_profile: str,
                        cpu: int, apic: int, expected_pc: int) -> LocalApicFixedProjection:
    """Bind the limited delivery model, retaining all other event evidence.

    The delivery profile is a producer obligation, not a discovered property.
    Captured enable bits must agree with it. Unknown instruction shadows and
    pending/injected events remain in environment; they do not become zero or
    disappear when the APIC banks are copied. The C++ owner admits bank bits.
    """
    if delivery_profile != DELIVERY_PROFILE:
        raise SnapshotError("unsupported local APIC delivery profile")
    evidence = project_events(snapshot, profile=profile, cpu=cpu, apic=apic,
                              expected_pc=expected_pc)
    bank = evidence.apic
    if not bank["Base"] & 0x800 or not bank["SpuriousVector"] & 0x100:
        raise SnapshotError("fixed-interrupt profile requires captured enabled local APIC")
    fields = {name: list(bank[name]) for name in ("IRR", "ISR", "TMR")}
    fields.update(TPR=bank["TPR"],
                  EOIBroadcastSuppressed=bool(bank["SpuriousVector"] & 0x1000))
    return LocalApicFixedProjection(
        fields, {**evidence.bindings, "delivery_profile": delivery_profile,
                 "delivery_profile_origin": "producer-bound"}, evidence)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--profile", choices=[PROFILE], required=True)
    parser.add_argument("--delivery-profile", choices=[DELIVERY_PROFILE], required=True)
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--apic", type=int, required=True)
    parser.add_argument("--expect-pc", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--cpp-initializer", action="store_true")
    args = parser.parse_args(argv)
    try:
        with QemuSaveSnapshot(args.save) as snapshot:
            projected = project_fixed_lapic(
                snapshot, profile=args.profile, delivery_profile=args.delivery_profile,
                cpu=args.cpu, apic=args.apic, expected_pc=args.expect_pc)
            if args.cpp_initializer:
                print(projected.cpp_initializer(), end="")
            else:
                json.dump(projected.summary(), sys.stdout, indent=2)
                print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-lapic: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
