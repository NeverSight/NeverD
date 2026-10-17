#!/usr/bin/env python3
"""Build a native example and execute both the CLI and hosted Python SDK paths."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def executable(value: str) -> str:
    found = shutil.which(value)
    if not found:
        raise ValueError(f"executable not found: {value}")
    return str(Path(found).resolve())


def layout(path: Path, system: str) -> dict:
    data = path.read_bytes()
    if system == "Windows":
        pe = struct.unpack_from("<I", data, 60)[0]
        sections = struct.unpack_from("<H", data, pe + 6)[0]
        optional = struct.unpack_from("<H", data, pe + 20)[0]
        headers = struct.unpack_from("<I", data, pe + 24 + 60)[0]
        return {"header_bytes": headers, "sections": sections,
                "free_header_bytes": headers - (pe + 24 + optional + sections * 40)}
    if system == "Darwin":
        position = 32
        for _ in range(struct.unpack_from("<I", data, 16)[0]):
            command, size = struct.unpack_from("<II", data, position)
            if command == 25 and data[position + 8:position + 24].rstrip(b"\0") == b"__TEXT":
                return {"text_vmaddr": struct.unpack_from("<Q", data, position + 24)[0]}
            position += size
        raise ValueError("missing native Mach-O TEXT segment")
    return {"file_bytes": len(data)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True, help="Python-enabled NeverD executable")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--output", type=Path, required=True, help="new output directory")
    parser.add_argument("--api", choices=("cli", "sdk", "both"), default="both")
    args = parser.parse_args()
    system = platform.system()
    arch = {"x86_64": "x64", "amd64": "x64", "arm64": "arm64", "aarch64": "arm64"}.get(
        platform.machine().lower()
    )
    if system not in ("Linux", "Darwin", "Windows") or arch is None:
        raise ValueError("use a native Linux, macOS or Windows x64/ARM64 host")
    neverd, clang = executable(args.neverd), executable(args.clang)
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    logs = root / "logs"
    logs.mkdir()
    commands = []

    def run(command: list[str], label: str, *, env=None, fail_ok=False):
        command = [str(part) for part in command]
        print(subprocess.list2cmdline(command) if system == "Windows" else shlex.join(command),
              flush=True)
        result = subprocess.run(command, capture_output=True, text=True, timeout=120, env=env)
        record = {"command": command, "returncode": result.returncode,
                  "stdout": result.stdout, "stderr": result.stderr}
        commands.append({"label": label, **record})
        (logs / (label + ".json")).write_text(json.dumps(record, indent=2) + "\n",
                                             encoding="utf-8")
        if result.returncode and not fail_ok:
            raise RuntimeError(f"{label}: {result.stderr or result.stdout}")
        return result

    if system == "Darwin":
        translated = run(["sysctl", "-in", "sysctl.proc_translated"], "host",
                         fail_ok=True)
        if translated.stdout.strip() == "1":
            raise ValueError("run natively; Rosetta is a separate execution mode")
    flags = ["-O0", "-fno-omit-frame-pointer", "-funwind-tables"]
    if system == "Windows":
        flags += ["--target=" + ("x86_64" if arch == "x64" else "aarch64")
                  + "-pc-windows-msvc"]
    elif system == "Darwin":
        sdk = run(["xcrun", "--show-sdk-path"], "sdk").stdout.strip()
        flags += ["-isysroot", sdk]
    suffix = ".exe" if system == "Windows" else ""
    name = ("patch_extra.dll" if system == "Windows" else
            "libpatch_extra.dylib" if system == "Darwin" else "libpatch_extra.so")
    dependency = "@rpath/" + name if system == "Darwin" else name
    library = root / name
    original = root / ("original" + suffix)
    source = HERE / "replacement.c"
    library_flags = ["-dynamiclib"] if system == "Darwin" else ["-shared"]
    if system != "Windows":
        library_flags += ["-fPIC"]
    if system == "Linux":
        library_flags += ["-Wl,-soname," + name]
    elif system == "Darwin":
        library_flags += ["-Wl,-install_name," + dependency]
    else:
        library_flags += ["-Wl,/implib:" + str(root / "patch_extra.lib")]
    run([clang, *flags, *library_flags, str(HERE / "extra.c"), "-o", str(library)],
        "build-library")
    app_flags = []
    if system == "Linux":
        app_flags = ["-fPIE", "-pie", "-Wl,--export-dynamic", "-Wl,--eh-frame-hdr",
                     "-Wl,-rpath,$ORIGIN"]
    elif system == "Darwin":
        app_flags = ["-Wl,-rpath,@loader_path", "-Wl,-headerpad,0",
                     "-Wl,-keep_dwarf_unwind", "-Wl,-adhoc_codesign"]
    link = [clang, *flags, str(HERE / "original.c"), *app_flags]
    if system == "Windows":
        # The linker may keep its own spare headers. Leave fewer than the
        # combined patch needs and verify actual header growth after publication.
        objects = []
        for attempt in range(8):
            probe = root / f"probe-{attempt}.exe"
            run([*link, *objects, "-o", str(probe)], f"build-probe-{attempt}")
            spare = layout(probe, system)["free_header_bytes"]
            if 0 <= spare < 3 * 40:
                break
            count = max(1, spare // 40 - 2)
            for index in range(len(objects), len(objects) + count):
                obj = root / f"header-{index}.obj"
                run([clang, *flags, "-c", str(HERE / "header_section.c"),
                     f'-DEXAMPLE_SECTION=".demo{index}"', f"-DEXAMPLE_SYMBOL=demo_{index}",
                     "-o", str(obj)], f"build-section-{index}")
                objects.append(str(obj))
        else:
            raise ValueError("the linker retains too much section-header capacity")
        link += objects
    run([*link, "-o", str(original)], "build-original")
    before = layout(original, system)
    if system == "Windows" and not 0 <= before["free_header_bytes"] < 3 * 40:
        raise ValueError("the original retains too much section-header capacity")
    original_hash = sha256(original)
    exports = json.loads(run([neverd, "exports", str(original), "--json"], "exports").stdout)
    addresses = {int(row["addr"], 0) if isinstance(row["addr"], str) else row["addr"]
                 for row in exports if row["name"] in ("target", "_target")}
    if len(addresses) != 1:
        raise ValueError("the example must export exactly one target")
    address = hex(addresses.pop())
    config = {"source": str(source), "clang": clang, "address": address,
              "dependency": dependency, "sdk_output": str(root / ("app.sdk" + suffix)),
              "sdk_report": str(root / "sdk-receipt.json")}
    (root / "example.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    images = {"original": original}
    for api in (("cli", "sdk") if args.api == "both" else (args.api,)):
        output = root / ("app." + api + suffix)
        if api == "cli":
            result = run([
                neverd, "patch", str(original), "--from-source=" + str(source),
                "--entry=replacement", "--func=" + address, "--compiler=clang",
                "--compiler-path=" + clang, "--artifact=object",
                "--bind=original:saved_target=" + address,
                "--dependency=function:extra_function=" + dependency,
                "--dependency=data:extra_state=" + dependency,
                "--patch-report", "-o", str(output),
            ], "patch-cli")
            receipt = json.loads(result.stdout)
            (root / "cli-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n",
                                                   encoding="utf-8")
        else:
            run([neverd, "plugins", "--plugin-dir=" + str(HERE / "plugin"),
                 "--run=Source Patch Example", "--binary=" + str(original)], "patch-sdk")
            receipt = json.loads(Path(config["sdk_report"]).read_text(encoding="utf-8"))
        if (receipt.get("success") is not True
                or receipt.get("source_sha256") != sha256(source)
                or len(receipt.get("original_callbacks", [])) != 1
                or receipt["original_callbacks"][0]["symbol"] != "saved_target"
                or receipt["original_callbacks"][0]["original_va"] != int(address, 0)
                or {(row["symbol"], row["library"], row["kind"])
                    for row in receipt.get("added_imports", [])}
                != {("extra_function", dependency, 0), ("extra_state", dependency, 1)}):
            raise ValueError(f"{api}: incomplete callback/import publication receipt")
        images[api] = output

    observations = []
    layouts = {"original": before}
    for api, image in images.items():
        after = layout(image, system)
        layouts[api] = after
        if api != "original":
            if system == "Windows" and after["header_bytes"] <= before["header_bytes"]:
                raise ValueError(f"{api}: the PE header table did not grow")
            if (system == "Windows" and (after["sections"] - before["sections"]) * 40
                    <= before["free_header_bytes"]):
                raise ValueError(f"{api}: the patch did not exceed original header capacity")
            if system == "Darwin" and after["text_vmaddr"] >= before["text_vmaddr"]:
                raise ValueError(f"{api}: the tight Mach-O header prefix did not grow")
            saved = json.loads(run([neverd, "exports", str(image), "--json"],
                                   "exports-" + api).stdout)
            if not any(row["name"] in ("target", "_target")
                       and int(str(row["addr"]), 0) == int(address, 0) for row in saved):
                raise ValueError(f"{api}: the original target VA changed")
        if system == "Darwin":
            run(["codesign", "--verify", "--strict", str(image)], "codesign-" + api)
            info = run(["xcrun", "dyld_info", "-validate_only", str(image)], "dyld-" + api)
            header = str(image)
            cpu = "x86_64" if arch == "x64" else "arm64"
            if (info.stdout.strip() not in (header + ":", header + f" [{cpu}]:")
                    or info.stderr.strip()):
                raise ValueError(f"{api}: dyld reported a structural diagnostic")
        for x in (-5, 0, 7):
            result = run([str(image), str(x)], f"execute-{api}-{x}")
            expected = x + 1 if api == "original" else 4 * x + 8
            if result.stdout.strip() != str(expected):
                raise ValueError(f"{api}: expected {expected}, got {result.stdout!r}")
            observations.append({"api": api, "input": x, "output": expected})
    # A copy beside no provider must fail in the actual OS loader.
    missing = root / "missing-dependency"
    missing.mkdir()
    loader_rejections = []
    environment = os.environ.copy()
    for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FALLBACK_LIBRARY_PATH"):
        environment.pop(key, None)
    for api, image in images.items():
        if api == "original":
            continue
        absent = missing / image.name
        shutil.copy2(image, absent)
        result = run([str(absent), "7"], "missing-" + api, env=environment, fail_ok=True)
        if (result.returncode == 0
                or (system == "Windows" and result.returncode & 0xffffffff != 0xc0000135)
                or (system != "Windows" and name not in result.stderr)):
            raise ValueError(f"{api}: expected the loader's missing-library error")
        loader_rejections.append(api)
    if sha256(original) != original_hash:
        raise ValueError("patching changed the original input")
    (root / "commands.json").write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    summary = {
        "schema": "neverd.source-patch.example.v1", "system": system,
        "architecture": arch, "execution": "native-os-process",
        "observations": observations, "loader_rejections": loader_rejections, "layout": layouts,
        "files": {path.relative_to(root).as_posix(): sha256(path)
                  for path in root.rglob("*") if path.is_file()},
        "sources": {path.relative_to(HERE).as_posix(): sha256(path)
                    for path in (HERE / "original.c", HERE / "extra.c", source,
                                 HERE / "header_section.c", HERE / "plugin/patch.py", HERE / "run.py")},
    }
    (root / "verified.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {len(observations)} executions, {len(loader_rejections)} loader rejections")


if __name__ == "__main__":
    main()
