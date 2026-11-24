"""Bind an immediate RTM step to its original saved x64 vCPU and RIP."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, _integer
from scripts.qemu_save_x64 import PROFILE, _captured_code_segments, _check_x64_domain, _uint


def project_rtm(snapshot: QemuSaveSnapshot, *, profile: str, expected_pc: int,
                cpu: int | None = None, load_base: int | None = None) -> dict:
    """Project the observed word; the exporter owns instruction and MSR semantics."""
    _check_x64_domain(snapshot, profile)
    matches = snapshot.cpus_at_x64_rip(expected_pc, cpu)
    cpu = next(matches, None)
    if cpu is None:
        raise SnapshotError("no saved vCPU at the requested RIP")
    if next(matches, None) is not None:
        raise SnapshotError("multiple saved vCPUs at the requested RIP; select --cpu")
    record = snapshot.read_device("cpu", cpu)
    _captured_code_segments(record)
    value = None
    if snapshot.has_cpu_subsection_u32(cpu, "cpu/msr_tsx_ctrl", "env.tsx_ctrl"):
        value = _uint(record["fields"]["subsections"]["cpu/msr_tsx_ctrl"], "env.tsx_ctrl", 32)
    site = {"cpu": cpu, "rip": hex(expected_pc), "value": value}
    if load_base is not None:
        site["load_base"] = hex(_integer(load_base, "original load base", expected_pc))
    return {"native_rtm_abort_steps": {"qemu_tsx_ctrl_v1": site}}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--profile", required=True, choices=[PROFILE])
    parser.add_argument("--expect-pc", required=True, type=lambda v: int(v, 0),
                        help="original XBEGIN virtual address in this save")
    parser.add_argument("--cpu", type=int, help="migration instance if multiple vCPUs match")
    parser.add_argument("--load-base", type=lambda v: int(v, 0),
                        help="known original mapping base when the export image is relocated")
    args = parser.parse_args(argv)
    try:
        with QemuSaveSnapshot(args.save) as snapshot:
            binding = project_rtm(snapshot, profile=args.profile, expected_pc=args.expect_pc,
                                  cpu=args.cpu, load_base=args.load_base)
        print(json.dumps(binding))
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-rtm: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
