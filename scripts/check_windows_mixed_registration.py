#!/usr/bin/env python3
"""Check generated SEH/C++ parents in both orders, before and after optimization."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import xml.etree.ElementTree as ET

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_realigned import require_test_result
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import load_libraries
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_realigned import require_test_result
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import load_libraries

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unittests/lift/eh/fixtures"
SOURCES = ("registration_cleanup_only.cpp", "registration_cleanup_shared.s",
           "registration_mixed_seh.s", "registration_mixed_observer.cpp")


def require_generated_entries(path: Path) -> None:
    pe = PE32(path.read_bytes())
    for name in (b"cleanup_only_parent", b"mixed_seh"):
        entry = pe.entry(name)
        offset = pe.raw(entry, 5)
        if pe.data[offset] != 0xe9:
            raise ValueError("mixed fixture entry was not replaced")
        target = (entry + 5 + struct.unpack_from("<i", pe.data, offset + 1)[0]) & 0xffffffff
        owners = [name for name, start, size, _, _ in pe.sections
                  if start <= target < start + size]
        if owners != [".ndtext"]:
            raise ValueError("mixed fixture entry does not target generated code")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True)
    parser.add_argument("--runtime-libs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--baseline-only", action="store_true",
                        help="qualify original fixtures before testing NeverD")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "passed": False, "commands": [], "cases": [],
              "baseline_only": args.baseline_only,
              "sources": {name: hashlib.sha256((FIXTURES / name).read_bytes()).hexdigest()
                          for name in SOURCES}}

    def run(command: list[str | Path], extra: dict | None = None) -> None:
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True,
                                errors="replace", timeout=180, cwd=output,
                                env=os.environ | (extra or {}))
        report["commands"].append({"command": command,
                                   "exit_code": result.returncode,
                                   "stdout": result.stdout, "stderr": result.stderr})
        if result.returncode:
            raise ValueError("mixed registration build or proof failed")

    def observe(path: Path, expected: int, record: dict) -> None:
        if not args.wine:
            return
        prefix = (args.wine_prefix or output / "wine-prefix").resolve()
        environment = os.environ | {"WINEPREFIX": str(prefix), "WINEDEBUG": "-all",
                                    "WINEDLLOVERRIDES": "mscoree,mshtml=d"}
        rebased = path.with_stem(path.stem + "-rebased")
        rebased.write_bytes(PE32(path.read_bytes()).rebase(0x500000))
        for executable in (path, rebased):
            result = run_image(executable, [args.wine], environment, 40)
            result.update(image=str(executable), expected_exit=expected,
                          image_sha256=hashlib.sha256(executable.read_bytes()).hexdigest())
            record["runtime"].append(result)
            if result.get("exit_code") != expected:
                raise ValueError("mixed registration runtime result differs")

    try:
        libraries, report["runtime_libraries"] = load_libraries(args.runtime_libs.resolve())
        for optimization in ("O0", "O1"):
            case = output / optimization
            case.mkdir(exist_ok=True)
            original, xml = case / "original.exe", case / "proof.xml"
            record = {"optimization": optimization, "passed": False, "runtime": []}
            report["cases"].append(record)
            objects = []
            for source in SOURCES:
                obj = case / (Path(source).stem + ".obj")
                flags = (["-fms-extensions", "-fexceptions", "-fcxx-exceptions",
                          "-fno-omit-frame-pointer", "-" + optimization,
                          "-DNEVERD_SHARED_PROLOGUE", "-DNEVERD_MIXED_REGISTRATION"]
                         if source.endswith(".cpp") else [])
                run([args.compiler, "--target=i686-pc-windows-msvc", *flags,
                     "-c", FIXTURES / source, "-o", obj])
                objects.append(obj)
            run([args.linker, "/entry:mainCRTStartup", "/nodefaultlib",
                 "/machine:x86", "/subsystem:console", "/fixed:no",
                 "/dynamicbase:no", "/export:cleanup_only_parent", "/export:mixed_seh",
                 "/out:" + str(original), *objects, *libraries])
            record["original_sha256"] = hashlib.sha256(original.read_bytes()).hexdigest()
            observe(original, 0, record)
            if not args.baseline_only:
                products = [case / ("product-" + order + "-" + opt + ".exe")
                            for order in ("cxx-first", "seh-first") for opt in ("raw", "opt")]
                for product in products:
                    product.unlink(missing_ok=True)
                run([args.test_binary.resolve(), "--gtest_filter=WindowsMixedRegistration.*",
                     "--gtest_output=xml:" + str(xml)],
                    {"NEVERD_MIXED_REGISTRATION_PE32": str(original),
                     "NEVERD_MIXED_REGISTRATION_OUTPUT": str(case / "product")})
                if require_test_result(xml) != 1:
                    raise ValueError("mixed source reconstruction was missing or skipped")
                record["products"] = {}
                for product in products:
                    require_generated_entries(product)
                    record["products"][product.name] = hashlib.sha256(product.read_bytes()).hexdigest()
                    observe(product, 0, record)
                    wrong = product.with_stem(product.stem + "-wrong-trace")
                    pe = PE32(product.read_bytes())
                    changed = bytearray(pe.data)
                    struct.pack_into("<I", changed, pe.raw(pe.entry(b"cleanup_only_trace")), 3)
                    wrong.write_bytes(changed)
                    observe(wrong, 1, record)
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, struct.error, subprocess.TimeoutExpired, ET.ParseError) as error:
        report["error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("mixed registration:", "PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
