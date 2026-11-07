#!/usr/bin/env python3
"""Read saved bytes through an explicitly supplied physical RAM mapping.

The producer supplies the effective, flattened mapping for the captured address
space. This module does not infer chipset mappings, run devices or page tables,
or certify CPU access permissions. See docs/qemu-save-snapshot.md.
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
from dataclasses import dataclass
import json
from pathlib import Path
import re
import sys

from scripts.qemu_save_snapshot import (
    QemuSaveSnapshot, SnapshotError, _integer, _json_constant, _json_object,
    _list, _name, _object,
)


def _uint(value, label: str) -> int:
    if isinstance(value, str) and re.fullmatch(r"0x[0-9a-fA-F]{1,16}", value):
        value = int(value, 16)
    return _integer(value, label, 0xffffffffffffffff)


def _fields(value, keys: set[str], label: str) -> dict:
    value = _object(value, label)
    if value.keys() != keys:
        raise SnapshotError(f"{label} fields must be {', '.join(sorted(keys))}")
    return value


@dataclass(frozen=True)
class _Region:
    start: int
    end: int
    block: str
    offset: int


class QemuPhysicalMemory:
    """Borrow one save's RAM index and build a sorted, immutable mapping index.

    Machine/block checks detect mismatched bindings, not whether the supplied
    mapping describes the actual chipset. The producer must flatten overlays
    and exclude MMIO, holes and unavailable regions for the selected address
    space. Aliases retain their original RAM-block offsets. Keep the snapshot
    open and its input file immutable for this view's lifetime.
    """

    def __init__(self, snapshot: QemuSaveSnapshot, description: dict):
        description = _fields(description, {"schema", "machine", "ram_blocks", "regions"},
                              "physical map")
        if type(description["schema"]) is not int or description["schema"] != 1:
            raise SnapshotError("unsupported physical map schema")
        machine = _name(description["machine"], "physical map machine")
        if machine != snapshot.machine:
            raise SnapshotError("physical map machine mismatch")
        available = snapshot.summary()["ram_blocks"]
        blocks = {}
        for name, size in _object(description["ram_blocks"], "RAM-block bindings").items():
            name = _name(name, "RAM-block name")
            size = _uint(size, "bound RAM-block size")
            if name not in available or size != available[name]["size"]:
                raise SnapshotError(f"RAM-block binding mismatch for {name!r}")
            blocks[name] = size
        rows = _list(description["regions"], "physical map regions")
        if len(rows) > snapshot.limits.physical_regions:
            raise SnapshotError("physical map region count exceeds inspection limit")
        regions = []
        for row in rows:
            row = _fields(row, {"physical_start", "size", "ram_block", "block_offset"},
                          "physical region")
            start = _uint(row["physical_start"], "physical region start")
            size = _uint(row["size"], "physical region size")
            offset = _uint(row["block_offset"], "RAM-block offset")
            block = _name(row["ram_block"], "region RAM-block name")
            if block not in blocks:
                raise SnapshotError(f"region RAM block {block!r} has no size binding")
            if not size or size > (1 << 64) - start:
                raise SnapshotError("empty or overflowing physical region")
            if offset > blocks[block] or size > blocks[block] - offset:
                raise SnapshotError("physical region exceeds RAM-block extent")
            regions.append(_Region(start, start + size, block, offset))
        regions.sort(key=lambda region: region.start)
        for previous, current in zip(regions, regions[1:]):
            if previous.end > current.start:
                raise SnapshotError("overlapping physical regions; supply a flattened map")
        self._snapshot = snapshot
        self._machine = machine
        self._blocks = tuple(sorted(blocks.items()))
        self._regions = tuple(regions)
        self._starts = tuple(region.start for region in regions)

    @classmethod
    def from_file(cls, snapshot: QemuSaveSnapshot, path: str | Path) -> QemuPhysicalMemory:
        """Load bounded UTF-8 JSON; keys and integers obey the same API validator."""
        with open(path, "rb") as source:
            data = source.read(snapshot.limits.metadata_bytes + 1)
        if len(data) > snapshot.limits.metadata_bytes:
            raise SnapshotError("physical map JSON exceeds inspection limit")
        try:
            description = json.loads(data.decode("utf-8"), object_pairs_hook=_json_object,
                                     parse_constant=_json_constant)
        except (UnicodeError, ValueError, RecursionError) as error:
            raise SnapshotError(f"invalid physical map JSON: {error}") from error
        return cls(snapshot, description)

    def read_physical(self, address: int, size: int) -> bytes:
        """Read a bounded physical interval; holes and missing saved pages fail.

        An empty read validates its uint64 address but needs no mapping. A
        failing read returns no prefix and does not alter the save or the map.
        """
        address = _integer(address, "physical read address", 0xffffffffffffffff)
        size = _integer(size, "physical read size", self._snapshot.limits.read_bytes)
        if size > (1 << 64) - address:
            raise SnapshotError("physical read overflows uint64 address space")
        index = bisect_right(self._starts, address) - 1
        result = bytearray()
        while size:
            if not 0 <= index < len(self._regions):
                raise SnapshotError(f"unmapped physical address {address:#x}")
            region = self._regions[index]
            if not region.start <= address < region.end:
                raise SnapshotError(f"unmapped physical address {address:#x}")
            amount = min(size, region.end - address)
            result.extend(self._snapshot.read_ram(
                region.block, region.offset + address - region.start, amount))
            address += amount
            size -= amount
            index += 1
        return bytes(result)

    def summary(self) -> dict:
        """Return a normalized copy of the producer's mapping, not topology proof."""
        return {
            "schema": 1, "machine": self._machine,
            "ram_blocks": {name: hex(size) for name, size in self._blocks},
            "regions": [{"physical_start": hex(region.start),
                         "size": hex(region.end - region.start),
                         "ram_block": region.block, "block_offset": hex(region.offset)}
                        for region in self._regions],
        }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--map", required=True, type=Path, dest="mapping",
                        help="producer-supplied flattened physical mapping JSON")
    parser.add_argument("--physical", required=True, nargs=2, metavar=("ADDRESS", "SIZE"),
                        type=lambda value: int(value, 0), help="read a bounded physical range")
    args = parser.parse_args(argv)
    try:
        with QemuSaveSnapshot(args.save) as snapshot:
            memory = QemuPhysicalMemory.from_file(snapshot, args.mapping)
            address, size = args.physical
            data = memory.read_physical(address, size)
            json.dump({"mapping": memory.summary(),
                       "read": {"physical_address": hex(address), "size": size,
                                "hex": data.hex()}}, sys.stdout, indent=2)
            print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-memory: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
