"""Carry the selected native x86 MSVC link libraries to the PE32 cross-linker."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

LIBRARIES = ("msvcrt.lib", "vcruntime.lib", "msvcprt.lib", "ucrt.lib",
             "oldnames.lib", "kernel32.lib")


def validate_manifest(manifest: dict) -> None:
    if not isinstance(manifest, dict):
        raise ValueError("missing native x86 MSVC library manifest")
    files = manifest.get("files", [])
    if manifest.get("schema") != 1 or manifest.get("architecture") != "x86" or \
            not isinstance(manifest.get("toolset"), str) or not manifest["toolset"] or \
            not isinstance(files, list) or len(files) != len(LIBRARIES):
        raise ValueError("incomplete native x86 MSVC library manifest")
    for name, record in zip(LIBRARIES, files):
        if not isinstance(record, dict) or record.get("name") != name:
            raise ValueError("native x86 MSVC library set differs")
        digest = record.get("sha256", "")
        if not isinstance(digest, str) or len(digest) != 64 or \
                any(c not in "0123456789abcdef" for c in digest):
            raise ValueError("invalid native MSVC library digest")


def library_data(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) <= 8 or not data.startswith(b"!<arch>\n"):
        raise ValueError(f"native MSVC library is not a COFF archive: {path}")
    return data


def capture_libraries(root: Path, directories: list[Path], toolset: str) -> dict:
    """Use the compiler's LIB search order, without substituting import stubs."""
    selected = {}
    for name in LIBRARIES:
        path = next((directory / name for directory in directories
                     if (directory / name).is_file()), None)
        if path is None:
            raise ValueError(f"selected x86 MSVC toolchain has no {name}")
        selected[name] = library_data(path)
    manifest = {"schema": 1, "architecture": "x86", "toolset": toolset,
                "files": [{"name": name, "sha256": hashlib.sha256(data).hexdigest()}
                          for name, data in selected.items()]}
    validate_manifest(manifest)
    root.mkdir(parents=True, exist_ok=True)
    for name, data in selected.items():
        (root / name).write_bytes(data)
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def load_libraries(root: Path) -> tuple[list[Path], dict]:
    manifest = json.loads((root / "manifest.json").read_text())
    validate_manifest(manifest)
    paths = []
    for record in manifest["files"]:
        path = root / record["name"]
        if hashlib.sha256(library_data(path)).hexdigest() != record["sha256"]:
            raise ValueError(f"native MSVC library digest differs: {path}")
        paths.append(path)
    return paths, manifest
