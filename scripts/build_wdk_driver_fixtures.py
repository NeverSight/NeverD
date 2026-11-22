#!/usr/bin/env python3
"""Build the original native driver corpus against pinned Microsoft WDK files.

Downloads and extracted Microsoft files remain in the selected cache, with
their original license and package metadata. No installer or host driver runs.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tempfile
import urllib.request
import zipfile


ROOT = Path(__file__).resolve().parents[1]
DEFINITIONS = ROOT / "scripts" / "WDKDriverFixtures.def"


def declarations(path: Path = DEFINITIONS) -> dict[str, list[list[str]]]:
    result: dict[str, list[list[str]]] = {}
    for kind, arguments in re.findall(
        r"^NEVERD_WDK_(\w+)\((.*?)\)\s*$", path.read_text(encoding="utf-8"),
        re.M | re.S,
    ):
        fields = ast.literal_eval("[" + arguments + "]")
        if not fields or not all(isinstance(field, str) for field in fields):
            raise ValueError(f"invalid WDK declaration: {kind}")
        result.setdefault(kind, []).append(fields)
    return result


LIMITS = {name: int(value) for name, value in declarations()["LIMIT"]}


def digest(path: Path) -> str:
    with path.open("rb") as source:
        value = hashlib.sha256()
        for block in iter(lambda: source.read(LIMITS["read_chunk"]), b""):
            value.update(block)
    return value.hexdigest()


def download(cache: Path, name: str, url: str, expected: str) -> Path:
    destination = cache / name
    if not destination.exists():
        # Independent target generations can authenticate the same cached package.
        # Only a fully written, verified download may acquire its public name.
        temporary = tempfile.NamedTemporaryFile(dir=cache, prefix=name + ".", suffix=".part",
                                                delete=False)
        partial = Path(temporary.name)
        try:
            with temporary as output, urllib.request.urlopen(
                url, timeout=LIMITS["download_timeout"]
            ) as response:
                while block := response.read(LIMITS["read_chunk"]):
                    output.write(block)
            if digest(partial) != expected:
                raise ValueError(f"Microsoft package hash mismatch: {name}")
            partial.replace(destination)
        finally:
            partial.unlink(missing_ok=True)
    if digest(destination) != expected:
        raise ValueError(f"Microsoft package hash mismatch: {name}")
    return destination


def extract(archives: list[Path], kit: Path, selections: list[str]) -> list[dict]:
    selected: dict[str, tuple[str, bytes]] = {}
    found: set[str] = set()
    for archive in archives:
        with zipfile.ZipFile(archive) as package:
            for entry in package.infolist():
                if entry.is_dir():
                    continue
                matches = [
                    value for value in selections
                    if (entry.filename.startswith(value) if value.endswith("/")
                        else entry.filename == value)
                ]
                if not matches:
                    continue
                path = PurePosixPath(entry.filename)
                if (path.is_absolute() or ".." in path.parts
                        or "\\" in entry.filename or ":" in entry.filename):
                    raise ValueError(f"invalid Microsoft archive path: {path}")
                data = package.read(entry)
                key = entry.filename.casefold()
                if key in selected and selected[key][1] != data:
                    raise ValueError(f"conflicting Microsoft archive file: {path}")
                selected[key] = (entry.filename, data)
                destination = kit / path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
                found.update(matches)
    if missing := set(selections) - found:
        raise ValueError(f"missing Microsoft package inputs: {sorted(missing)}")
    # The include overlay must not admit an old or injected unverified header.
    actual = {path.relative_to(kit).as_posix().casefold()
              for path in kit.rglob("*") if path.is_file()}
    if actual != set(selected):
        raise ValueError("unverified files in Microsoft extraction directory")
    return [
        {"path": name, "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in sorted(selected.values())
    ]


def overlay_directory(path: Path, top: bool = False) -> dict:
    return {
        "type": "directory", "name": path.as_posix() if top else path.name,
        "contents": [
            {"type": "file", "name": child.name,
             "external-contents": child.as_posix()}
            if child.is_file() else overlay_directory(child)
            for child in sorted(path.iterdir())
        ],
    }


def cache_entry(name: str, image: Path) -> str:
    # Check before filesystem canonicalization, which can itself reject an
    # invalid Windows filename before the portable diagnostic is available.
    raw = image.as_posix()
    if any(character in raw for character in ('"', ';', '\n', '\r', '$')):
        raise ValueError(f"unsupported CMake fixture path: {raw}")
    path = image.resolve().as_posix()
    if any(character in path for character in ('"', ';', '\n', '\r', '$')):
        raise ValueError(f"unsupported CMake fixture path: {path}")
    return f'set({name} "{path}" CACHE FILEPATH "Pinned WDK fixture" FORCE)\n'


def build(output: Path, cache: Path, clang: str, linker: str) -> None:
    inventory = declarations()
    output.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    cache_file = output / "fixtures.cmake"
    # A failed rebuild must not leave an earlier cache claiming completeness.
    cache_file.unlink(missing_ok=True)
    (output / "build-manifest.json").unlink(missing_ok=True)
    archives = [download(cache, *record) for record in inventory["DOWNLOAD"]]
    for record in inventory["LICENSE"]:
        license_file = download(cache, *record)
        (output / license_file.name).write_bytes(license_file.read_bytes())
    kit = output / "kit"
    extracted = extract(archives, kit, [row[0] for row in inventory["EXTRACT"]])
    overlay = output / "headers-vfs.json"
    overlay.write_text(json.dumps({
        "version": 0, "case-sensitive": False,
        "roots": [overlay_directory(kit / "c/Include", True)],
    }, indent=2) + "\n", encoding="utf-8")
    resource = Path(subprocess.check_output(
        [clang, "-print-resource-dir"], text=True
    ).strip()) / "include"
    arguments = {row[0]: row[1:] for row in inventory["ARGUMENTS"]}
    entries = dict(inventory["ENTRY"])
    includes = [item for row in inventory["INCLUDE"]
                for item in ("-isystem", str(kit / row[0]))]
    sources = ROOT / "unittests/emulation/fixtures"
    builds = []
    cache_lines = []
    for name, filename, library, debug, profile in inventory["FIXTURE"]:
        source = sources / filename
        for guard in (False, True):
            identity = "NEVERD_" + name + ("_CFG" if guard else "") + "_FIXTURE"
            stem = output / identity.lower()
            obj, image = stem.with_suffix(".obj"), stem.with_suffix(".sys")
            cc = [clang, *arguments["compile"], "-isystem", str(resource),
                  "-ivfsoverlay", str(overlay), *includes, "-DDBG=" + debug,
                  "-c", str(source), "-o", str(obj)]
            ld = [linker, *arguments["link"], "/entry:" + entries[library],
                  "/out:" + str(image), "/map:" + str(stem.with_suffix(".map")),
                  str(obj), *[str(kit / path) for group, path in inventory["LIBRARY"]
                              if group == "WDM" or group == library]]
            if guard:
                cc += arguments["cfg_compile"]
                ld += arguments["cfg_link"]
            if profile:
                cc += arguments[profile + "_compile"]
                ld += arguments.get(profile + "_link", [])
            for phase, command in (("compile", cc), ("link", ld)):
                with stem.with_suffix("." + phase + ".log").open("w") as log:
                    subprocess.run(command, check=True, stdout=log,
                                   stderr=subprocess.STDOUT,
                                   timeout=LIMITS["command_timeout"])
            cache_lines.append(cache_entry(identity, image))
            builds.append({"cache_variable": identity, "source": filename,
                           "source_sha256": digest(source), "commands": [cc, ld],
                           "image": str(image), "image_sha256": digest(image)})
            print(f"Built {identity}", flush=True)
    manifest = {
        "compiler": subprocess.check_output([clang, "--version"], text=True),
        "linker": subprocess.check_output([linker, "--version"], text=True),
        "packages": [{"file": name, "url": url, "sha256": sha}
                     for name, url, sha in inventory["DOWNLOAD"]],
        "licenses": [{"file": name, "url": url, "sha256": sha}
                     for name, url, sha in inventory["LICENSE"]],
        "declarations_sha256": digest(DEFINITIONS),
        "extracted": extracted,
        "fixture_inputs": {str(path.relative_to(ROOT)): digest(path)
                           for path in sorted(sources.iterdir()) if path.is_file()},
        "builds": builds,
    }
    (output / "build-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    cache_file.write_text("".join(cache_lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--linker", default="lld-link")
    args = parser.parse_args()
    build(args.output.resolve(), args.cache.resolve(), args.clang, args.linker)


if __name__ == "__main__":
    main()
