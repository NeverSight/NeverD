"""Interpret a captured QEMU/KVM x64 event boundary without delivering events.

QEMU's save/post-load contract, not the host CPU, owns these interpretations.
The implementation is independently authored from the documented formats in
docs/qemu-save-snapshot.md. No guest, device model or restore callback runs.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import sys

from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, _integer, _json_state
from scripts.qemu_save_x64 import (
    PROFILE, _array, _captured_apic, _captured_cpu, _subsection, _uint,
)


def _vector(fields: dict, name: str) -> int:
    value = fields.get(name)
    if type(value) is not int or not -1 <= value <= 255:
        raise SnapshotError(f"invalid saved event vector {name}")
    return value


def _exception(record: dict) -> tuple[dict, dict]:
    fields = record["fields"]
    vector = _vector(fields, "env.exception_nr")
    has_error = _uint(fields, "env.has_error_code", 1)
    error = _subsection(record, "cpu/error_code")
    if has_error != (error is not None):
        raise SnapshotError("exception error-code flag/subsection mismatch")
    error_code = None
    if error is not None:
        value = error.get("env.error_code")
        if type(value) is not int or not -(1 << 31) <= value < (1 << 31):
            raise SnapshotError("invalid signed exception error code")
        error_code = value & 0xffffffff
    info = _subsection(record, "cpu/exception_info")
    pending = 0 if info is None else _uint(info, "env.exception_pending", 1)
    injected = 0 if info is None else _uint(info, "env.exception_injected", 1)
    has_payload = None if info is None else _uint(info, "env.exception_has_payload", 1)
    payload = None if info is None else _uint(info, "env.exception_payload")
    if vector == -1 and (pending or injected):
        raise SnapshotError("pending/injected exception has no vector")
    phase_origin = "omitted-no-vector" if info is None else "captured"
    # cpu_post_load restores this state even without exception_info. Conversely,
    # stale has_error_code/error_code do not create an event when nr is -1.
    if vector != -1 and not pending and not injected:
        injected = 1
        phase_origin = "qemu-post-load-injected"
    return ({"Vector": vector, "Pending": pending, "Injected": injected,
             "HasErrorCode": has_error, "ErrorCode": error_code,
             "HasPayload": has_payload, "Payload": payload},
            {"exception.phase": phase_origin,
             "exception.error_code": "captured" if error is not None else "not-saved",
             "exception.payload": "captured" if info is not None else "not-saved"})


@dataclass
class X64EventProjection:
    """Restore-equivalent event evidence and explicit scheduler input bindings.

    This is not a selected event, event queue or runtime admission certificate.
    Injected events may need reinjection; pending events still need arbitration.
    """

    events: dict
    activity: dict
    apic: dict
    interruptibility: dict
    bindings: dict
    origins: dict
    environment: dict

    def summary(self) -> dict:
        return {"schema": 1, "kind": "x64_event_projection", "bindings": self.bindings,
                "origins": self.origins, "events": _json_state(self.events),
                "activity": _json_state(self.activity), "apic": _json_state(self.apic),
                "interruptibility": _json_state(self.interruptibility),
                "environment": _json_state(self.environment)}


def project_events(snapshot: QemuSaveSnapshot, *, profile: str, cpu: int, apic: int,
                   expected_pc: int, sti_blocked: int | None = None,
                   ss_blocked: int | None = None,
                   nmi_blocked_by_sti: int | None = None) -> X64EventProjection:
    """Retain every saved event phase; do not consume, rank or deliver it.

    Unbound scheduler inputs remain None, never zero. QEMU's KVM path does not
    serialize interrupt.shadow; clear hflags cannot prove clear STI/MOV-SS
    shadows. NMIBlocked is independently captured in hflags2.
    """
    record, _ = _captured_cpu(snapshot, profile, cpu, expected_pc)
    apic_record = _captured_apic(snapshot, apic)
    common = snapshot.cpu_common.get(cpu)
    if common is None or common["version"] != 1:
        raise SnapshotError("event projection requires matching CPU-common v1")
    fields, common_fields = record["fields"], common["fields"]
    if _subsection(common, "cpu_common/exception_index") is not None:
        raise SnapshotError("TCG exception-index subsection in KVM event profile")
    crash = _subsection(common, "cpu_common/crash_occurred")
    triple = _subsection(record, "cpu/triple_fault")
    exception, origins = _exception(record)
    interrupt = _vector(fields, "env.interrupt_injected")
    flags, flags2 = _uint(fields, "env.hflags", 32), _uint(fields, "env.hflags2", 32)
    events = {
        "Exception": exception,
        "Interrupt": {"Vector": interrupt, "Injected": int(interrupt != -1),
                      "Soft": _uint(fields, "env.soft_interrupt", 1)},
        "NMI": {"Injected": _uint(fields, "env.nmi_injected", 1),
                "Pending": _uint(fields, "env.nmi_pending", 8), "Blocked": (flags2 >> 2) & 1},
        "TripleFaultPending": 0 if triple is None else _uint(triple, "env.triple_fault_pending", 1),
    }
    activity = {"Halted": _uint(common_fields, "halted", 1),
                "InterruptRequest": _uint(common_fields, "interrupt_request", 32),
                "MPState": _uint(fields, "env.mp_state", 32),
                "SIPIVector": _uint(fields, "env.sipi_vector", 32),
                "SMM": (flags >> 19) & 1, "GuestMode": (flags >> 21) & 1,
                "SMMInsideNMI": (flags2 >> 4) & 1,
                "CrashOccurred": 0 if crash is None else _uint(crash, "crash_occurred", 1)}
    apic_fields = apic_record["fields"]
    apic_state = {"Base": _uint(apic_fields, "apicbase", 32),
                  "TPR": _uint(apic_fields, "tpr", 8),
                  "SpuriousVector": _uint(apic_fields, "spurious_vec", 32)}
    for name in ("IRR", "ISR", "TMR"):
        apic_state[name] = [_integer(value, f"APIC {name} word", 0xffffffff)
                            for value in _array(apic_fields.get(name.lower()), 8, f"APIC {name}")]
    shadows = {}
    for name, value in (("STIBlocked", sti_blocked), ("SSBlocked", ss_blocked),
                        ("NMIBlockedBySTI", nmi_blocked_by_sti)):
        shadows[name] = None if value is None else _integer(value, f"producer {name} binding", 1)
        origins[f"interruptibility.{name}"] = "not-saved" if value is None else "producer-bound"
    origins.update({"events.TripleFaultPending": "omitted-zero" if triple is None else "captured",
                    "activity.CrashOccurred": "omitted-zero" if crash is None else "captured",
                    "events.NMI.Blocked": "captured-hflags2"})
    return X64EventProjection(
        events, activity, apic_state, shadows,
        {"profile": profile, "cpu_instance": cpu, "apic_instance": apic,
         "expected_pc": hex(expected_pc)}, origins,
        {"cpu": record, "cpu_common": common, "apic": apic_record})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--profile", choices=[PROFILE], required=True)
    parser.add_argument("--cpu", type=int, required=True)
    parser.add_argument("--apic", type=int, required=True)
    parser.add_argument("--expect-pc", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--sti-blocked", type=int, help="producer binding; otherwise unknown")
    parser.add_argument("--ss-blocked", type=int, help="producer binding; otherwise unknown")
    parser.add_argument("--nmi-blocked-by-sti", type=int, help="producer policy; otherwise unknown")
    args = parser.parse_args(argv)
    try:
        with QemuSaveSnapshot(args.save) as snapshot:
            projected = project_events(
                snapshot, profile=args.profile, cpu=args.cpu, apic=args.apic,
                expected_pc=args.expect_pc, sti_blocked=args.sti_blocked,
                ss_blocked=args.ss_blocked, nmi_blocked_by_sti=args.nmi_blocked_by_sti)
            json.dump(projected.summary(), sys.stdout, indent=2)
            print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-events: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
