#!/usr/bin/env python3
"""Inspect selected device states in a completed libvirt/QEMU save offline.

With no --device selectors, list section identities without decoding auxiliary
device fields. No device model, post-load callback or guest instruction runs.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from scripts.qemu_save_snapshot import QemuSaveSnapshot, SnapshotError, _json_state


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("save", type=Path)
    parser.add_argument("--device", nargs=2, action="append", default=[],
                        metavar=("NAME", "INSTANCE"),
                        help="read this exact migration device/instance (repeatable)")
    args = parser.parse_args(argv)
    try:
        selected, seen = [], set()
        for name, instance in args.device:
            try:
                key = (name, int(instance, 0))
            except ValueError as error:
                raise SnapshotError("device instance must be a decimal or prefixed integer") from error
            if key not in seen:
                seen.add(key)
                selected.append(key)
        with QemuSaveSnapshot(args.save) as snapshot:
            devices = []
            for name, instance in selected:
                record = snapshot.read_device(name, instance)
                devices.append({**record, "fields": _json_state(record["fields"])})
            json.dump({"schema": 1, "machine": snapshot.machine,
                       "inventory": snapshot.device_inventory(), "devices": devices},
                      sys.stdout, indent=2)
            print()
    except (OSError, SnapshotError) as error:
        print(f"qemu-save-devices: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
