#!/usr/bin/env python3
"""Install pinned NeverC inputs and qualify actual x64/ARM64 Linux processes."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts import check_source_patch_cpp_runtime as cpp
from scripts import check_source_patch_pe_runtime as pe

SCHEMA = "neverd.source-patch.elf-runtime.v1"
PRODUCER = "182fd2c5dc"
PROFILES = {"x64": ("x86_64-unknown-linux-gnu", 62),
            "arm64": ("aarch64-unknown-linux-gnu", 183)}
CASES = {
    "original-eh": ((0, 1), (8, 77), (9, 19)),
    "original-callback": ((0, 1), (2, 42)),
    "c-ir": ((0, 11), (2, 15)),
    "c-object": ((0, 11), (2, 15)),
    "c-hosted": ((0, 11), (2, 15)),
    "hosted": ((0, 40), (2, 42)),
    "exceptions": cpp.PATCHED_CASES,
}


def machine(path: Path) -> int:
    data = path.read_bytes()
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("not a little-endian ELF64 fixture")
    return struct.unpack_from("<H", data, 18)[0]


def image_bytes(path: Path, va: int, size: int) -> bytes:
    machine(path)
    data = path.read_bytes()
    start = struct.unpack_from("<Q", data, 32)[0]
    stride, count = struct.unpack_from("<HH", data, 54)
    if stride != 56 or start + stride * count > len(data) or size <= 0:
        raise ValueError("invalid ELF program headers or code extent")
    matches = []
    for index in range(count):
        kind, flags, offset, begin, _, filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", data, start + index * stride)
        if kind == 1 and flags & 1 and begin <= va and va + size <= begin + filesz:
            at = offset + va - begin
            if at + size <= len(data):
                matches.append(data[at:at + size])
    if len(matches) != 1:
        raise ValueError("hosted code has no unique executable ELF extent")
    return matches[0]


def fixtures(root: Path) -> dict[str, dict]:
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if (manifest.get("schema") != "neverd.neverc-linux-fixtures.v2" or
        manifest.get("producer_commit") != PRODUCER or manifest.get("language") != "c"):
        raise ValueError("unqualified Linux producer fixtures")
    if set(manifest["sources"]) != {"original-eh.cpp", "original-callback.cpp",
                                    "replacement.c", "hosted.c", "exceptions.cpp"}:
        raise ValueError("Linux source inventory mismatch")
    for name, digest in manifest["sources"].items():
        path = (root / name).resolve()
        if not path.is_relative_to(root) or pe.digest(path) != digest:
            raise ValueError("Linux source path/digest mismatch")
    rows = manifest["entries"]
    if len(rows) != 2 or {row["architecture"] for row in rows} != set(PROFILES):
        raise ValueError("Linux fixture architecture inventory mismatch")
    for row in rows:
        arch = row["architecture"]
        if (row["target"], row["machine"]) != PROFILES[arch]:
            raise ValueError("Linux fixture target mismatch")
        if set(row["files"]) != {"original-eh.elf", "original-callback.elf", "c.ll", "c.o",
                                 "hosted.o", "hosted.bin", "hosted.audit.json",
                                 "c-hosted.o", "c-hosted.bin", "c-hosted.audit.json",
                                 "producer-commands.json"}:
            raise ValueError("Linux producer artifact inventory mismatch")
        for item in row["files"].values():
            path = (root / item["path"]).resolve()
            if not path.is_relative_to(root) or pe.digest(path) != item["sha256"]:
                raise ValueError("Linux producer artifact path/digest mismatch")
        directory = root / arch
        for prefix in ("hosted", "c-hosted"):
            audit = json.loads((directory / (prefix + ".audit.json")).read_text(encoding="utf-8"))
            if (audit.get("execution_profile") != "hosted-v1" or
                audit.get("keep_unwind") is not True or audit.get("entry_offset") != 0 or
                audit.get("target_triple") != PROFILES[arch][0] or
                audit.get("input_digest") != pe.digest(directory / (prefix + ".o")) or
                audit.get("output_digest") != pe.digest(directory / (prefix + ".bin"))):
                raise ValueError("Linux hosted producer audit mismatch")
    return {row["architecture"]: row for row in rows}


def prepare(args: argparse.Namespace) -> None:
    inputs = args.fixtures.resolve()
    rows = fixtures(inputs)
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": SCHEMA, "commit": os.environ.get("GITHUB_SHA"),
                "producer_commit": PRODUCER, "neverd_sha256": pe.digest(args.neverd),
                "fixture_manifest_sha256": pe.digest(inputs / "manifest.json"), "images": []}
    for arch, row in rows.items():
        directory = root / arch
        directory.mkdir()

        def record(path: Path, case: str) -> None:
            if machine(path) != PROFILES[arch][1]:
                raise ValueError("Linux publication machine mismatch")
            manifest["images"].append({"architecture": arch, "case": case,
                "path": path.relative_to(root).as_posix(), "sha256": pe.digest(path)})

        originals = {}
        for case in ("original-eh", "original-callback"):
            path = directory / (case + ".elf")
            shutil.copyfile(inputs / arch / path.name, path)
            originals[case] = path
            record(path, case)
        for case in ("c-ir", "c-object", "c-hosted", "hosted", "exceptions"):
            original = originals["original-callback" if case == "hosted" else "original-eh"]
            before = pe.digest(original)
            patched = directory / (case + ".elf")
            target = row["callback_target_va" if case == "hosted" else "target_va"]
            command = [str(args.neverd), "patch", str(original), "--entry=replacement",
                       f"--func={target:#x}", "--patch-report", "-o", str(patched)]
            if case == "c-ir":
                command += ["--from-ir=" + str(inputs / arch / "c.ll")]
            elif case in ("c-object", "c-hosted", "hosted"):
                command += ["--from-object=" + str(inputs / arch / ("c.o" if case == "c-object" else case + ".o"))]
            else:
                command += ["--from-source=" + str(inputs / "exceptions.cpp"), "--language=c++",
                            "--compiler-path=" + str(args.clang), "--compiler-arg=-O0",
                            f"--bind=data:cleanup_count={row['cleanup_va']:#x}:4",
                            f"--bind=function:original_thrower={row['thrower_va']:#x}"]
            result = pe.run(command, directory / (case + "-patch.log"))
            reports = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
            if (len(reports) != 1 or reports[0].get("schema") != "neverd.source-patch.v1" or
                reports[0].get("success") is not True or reports[0].get("target") != PROFILES[arch][0] or
                len(reports[0].get("installed", [])) != 1 or pe.digest(original) != before):
                raise ValueError("Linux publication receipt or original identity mismatch")
            receipt = reports[0]
            if case in ("c-object", "c-hosted", "hosted"):
                obj = inputs / arch / ("c.o" if case == "c-object" else case + ".o")
                if receipt.get("object_sha256") != pe.digest(obj):
                    raise ValueError("Linux object publication identity mismatch")
            if case in ("hosted", "c-hosted"):
                payload = (inputs / arch / (case + ".bin")).read_bytes()
                if (receipt.get("object_sha256") != pe.digest(inputs / arch / (case + ".o")) or
                    image_bytes(patched, receipt["installed"][0]["replacement_va"], len(payload)) != payload):
                    raise ValueError("installed Linux hosted code changed")
            (directory / (case + "-receipt.json")).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            record(patched, case)
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def execute(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    output = root / (args.arch + "-runtime.json")
    output.unlink(missing_ok=True)
    native = {"x86_64": "x64", "amd64": "x64", "aarch64": "arm64", "arm64": "arm64"}.get(platform.machine().lower())
    if platform.system() != "Linux" or native != args.arch:
        raise RuntimeError("Linux qualification requires the matching native host")
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA or manifest.get("producer_commit") != PRODUCER:
        raise ValueError("Linux runtime profile mismatch")
    if os.environ.get("GITHUB_SHA") and manifest.get("commit") != os.environ["GITHUB_SHA"]:
        raise ValueError("Linux runtime commit mismatch")
    rows = [row for row in manifest["images"] if row["architecture"] == args.arch]
    if len(rows) != len(CASES) or {row["case"] for row in rows} != set(CASES):
        raise ValueError("Linux runtime inventory mismatch")
    observations = []
    for row in rows:
        path = (root / row["path"]).resolve()
        if (not path.is_relative_to(root) or pe.digest(path) != row["sha256"] or
            machine(path) != PROFILES[args.arch][1]):
            raise ValueError("Linux runtime path/digest/machine mismatch")
        path.chmod(0o755)
        for mode, expected in CASES[row["case"]]:
            for attempt in range(3):
                result = pe.run([str(path), str(mode)], path.with_suffix(f".mode-{mode}-run-{attempt}.log"), expected)
                observations.append({"case": row["case"], "path": row["path"], "sha256": row["sha256"],
                    "mode": mode, "attempt": attempt, "expected": expected, "actual": result.returncode})
    output.write_text(json.dumps({"schema": SCHEMA, "commit": manifest["commit"],
        "producer_commit": PRODUCER, "architecture": args.arch, "native_host": native,
        "observations": observations}, indent=2) + "\n", encoding="utf-8")
    print(f"{args.arch}: all 66 native Linux C/hosted/Clang EH observations passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    for name in ("neverd", "clang", "fixtures", "output"):
        prep.add_argument("--" + name, type=Path, required=True)
    check = commands.add_parser("run")
    check.add_argument("--root", type=Path, required=True)
    check.add_argument("--arch", choices=PROFILES, required=True)
    args = parser.parse_args()
    if args.command == "prepare":
        args.neverd, args.clang = args.neverd.absolute(), args.clang.absolute()
        prepare(args)
    else:
        execute(args)


if __name__ == "__main__":
    main()
