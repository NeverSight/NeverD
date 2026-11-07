#!/usr/bin/env python3
"""Check a supplied main-RAM map against captured QEMU 10.2 Q35 MCH state.

This is a system-address-space observation check, not topology discovery or
an SMM/device execution model. Unsaved machine properties are explicit producer
bindings. Other device overlays remain the map producer's responsibility.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from scripts.qemu_save_memory import QemuPhysicalMemory
from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, _integer, _name


PROFILE = "qemu-10.2-q35-main-ram"


def check_main_ram(snapshot: QemuSaveSnapshot, memory: QemuPhysicalMemory, *,
                   profile: str, mch_name: str, mch_instance: int, ram_block: str,
                   below_4g: int, above_4g_start: int, smm_ranges: bool,
                   extended_tseg_mib: int) -> dict:
    """Check the complete supplied map once; add no per-read work.

    The explicit profile and machine properties bind interpretation, not the
    saving executable's identity. Only main RAM outside the first MiB is
    supported. Success proves consistency with these MCH constraints, not
    absence of other overlays, CPU access permission or invocation capture.
    """
    if profile != PROFILE or snapshot.machine != "pc-q35-10.2":
        raise SnapshotError("unsupported Q35 main-RAM profile/machine")
    if memory._snapshot is not snapshot:
        raise SnapshotError("Q35 map and device evidence must use the same snapshot")
    ram_block = _name(ram_block, "main RAM block")
    blocks = snapshot.summary()["ram_blocks"]
    if ram_block not in blocks:
        raise SnapshotError(f"main RAM block {ram_block!r} is absent")
    ram_size = blocks[ram_block]["size"]
    below_4g = _integer(below_4g, "below-4G RAM size", 0xb0000000)
    above_4g_start = _integer(above_4g_start, "above-4G RAM base", 0xffffffffffffffff)
    extended_tseg_mib = _integer(extended_tseg_mib, "extended TSEG MiB", 0xfff)
    if type(smm_ranges) is not bool:
        raise SnapshotError("SMM ranges binding must be a boolean")
    if below_4g < 0x100000 or below_4g > ram_size or below_4g % snapshot.page_size:
        raise SnapshotError("invalid below-4G RAM extent")
    if above_4g_start < 0x100000000 or above_4g_start % snapshot.page_size:
        raise SnapshotError("invalid above-4G RAM base")
    above_end = above_4g_start + ram_size - below_4g
    if above_end > 1 << 64:
        raise SnapshotError("above-4G RAM extent overflows uint64")

    mch = snapshot.read_device(mch_name, mch_instance)
    parent = mch["fields"].get("parent_obj")
    if (mch["vmsd_name"] != "mch" or mch["version"] != 1 or mch["subsection_versions"] or
            not isinstance(parent, dict) or type(parent.get("version_id")) is not int or
            parent["version_id"] != 2):
        raise SnapshotError("unsupported MCH/PCI layout version")
    config = parent.get("config[0]")
    if (not isinstance(config, bytes) or len(config) != 256 or
            config[:4] != b"\x86\x80\xc0\x29"):
        raise SnapshotError("missing or mismatched Q35 MCH PCI configuration")

    # Exclude the legacy area as a whole: PAM, VGA and SMBASE reads need their
    # own mapping interpretation, even when some individual bytes are RAM.
    excluded = [("legacy-first-mib", 0, 0x100000)]
    smram, esmramc = config[0x9d], config[0x9e]
    tseg_size = 0
    if smm_ranges and smram & 8 and esmramc & 1:
        # Extended mode uses the unsaved QOM property, NOT config[0x50:0x52],
        # whose guest-writable value can be a query or unrelated value.
        tseg_size = (1, 2, 8, extended_tseg_mib)[(esmramc >> 1) & 3] << 20
        if tseg_size > below_4g:
            raise SnapshotError("TSEG exceeds bound below-4G RAM")
        if tseg_size:
            excluded.append(("tseg", below_4g - tseg_size, below_4g))

    pciexbar = int.from_bytes(config[0x60:0x68], "little")
    length_code = (pciexbar >> 1) & 3
    if length_code == 3:
        # QEMU leaves the previous mapping in place for reserved LENGTH. The
        # saved register alone cannot recover that mapping, even with EN=0.
        raise SnapshotError("reserved PCIEXBAR length leaves mapping history unknown")
    if pciexbar & 1:
        # Match this QEMU profile's address masks, including its low mask bits;
        # do not substitute a hardware alignment rule for the saved producer.
        mask = 0xff0000000 | (0, 0x06000000, 0x02000000)[length_code]
        start = pciexbar & mask
        excluded.append(("pcie-ecam", start, start + (0x10000000 >> length_code)))

    mapping = memory.summary()
    for row in mapping["regions"]:
        start, size, offset = (int(row[key], 16)
                               for key in ("physical_start", "size", "block_offset"))
        end = start + size
        if row["ram_block"] != ram_block:
            raise SnapshotError("Q35 main-RAM map contains a different RAM block")
        if end <= below_4g:
            expected_offset = start
        elif above_4g_start <= start and end <= above_end:
            expected_offset = below_4g + start - above_4g_start
        else:
            raise SnapshotError(f"region {start:#x} is outside bound Q35 main RAM")
        if offset != expected_offset:
            raise SnapshotError(f"region {start:#x} has the wrong original RAM-block offset")
        for name, first, last in excluded:
            if start < last and first < end:
                raise SnapshotError(f"region {start:#x} overlaps Q35 {name}")

    return {
        "schema": 1, "profile": PROFILE, "scope": "main-ram-mch-consistency",
        "machine": snapshot.machine,
        "mch": {"name": mch["name"], "instance_id": mch["instance_id"],
                "section_id": mch["section_id"], "version": mch["version"],
                "config_sha256": hashlib.sha256(config).hexdigest()},
        "bindings": {"ram_block": ram_block, "below_4g": hex(below_4g),
                     "above_4g_start": hex(above_4g_start), "smm_ranges": smm_ranges,
                     "extended_tseg_mib": extended_tseg_mib},
        "tseg_bytes": hex(tseg_size),
        "excluded": [{"name": name, "start": hex(start), "size": hex(end - start)}
                     for name, start, end in excluded],
        "checked_regions": len(mapping["regions"]),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--map", required=True, type=Path, dest="mapping")
    parser.add_argument("--profile", required=True, choices=[PROFILE])
    parser.add_argument("--mch", required=True, nargs=2, metavar=("NAME", "INSTANCE"))
    parser.add_argument("--ram-block", required=True)
    parser.add_argument("--below-4g", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--above-4g-start", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--smm-ranges", required=True, choices=["on", "off"])
    parser.add_argument("--extended-tseg-mib", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--physical", nargs=2, metavar=("ADDRESS", "SIZE"),
                        type=lambda value: int(value, 0), help="read after checking the entire map")
    args = parser.parse_args(argv)
    try:
        try:
            instance = int(args.mch[1], 0)
        except ValueError as error:
            raise SnapshotError("MCH instance must be a decimal or prefixed integer") from error
        with QemuSaveSnapshot(args.save) as snapshot:
            memory = QemuPhysicalMemory.from_file(snapshot, args.mapping)
            evidence = check_main_ram(
                snapshot, memory, profile=args.profile, mch_name=args.mch[0], mch_instance=instance,
                ram_block=args.ram_block, below_4g=args.below_4g, above_4g_start=args.above_4g_start,
                smm_ranges=args.smm_ranges == "on", extended_tseg_mib=args.extended_tseg_mib)
            result = {"validation": evidence, "mapping": memory.summary()}
            if args.physical:
                address, size = args.physical
                data = memory.read_physical(address, size)
                result["read"] = {"physical_address": hex(address), "size": size, "hex": data.hex()}
            json.dump(result, sys.stdout, indent=2)
            print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-q35: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
