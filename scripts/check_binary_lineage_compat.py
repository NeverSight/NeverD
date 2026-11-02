#!/usr/bin/env python3
"""Compare v1 lineage identity with a real legacy engine in separate processes.

The new engine writes v1 and v2 documents. The legacy engine must reopen v1
identically and refuse v2 while retaining its previous graph. Supply retained
libraries and a v1 collection whose source reports both engines understand.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TIMEOUT = 180.0
PAGE_SIZE = 200
HASH_BLOCK_BYTES = 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(HASH_BLOCK_BYTES), b""):
            value.update(block)
    return value.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def sample(args: argparse.Namespace) -> None:
    sys.path.insert(0, str(ROOT / "pluginsdk" / "python"))
    from neverd_plugin import Lineage, NeverDError
    from neverd_plugin.ffi import HostAPI

    library_hash = digest(args.library)
    directory = os.add_dll_directory(str(args.library.parent)) if hasattr(os, "add_dll_directory") else None
    library = ctypes.CDLL(str(args.library))
    refusals = {}
    require(hasattr(library, "neverd_lineage_edit_releases_json") == (args.role == "modern"),
            "expected a release-capable modern engine and a legacy engine without that export")
    with Lineage(host=HostAPI(library)) as graph:
        graph.load(args.collection.read_text(encoding="utf-8"))
        summary = graph.summary()
        require(json.loads(graph.export())["schema_version"] == 1, "input must be a v1 collection")
        versions = [row for page in graph.pages("versions", limit=PAGE_SIZE) for row in page["items"]]
        reports = [row for page in graph.pages("reports", limit=PAGE_SIZE) for row in page["items"]]
        sources = {r["id"]: hashlib.sha256(graph.report(r["id"]).encode()).hexdigest() for r in reports}
        identity = {"revision": summary["revision"], "versions": versions,
                    "reports": reports, "source_sha256": sources}
        exported = graph.export()
        if args.role == "modern":
            (args.output_dir / "modern-v1.ndlineage").write_text(exported, encoding="utf-8")
            graph.edit_releases({"upsert": [{"id": "compat-release", "label": "Compatibility release",
                "branch": "", "version": versions[0]["id"], "published_at": None, "parents": []}]})
            require(graph.summary()["document_schema_version"] == 2, "release edit did not upgrade document")
            require({r["id"]: hashlib.sha256(graph.report(r["id"]).encode()).hexdigest()
                     for r in reports} == sources, "release edit changed report bytes")
            released = graph.export()
            (args.output_dir / "modern-v2.ndlineage").write_text(released, encoding="utf-8")
            schema_only = json.loads(released)
            del schema_only["releases"]
            schema_only_text = json.dumps(schema_only)
            graph.load(schema_only_text)
            require(graph.summary()["document_schema_version"] == 2 and
                    graph.summary()["releases"] == 0,
                    "schema-only v2 control is not a valid empty release collection")
            (args.output_dir / "modern-v2-schema-only.ndlineage").write_text(
                schema_only_text, encoding="utf-8")
        else:
            graph.load((args.output_dir / "modern-v1.ndlineage").read_text(encoding="utf-8"))
            require(graph.export() == exported and graph.summary()["revision"] == summary["revision"],
                    "new v1 export changed legacy canonical persistence or identity")
            for name in ("modern-v2", "modern-v2-schema-only"):
                try:
                    graph.load((args.output_dir / (name + ".ndlineage")).read_text(encoding="utf-8"))
                except NeverDError as error:
                    message = str(error)
                    # Older readers can reject the new field before reaching
                    # the version check. The field-free control must still
                    # diagnose the unsupported version itself.
                    require("schema" in message or "version" in message or
                            (name == "modern-v2" and message == "unknown lineage field: releases"),
                            "v2 refusal did not identify unsupported schema/version or release field")
                    require(graph.export() == exported, "v2 refusal replaced the legacy graph")
                    refusals[name] = message
                else:
                    raise ValueError("legacy engine silently accepted " + name)
            try:
                graph.edit_releases({"upsert": []})
            except RuntimeError as error:
                require("does not export" in str(error), "missing optional export was not diagnosed")
                require(graph.export() == exported, "missing optional export changed the graph")
            else:
                raise ValueError("legacy engine unexpectedly edited releases")
    if directory is not None:
        directory.close()
    require(digest(args.library) == library_hash, "library changed during compatibility check")
    write_json(args.output_dir / (args.role + ".json"),
               {"validated": True, "library_sha256": library_hash, "identity": identity,
                "v2_refusals": refusals})


def run(args: argparse.Namespace) -> None:
    args.output_dir.mkdir(parents=True, exist_ok=False)
    inputs = {str(p): digest(p) for p in (args.library, args.legacy_library, args.collection, Path(__file__))}
    package = ROOT / "pluginsdk" / "python" / "neverd_plugin"
    bindings = {str(p.relative_to(package)): digest(p) for p in sorted(package.rglob("*.py"))}
    for role, library in (("modern", args.library), ("legacy", args.legacy_library)):
        command = [sys.executable, str(Path(__file__).resolve()), "--role", role,
                   "--library", str(library), "--collection", str(args.collection),
                   "--output-dir", str(args.output_dir)]
        with (args.output_dir / (role + ".log")).open("w") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
        require(result.returncode == 0, f"{role} compatibility process failed; see {role}.log")
    modern = json.loads((args.output_dir / "modern.json").read_text())
    legacy = json.loads((args.output_dir / "legacy.json").read_text())
    require(modern["identity"] == legacy["identity"], "v1 identity, metadata or source bytes changed")
    require(all(digest(Path(p)) == value for p, value in inputs.items()), "compatibility input changed")
    require(bindings == {str(p.relative_to(package)): digest(p) for p in sorted(package.rglob("*.py"))},
            "bindings changed during compatibility check")
    write_json(args.output_dir / "result.json", {"validated": True, "inputs_sha256": inputs,
        "bindings_sha256": bindings, "v1_revision": modern["identity"]["revision"],
        "source_reports": len(modern["identity"]["reports"]),
        "legacy_v2_refusals": legacy["v2_refusals"],
        "checks": ["v1 identity and original reports", "modern v1 export reopens in legacy engine",
                   "legacy release extension and schema-only v2 refusals retain v1",
                   "missing optional export retains graph operations"]})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--legacy-library", type=Path)
    parser.add_argument("--collection", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--role", choices=("modern", "legacy"), help=argparse.SUPPRESS)
    args = parser.parse_args()
    require(args.timeout > 0, "timeout must be positive")
    require(args.role is not None or args.legacy_library is not None, "--legacy-library is required")
    for name in ("library", "legacy_library", "collection"):
        if getattr(args, name) is not None:
            setattr(args, name, getattr(args, name).resolve(strict=True))
    args.output_dir = args.output_dir.resolve()
    (sample if args.role else run)(args)


if __name__ == "__main__":
    main()
