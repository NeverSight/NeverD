"""Capture and authenticate the Microsoft x86 CRT used by catch-search probes."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct

NAME = "vcruntime140.dll"


def runtime_data(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("catch search requires a native x86 runtime DLL")
    header = struct.unpack_from("<I", data, 60)[0]
    if header > len(data) - 26 or data[header:header + 4] != b"PE\0\0" or \
            struct.unpack_from("<H", data, header + 4)[0] != 0x14c or \
            struct.unpack_from("<H", data, header + 24)[0] != 0x10b or \
            not struct.unpack_from("<H", data, header + 22)[0] & 0x2000:
        raise ValueError("catch search runtime is not a PE32 x86 DLL")
    return data


def validate_manifest(manifest: dict) -> None:
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or \
            manifest.get("provider") != "Microsoft Visual C++" or \
            manifest.get("architecture") != "x86" or manifest.get("name") != NAME or \
            not isinstance(manifest.get("toolset"), str) or not manifest["toolset"]:
        raise ValueError("catch search has no captured Microsoft x86 runtime")
    digest = manifest.get("sha256", "")
    if not isinstance(digest, str) or len(digest) != 64 or \
            any(c not in "0123456789abcdef" for c in digest):
        raise ValueError("catch search runtime digest is invalid")


def capture_runtime(root: Path, redist: Path, toolset: str) -> dict:
    sources = list(redist.glob("x86/Microsoft.VC*.CRT/" + NAME))
    if len(sources) != 1:
        raise ValueError("selected MSVC redistributable has no unique x86 runtime")
    data = runtime_data(sources[0])
    manifest = {"schema": 1, "provider": "Microsoft Visual C++", "architecture": "x86",
                "name": NAME, "toolset": toolset, "sha256": hashlib.sha256(data).hexdigest()}
    validate_manifest(manifest)
    root.mkdir(parents=True, exist_ok=True)
    (root / NAME).write_bytes(data)
    (root / "runtime.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def validate_runtime(root: Path, manifest: dict) -> Path:
    validate_manifest(manifest)
    path = root / NAME
    if hashlib.sha256(runtime_data(path)).hexdigest() != manifest["sha256"]:
        raise ValueError("catch search runtime bytes changed")
    return path


def load_runtime(root: Path) -> tuple[Path, dict]:
    manifest = json.loads((root / "runtime.json").read_text())
    return validate_runtime(root, manifest), manifest
