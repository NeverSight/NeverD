#!/usr/bin/env python3
"""Package the Linux CLI's resolved non-system library closure for CI shards."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dependencies(binary: Path, env: dict[str, str]) -> list[Path]:
    output = subprocess.run(["ldd", str(binary.resolve())], env=env, text=True,
                            capture_output=True, check=True).stdout
    result = []
    for line in output.splitlines():
        if "not found" in line:
            raise ValueError(f"unresolved runtime dependency: {line.strip()}")
        if "linux-vdso" in line:
            continue
        match = re.fullmatch(r"\s*(?:\S+\s+=>\s+)?(/\S+)\s+\(0x[0-9a-f]+\)\s*", line)
        if not match:
            raise ValueError(f"unrecognized ldd dependency: {line.strip()}")
        path = Path(match.group(1))
        if not path.is_file():
            raise ValueError(f"missing runtime dependency: {path}")
        result.append(path)
    if not result:
        raise ValueError("no dynamic dependencies found for the Linux CLI")
    return result


def system_library(path: Path) -> bool:
    resolved = path.resolve()
    return any(resolved.is_relative_to(root) for root in (Path("/lib"), Path("/usr/lib")))


def verify(directory: Path, commit: str) -> dict[str, str]:
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("schema") != 1 or manifest.get("commit") != commit:
        raise ValueError("bundle commit or schema mismatch")
    files = {p.relative_to(directory).as_posix(): p for p in directory.rglob("*") if p.is_file()}
    if set(files) != set(manifest["files"]) | {"manifest.json"}:
        raise ValueError("bundle contains missing or unexpected files")
    if "bin/neverd" not in files or not any(p.startswith("licenses/") for p in files):
        raise ValueError("bundle is missing the CLI or license notices")
    for relative, expected in manifest["files"].items():
        if digest(files[relative]) != expected:
            raise ValueError(f"bundle hash mismatch: {relative}")
    env = dict(os.environ)
    # Exclude inherited search paths so a worker cannot use the original build.
    env["LD_LIBRARY_PATH"] = str(directory.resolve() / "lib")
    for dep in dependencies(directory / "bin/neverd", env):
        if not system_library(dep) and not dep.resolve().is_relative_to(directory.resolve() / "lib"):
            raise ValueError(f"runtime dependency escapes verified bundle: {dep}")
    return env


def pack(binary: Path, destination: Path, commit: str) -> None:
    if destination.exists():
        raise ValueError("bundle output already exists")
    with tempfile.TemporaryDirectory(prefix="neverd-mobile-bundle-") as temporary:
        stage = Path(temporary)
        (stage / "bin").mkdir()
        (stage / "lib").mkdir()
        shutil.copy2(binary, stage / "bin/neverd")
        shutil.copytree(binary.parent / "licenses", stage / "licenses")
        system = []
        for dep in dependencies(binary, dict(os.environ)):
            if system_library(dep):
                system.append(str(dep))
                continue
            target = stage / "lib" / dep.name
            if target.exists() and digest(target) != digest(dep):
                raise ValueError(f"conflicting runtime library basename: {dep.name}")
            shutil.copy2(dep, target)
        manifest = {"schema": 1, "commit": commit, "system_libraries": sorted(system),
                    "files": {p.relative_to(stage).as_posix(): digest(p)
                              for p in sorted(stage.rglob("*")) if p.is_file()}}
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        verify(stage, commit)
        with tarfile.open(destination, "x:gz") as archive:
            for path in sorted(stage.iterdir()):
                archive.add(path, arcname=path.name)


def restore(archive_path: Path, destination: Path, commit: str) -> None:
    destination.mkdir(parents=True, exist_ok=False)
    with tarfile.open(archive_path, "r:gz") as archive:
        for item in archive.getmembers():
            if not (item.isdir() or item.isfile()):
                raise ValueError("runtime bundle may contain only directories and regular files")
        archive.extractall(destination, filter="data")
    verify(destination, commit)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("pack", "restore", "verify"))
    parser.add_argument("--binary", type=Path, default=Path("build-mobile/bin/neverd"))
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--directory", type=Path, default=Path("build-mobile"))
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    if args.action == "pack":
        pack(args.binary, args.archive, args.commit)
    elif args.action == "restore":
        restore(args.archive, args.directory, args.commit)
    else:
        verify(args.directory, args.commit)


if __name__ == "__main__":
    main()
