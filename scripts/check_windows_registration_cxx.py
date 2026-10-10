#!/usr/bin/env python3
"""Execute original and optional generated PE32 C++ ABI probes, not source rewrites."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

if __package__:
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_rewrite import PE32
    from .windows_registration_libraries import capture_libraries
else:
    from check_windows_registration_eh import run_image
    from check_windows_registration_rewrite import PE32
    from windows_registration_libraries import capture_libraries

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "unittests/lift/eh/fixtures/registration_cxx_runtime.cpp"
OBSERVATION = re.compile(
    r"neverd-registration-cxx: value=(\d+) caller=([0-9a-fA-F]{8}) "
    r"entry=([0-9a-fA-F]{8}) chain=(\d+) iterations=(\d+) trace=(\d+) "
    r"caught=(\d+)\s*")


def parent_code_end(map_path: Path, image: PE32, entry: int) -> int:
    """Bound the exported parent by real linker function starts, including funclets."""
    starts = sorted({int(match.group(1), 16) - image.base
                     for line in map_path.read_text(errors="replace").splitlines()
                     if (match := re.match(
                         r"\s+[0-9a-fA-F]{4}:[0-9a-fA-F]+\s+\S+\s+"
                         r"([0-9a-fA-F]+)\s+f\b", line))})
    if entry not in starts:
        raise ValueError("C++ parent export has no linker function owner")
    following = [start for start in starts if start > entry]
    if not following:
        raise ValueError("C++ parent has no bounded linker code extent")
    return following[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler", default="cl")
    parser.add_argument("--capture-runtime-libraries", action="store_true",
                        help="preserve the selected native x86 MSVC link libraries")
    parser.add_argument("--frame-object", type=Path,
                        help="LLVM-generated catch-frame object linked with genuine MSVC RTTI")
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("building this oracle requires native Windows MSVC")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "original-msvc-cxx-runtime",
              "platform": platform.platform(), "passed": False,
              "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
              "cases": []}
    try:
        if args.capture_runtime_libraries:
            if os.environ.get("VSCMD_ARG_TGT_ARCH") != "x86":
                raise ValueError("runtime library capture requires the x86 MSVC environment")
            report["runtime_libraries"] = capture_libraries(
                output / "runtime-libs",
                [Path(path) for path in os.environ.get("LIB", "").split(";") if path],
                os.environ.get("VCToolsVersion", ""))
        profiles = [("reference" if reference else "value", reference,
                     False, False) for reference in (False, True)]
        if args.frame_object:
            frame_object = args.frame_object.resolve()
            report["frame_object_sha256"] = hashlib.sha256(
                frame_object.read_bytes()).hexdigest()
            report["evidence"] = "original-and-generated-msvc-cxx-frame-runtime"
            profiles += [("generated-" + ("aligned-" if aligned else "") +
                          ("reference" if reference else "value"), reference,
                          True, aligned)
                         for aligned in (False, True)
                         for reference in (False, True)]
        for name, reference, generated, aligned in profiles:
            case = output / name
            case.mkdir(exist_ok=True)
            image = case / "original.exe"
            record = {"case": name, "passed": False, "observations": [],
                      "evidence": "generated-frame-abi" if generated else "original-msvc-abi"}
            report["cases"].append(record)
            # /EHs retains unwinding across the exported C-linkage throw
            # helper; /EHc would incorrectly promise that call cannot throw.
            command = [args.compiler, "/nologo", "/WX", "/std:c++17", "/EHs",
                       "/GS-", "/Od", "/Oy-", "/Z7", "/MD",
                       "/D_CRT_SECURE_NO_WARNINGS",
                       f"/DREGISTRATION_CXX_REFERENCE={int(reference)}",
                       f"/DREGISTRATION_CXX_GENERATED={int(generated)}",
                       f"/DREGISTRATION_CXX_ALIGNED={int(aligned)}",
                       f"/Fo{case / 'original.obj'}", f"/Fe{image}", str(SOURCE)]
            if generated:
                command.append(str(frame_object))
            command += ["/link", "/debug", "/incremental:no", "/dynamicbase:no",
                       "/fixed:no", f"/pdb:{case / 'original.pdb'}",
                       f"/map:{case / 'original.map'}"]
            compiled = subprocess.run(command, cwd=case, capture_output=True,
                                      text=True, errors="replace", timeout=120)
            record["compiler"] = {"command": command,
                                  "exit_code": compiled.returncode,
                                  "stdout": compiled.stdout,
                                  "stderr": compiled.stderr}
            if compiled.returncode:
                raise ValueError(f"MSVC compilation failed for {name}")
            pe = PE32(image.read_bytes())
            entry_name = ("registration_cxx_generated_" +
                          ("aligned_" if aligned else "") +
                          ("reference" if reference else "value")
                          if generated else "registration_cxx_probe")
            entry = pe.entry(entry_name.encode("ascii"))
            code_end = parent_code_end(case / "original.map", pe, entry)
            record["entry_symbol"] = entry_name
            rebased = case / "original-rebased.exe"
            rebased.write_bytes(pe.rebase(0x530000))
            for path in (image, rebased):
                data = PE32(path.read_bytes())
                result = run_image(path, [], os.environ.copy(), 60)
                observation = {"image": str(path),
                               "sha256": hashlib.sha256(data.data).hexdigest(),
                               "runtime": result, "passed": False}
                record["observations"].append(observation)
                match = OBSERVATION.fullmatch(result.get("stdout", ""))
                if result.get("exit_code") != 0 or not match:
                    raise ValueError(f"C++ exception execution failed for {path}")
                value, caller, actual_entry, chain, iterations, trace, caught = (
                    int(v, 16 if i in (1, 2) else 10)
                    for i, v in enumerate(match.groups()))
                if ((value, chain, iterations, trace, caught) !=
                        (7, 1, 4, 213, 18 if reference else 7) or
                        actual_entry - entry != data.base):
                    raise ValueError("C++ cleanup, catch object, chain or base differs")
                owners = [n for n, rva, size, _, _ in data.sections
                          if rva <= caller - data.base < rva + size]
                if (owners != [".text"] or
                        not entry <= caller - data.base < code_end):
                    raise ValueError("C++ caller PC is outside the selected parent")
                observation.update(passed=True, value=value, trace=trace,
                                   caught=caught, caller_rva=caller - data.base,
                                   caller_owner_begin_rva=entry,
                                   caller_owner_end_rva=code_end,
                                   runtime_base=data.base, chain_restored=True,
                                   iterations=iterations)
            record["passed"] = True
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    finally:
        destination = output / "cxx-runtime.json"
        destination.write_text(json.dumps(report, indent=2))
    print(("PASS" if report["passed"] else "FAIL") +
          " MSVC C++ frame runtime: " + str(destination))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
