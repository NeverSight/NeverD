#!/usr/bin/env python3
"""Generate tiny architecture-only PE fixtures in a build directory."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


_TARGETS = {
    "x86": (0x014C, 0x10B, b"\xc3"),
    "x86_64": (0x8664, 0x20B, b"\xc3"),
    "arm": (0x01C4, 0x10B, b"\x70\x47"),
    "aarch64": (0xAA64, 0x20B, b"\xc0\x03\x5f\xd6"),
}


def _minimal_pe(architecture: str) -> bytes:
    machine, magic, code = _TARGETS[architecture]
    pe32_plus = magic == 0x20B
    optional_size = 0xF0 if pe32_plus else 0xE0
    data = bytearray(0x400)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)

    pe = 0x80
    data[pe : pe + 4] = b"PE\0\0"
    characteristics = 0x0022 if pe32_plus else 0x0102
    struct.pack_into(
        "<HHIIIHH",
        data,
        pe + 4,
        machine,
        1,
        0,
        0,
        0,
        optional_size,
        characteristics,
    )
    optional = pe + 24
    struct.pack_into("<H", data, optional, magic)
    struct.pack_into("<I", data, optional + 4, 0x200)
    entry_rva = 0x1001 if architecture == "arm" else 0x1000
    struct.pack_into("<II", data, optional + 16, entry_rva, 0x1000)
    if pe32_plus:
        struct.pack_into("<Q", data, optional + 24, 0x140000000)
        directory_count_offset = optional + 108
    else:
        struct.pack_into("<II", data, optional + 24, 0x1000, 0x400000)
        directory_count_offset = optional + 92
    struct.pack_into("<II", data, optional + 32, 0x1000, 0x200)
    struct.pack_into("<II", data, optional + 56, 0x2000, 0x200)
    struct.pack_into("<H", data, optional + 68, 3)
    struct.pack_into("<I", data, directory_count_offset, 16)

    section = optional + optional_size
    data[section : section + 8] = b".text\0\0\0"
    struct.pack_into(
        "<IIIIIIHHI",
        data,
        section + 8,
        len(code),
        0x1000,
        0x200,
        0x200,
        0,
        0,
        0,
        0,
        0x60000020,
    )
    data[0x200 : 0x200 + len(code)] = code
    return bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    artifacts = []
    for architecture in _TARGETS:
        relative = Path("artifacts") / f"{architecture}.exe"
        payload = _minimal_pe(architecture)
        path = args.output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        artifacts.append(
            {
                "path": relative.as_posix(),
                "sha256": hashlib.sha256(payload).hexdigest(),
                "architecture": architecture,
            }
        )

    manifest = {"schema_version": 1, "artifacts": artifacts}
    (args.output / "synthetic.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
