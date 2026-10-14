#!/usr/bin/env python3
"""Execute pinned NeverC C and hosted frames in native Windows processes."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from scripts import check_source_patch_cpp_runtime as cpp
from scripts import check_source_patch_pe_runtime as pe

SCHEMA = "neverd.source-patch.hosted-pe-runtime.v1"
PRODUCER = "182fd2c5dc"
CASES = {"hosted-original": ((0, 1), (2, 42)), "hosted": ((0, 40), (2, 42)),
         "c-original": ((0, 1), (2, 3)), "c-ir": ((0, 11), (2, 15)),
         "c-object": ((0, 11), (2, 15)), "c-hosted": ((0, 11), (2, 15))}
ORIGINAL = r'''
extern "C" __declspec(dllexport) int cleanup_count = 0;
using Callback = int (*)(int);
extern "C" __declspec(noinline) int throwing(int x) { if (x) throw x; return 0; }
extern "C" __declspec(dllexport) __declspec(noinline) int target(Callback cb, int x) {
  volatile int value = cb(x); return value + 1;
}
int main(int argc, char **argv) {
  int x = argc > 1 ? argv[1][0] - '0' : 0;
  try { return target(throwing, x); }
  catch (int value) { return value + 40; }
}
'''


def image_bytes(path: Path, va: int, size: int) -> bytes:
    data = path.read_bytes()
    header = struct.unpack_from("<I", data, 0x3c)[0]
    count, optional = struct.unpack_from("<H", data, header + 6)[0], struct.unpack_from("<H", data, header + 20)[0]
    if struct.unpack_from("<H", data, header + 24)[0] != 0x20b:
        raise ValueError("hosted fixture requires PE32+")
    base = struct.unpack_from("<Q", data, header + 48)[0]
    rva = va - base
    matches = []
    for index in range(count):
        section = header + 24 + optional + index * 40
        length, begin, rawsize, offset = struct.unpack_from("<IIII", data, section + 8)
        if begin <= rva and rva + size <= begin + min(length, rawsize):
            start = offset + rva - begin
            if start + size <= len(data):
                matches.append(data[start:start + size])
    if len(matches) != 1:
        raise ValueError("hosted code has no unique file-backed PE extent")
    return matches[0]


def producer_fixture(root: Path, arch: str) -> tuple[Path, bytes]:
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if (manifest.get("schema") != "neverd.hosted-producer-fixtures.v2" or
        manifest.get("producer_commit") != PRODUCER or manifest.get("language") != "c"):
        raise ValueError("unqualified hosted producer revision")
    if pe.digest(root / "replacement.c") != manifest.get("source_sha256"):
        raise ValueError("hosted source digest mismatch")
    if pe.digest(root / "scalar.c") != manifest.get("c_source_sha256"):
        raise ValueError("C source digest mismatch")
    rows = [row for row in manifest["entries"] if row["architecture"] == arch]
    if len(rows) != 1 or rows[0]["target"] != pe.PROFILES[arch][0]:
        raise ValueError("hosted target inventory mismatch")
    row = rows[0]
    if set(row["c_files"]) != {"c.ll", "c.obj", "c-hosted.obj", "c-hosted.bin",
                                  "c-hosted.audit.json", "producer-commands.json"}:
        raise ValueError("C producer inventory mismatch")
    for item in row["c_files"].values():
        path = (root / item["path"]).resolve()
        if not path.is_relative_to(root.resolve()) or pe.digest(path) != item["sha256"]:
            raise ValueError("C producer artifact path/digest mismatch")
    for path in (root / arch / "c.obj", root / arch / "c-hosted.obj", root / row["object"]):
        data = path.read_bytes()
        if len(data) < 20 or struct.unpack_from("<H", data)[0] != pe.PROFILES[arch][1]:
            raise ValueError("producer must emit a native COFF object, not bitcode")
    cpp_audit = json.loads((root / arch / "c-hosted.audit.json").read_text(encoding="utf-8"))
    if (cpp_audit.get("execution_profile") != "hosted-v1" or cpp_audit.get("keep_unwind") is not True or
        cpp_audit.get("target_triple") != row["target"] or cpp_audit.get("entry_offset") != 0 or
        cpp_audit.get("input_digest") != pe.digest(root / arch / "c-hosted.obj") or
        cpp_audit.get("output_digest") != pe.digest(root / arch / "c-hosted.bin")):
        raise ValueError("C hosted compiler audit mismatch")
    files = {}
    for kind in ("object", "payload", "audit"):
        path = (root / row[kind]).resolve()
        if not path.is_relative_to(root.resolve()) or pe.digest(path) != row[kind + "_sha256"]:
            raise ValueError("hosted artifact path/digest mismatch")
        files[kind] = path
    audit = json.loads(files["audit"].read_text(encoding="utf-8"))
    if (audit.get("execution_profile") != "hosted-v1" or audit.get("keep_unwind") is not True or
        audit.get("target_triple") != pe.PROFILES[arch][0] or audit.get("entry_offset") != 0 or
        audit.get("input_digest") != pe.digest(files["object"]) or
        audit.get("output_digest") != pe.digest(files["payload"])):
        raise ValueError("hosted compiler audit mismatch")
    return files["object"], files["payload"].read_bytes()


def prepare(args: argparse.Namespace) -> None:
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    manifest = {"schema": SCHEMA, "commit": os.environ.get("GITHUB_SHA"),
                "producer_commit": PRODUCER, "images": []}
    for arch in cpp.ARCHES:
        obj, payload = producer_fixture(args.fixtures.resolve(), arch)
        directory = root / arch
        directory.mkdir()
        source, original_obj, original, patched = (directory / name for name in
            ("original.cpp", "original.obj", "original.exe", "patched.exe"))
        source.write_text(ORIGINAL, encoding="utf-8")
        pe.run([str(args.clang), "--driver-mode=g++", "--target=" + pe.PROFILES[arch][0],
                "-O0", "-fms-runtime-lib=dll", "-c", str(source), "-o", str(original_obj)],
               directory / "compile.log")
        libraries = ["/libpath:" + str(getattr(args, name + "_" + arch)) for name in ("crt", "ucrt", "sdk")]
        pe.run([str(args.linker), str(original_obj), "/out:" + str(original), "/subsystem:console",
                "/export:??_7type_info@@6B@,DATA", "/dynamicbase", *libraries], directory / "link.log")
        va = cpp.exports(args.library, original)["target"]
        before = pe.digest(original)
        result = pe.run([str(args.neverd), "patch", str(original), "--from-object=" + str(obj),
                         "--entry=replacement", f"--func={va:#x}", "--patch-report", "-o", str(patched)],
                        directory / "patch.log")
        receipt = pe.publication_receipt(result.stdout, arch, "object")
        if receipt.get("object_sha256") != pe.digest(obj) or len(receipt.get("installed", [])) != 1:
            raise ValueError("hosted object publication receipt mismatch")
        owner = receipt["installed"][0]["replacement_va"]
        if image_bytes(patched, owner, len(payload)) != payload or pe.digest(original) != before:
            raise ValueError("installed hosted code changed or original was overwritten")
        (directory / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        for case, path in (("hosted-original", original), ("hosted", patched)):
            manifest["images"].append({"architecture": arch, "case": case,
                "path": path.relative_to(root).as_posix(), "sha256": pe.digest(path)})

        cpp_source, cpp_obj, cpp_original = (directory / name for name in
            ("c-original.cpp", "c-original.obj", "c-original.exe"))
        cpp_source.write_text(cpp.ORIGINAL, encoding="utf-8")
        pe.run([str(args.clang), "--driver-mode=g++", "--target=" + pe.PROFILES[arch][0],
                "-O0", "-fms-runtime-lib=dll", "-c", str(cpp_source), "-o", str(cpp_obj)],
               directory / "cpp-compile.log")
        pe.run([str(args.linker), str(cpp_obj), "/out:" + str(cpp_original), "/subsystem:console",
                "/export:??_7type_info@@6B@,DATA", "/dynamicbase", *libraries], directory / "cpp-link.log")
        cpp_va = cpp.exports(args.library, cpp_original)["target"]
        cpp_before = pe.digest(cpp_original)
        manifest["images"].append({"architecture": arch, "case": "c-original",
            "path": cpp_original.relative_to(root).as_posix(), "sha256": cpp_before})
        for case, input_name, artifact in (("c-ir", "c.ll", "ir"),
                                           ("c-object", "c.obj", "object"),
                                           ("c-hosted", "c-hosted.obj", "object")):
            output = directory / (case + ".exe")
            input_path = args.fixtures.resolve() / arch / input_name
            command = [str(args.neverd), "patch", str(cpp_original), "--from-" + artifact + "=" + str(input_path),
                       "--entry=replacement", f"--func={cpp_va:#x}", "--patch-report", "-o", str(output)]
            result = pe.run(command, directory / (case + "-patch.log"))
            receipt = pe.publication_receipt(result.stdout, arch, artifact)
            if len(receipt.get("installed", [])) != 1 or pe.digest(cpp_original) != cpp_before:
                raise ValueError("C publication inventory/original mismatch")
            if artifact == "object" and receipt.get("object_sha256") != pe.digest(input_path):
                raise ValueError("C publication object digest mismatch")
            if case == "c-hosted":
                cpp_payload = (input_path.parent / "c-hosted.bin").read_bytes()
                if image_bytes(output, receipt["installed"][0]["replacement_va"], len(cpp_payload)) != cpp_payload:
                    raise ValueError("installed C hosted code changed")
            (directory / (case + "-receipt.json")).write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
            manifest["images"].append({"architecture": arch, "case": case,
                "path": output.relative_to(root).as_posix(), "sha256": pe.digest(output)})
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


def execute(args: argparse.Namespace) -> None:
    root = args.root.resolve()
    report = root / (args.arch + "-runtime.json")
    report.unlink(missing_ok=True)
    host = pe.runtime_host(args.arch, args.runner, args.expected_native_host)
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != SCHEMA or manifest.get("producer_commit") != PRODUCER:
        raise ValueError("hosted runtime profile mismatch")
    if os.environ.get("GITHUB_SHA") and manifest.get("commit") != os.environ["GITHUB_SHA"]:
        raise ValueError("hosted runtime commit mismatch")
    rows = [row for row in manifest["images"] if row["architecture"] == args.arch]
    if len(rows) != len(CASES) or {row["case"] for row in rows} != set(CASES):
        raise ValueError("hosted runtime inventory mismatch")
    observations = []
    for row in rows:
        image = (root / row["path"]).resolve()
        if not image.is_relative_to(root) or pe.digest(image) != row["sha256"] or pe.machine(image) != pe.PROFILES[args.arch][1]:
            raise ValueError("hosted runtime path/digest/machine mismatch")
        for mode, expected in CASES[row["case"]]:
            for attempt in range(3):
                command = ([str(args.runner)] if args.runner else []) + [str(image), str(mode)]
                result = pe.run(command, image.with_suffix(f".mode-{mode}-run-{attempt}.log"), expected)
                observations.append({"case": row["case"], "path": row["path"], "sha256": row["sha256"], "mode": mode,
                    "attempt": attempt, "expected": expected, "actual": result.returncode})
    report.write_text(json.dumps({"schema": SCHEMA, "commit": manifest["commit"],
        "producer_commit": PRODUCER, "architecture": args.arch, "native_host": host,
        "observations": observations}, indent=2) + "\n", encoding="utf-8")
    print(f"{args.arch}: all 36 native Windows NeverC C/hosted observations passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prep = commands.add_parser("prepare")
    for name in ("neverd", "library", "clang", "linker", "output", "fixtures"):
        prep.add_argument("--" + name, type=Path, required=True)
    for arch in cpp.ARCHES:
        for name in ("crt", "ucrt", "sdk"):
            prep.add_argument(f"--{name}-{arch}", type=Path, required=True)
    check = commands.add_parser("run")
    check.add_argument("--root", type=Path, required=True)
    check.add_argument("--arch", choices=cpp.ARCHES, required=True)
    check.add_argument("--expected-native-host", choices=cpp.ARCHES)
    check.add_argument("--runner", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        for name in ("neverd", "library", "clang", "linker"):
            setattr(args, name, getattr(args, name).absolute())
        prepare(args)
    else:
        execute(args)


if __name__ == "__main__":
    main()
