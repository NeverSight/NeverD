#!/usr/bin/env python3
"""Run the C LLVM C prints for fragmented SEH ranges against the IR it printed.

SEHSharedTryLayoutTests.cpp writes each of its functions as IR and as printed
C.  This builds both for Windows x64 with a harness that raises from chosen
calls, runs the two programs on every input and compares their traces: the
printed `__try` layout and its normal-exit flag must run the handlers the IR
runs.  The IR is compiled without its range markers, which decide nothing
for an exception a call raises.  Wine runs the programs on other hosts.
"""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess

if __package__:
    from .check_windows_registration_eh import run_image
else:
    from check_windows_registration_eh import run_image

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "unittests/lift/eh/fixtures/seh_layout_runtime.c"
TARGET = "--target=x86_64-pc-windows-msvc"
IMPORTS = {
    "kernel32": ("RaiseException", "GetStdHandle", "WriteFile", "ExitProcess"),
    "ntdll": ("__C_specific_handler",),
}
PRELUDE = "#include <stdint.h>\n#define EXCEPTION_EXECUTE_HANDLER 1\n"
MARKER = re.compile(
    r"invoke void @llvm\.seh\.try\.(?:begin|end)\(\) to label (%[\w.]+) "
    r"unwind label %[\w.]+")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-binary", type=Path, required=True,
                        help="NeverDSEHTerminationHandlerTests")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", default="clang")
    parser.add_argument("--linker", default="lld-link")
    parser.add_argument("--dlltool", default="llvm-dlltool")
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=120)
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "evidence": "llvmc-seh-layout-runtime",
              "passed": False, "cases": []}
    environment = os.environ.copy()

    def run(command, **extra):
        result = subprocess.run([str(part) for part in command],
                                env={**environment, **extra},
                                capture_output=True, text=True,
                                errors="replace", timeout=args.timeout)
        if result.returncode:
            raise ValueError(f"{command[0]} failed: "
                             f"{(result.stdout + result.stderr).strip()[:2000]}")

    try:
        tools = {name: shutil.which(getattr(args, name))
                 for name in ("compiler", "linker", "dlltool")}
        missing = [name for name, path in tools.items() if not path]
        if missing:
            raise ValueError("missing tools: " + ", ".join(missing))
        launcher = []
        if os.name != "nt":
            wine = args.wine or shutil.which("wine64") or shutil.which("wine")
            if not wine:
                raise ValueError("Wine is unavailable; nothing was executed")
            launcher = [wine]
            prefix = (args.wine_prefix or output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEDEBUG="-all")

        sources = output / "sources"
        sources.mkdir(exist_ok=True)
        for stale in sources.iterdir():
            stale.unlink()
        run([args.test_binary.resolve(),
             "--gtest_filter=SEHSharedTryLayout.*"],
            NEVERD_SEH_LAYOUT_OUTPUT=str(sources))
        names = sorted(path.stem for path in sources.glob("*.ll"))
        if not names:
            raise ValueError("the tests wrote no function")

        libraries = []
        for library, symbols in IMPORTS.items():
            definition = output / f"{library}.def"
            definition.write_text(
                f"LIBRARY {library}.dll\nEXPORTS\n" + "\n".join(symbols) + "\n")
            libraries.append(output / f"{library}.lib")
            run([tools["dlltool"], "-m", "i386:x86-64", "-d", definition,
                 "-l", libraries[-1]])
        (output / "prelude.h").write_text(PRELUDE)
        flags = [TARGET, "-fms-extensions", "-ffreestanding",
                 "-fno-stack-protector"]

        def program(name, harness, unit, tag):
            image = output / f"{name}-{tag}.exe"
            run([tools["linker"], "/nologo", "/machine:x64",
                 "/subsystem:console", "/entry:entry", "/nodefaultlib",
                 harness, unit, *libraries, f"/out:{image}"])
            result = run_image(image, launcher, environment, args.timeout)
            if result.get("exit_code") != 0 or not result.get("stdout"):
                raise ValueError(f"{image.name} did not run: "
                                 f"{result.get('error', result.get('stderr'))}")
            return result["stdout"]

        for name in names:
            text = (sources / f"{name}.ll").read_text()
            header = re.search(rf"define i32 @{re.escape(name)}\(([^)]*)\)", text)
            if not header:
                raise ValueError(f"{name}.ll does not define {name}")
            parameters = header.group(1).strip()
            count = len(parameters.split(",")) if parameters else 0
            (output / f"{name}.ll").write_text(MARKER.sub(r"br label \1", text))
            harness = output / f"{name}-harness.obj"
            run([tools["compiler"], *flags, "-O1", f"-DFUNC={name}",
                 f"-DARGS={count}", "-c", HARNESS, "-o", harness])
            unit = output / f"{name}-ir.obj"
            run([tools["compiler"], TARGET, "-O1", "-Wno-override-module",
                 "-c", output / f"{name}.ll", "-o", unit])
            expected = program(name, harness, unit, "ir")
            case = {"function": name, "runs": expected.count("\n"),
                    "traces": len(set(expected.split())), "printed": {}}
            report["cases"].append(case)
            for level in ("-O0", "-O2"):
                unit = output / f"{name}-c{level}.obj"
                run([tools["compiler"], *flags, level, "-include",
                     output / "prelude.h", "-c", sources / f"{name}.c",
                     "-o", unit])
                observed = program(name, harness, unit, "c" + level)
                case["printed"][level] = observed == expected
                if observed != expected:
                    pairs = zip(expected.split("\n"), observed.split("\n"))
                    index, (want, got) = next(
                        (i, pair) for i, pair in enumerate(pairs)
                        if pair[0] != pair[1])
                    case["first_difference"] = {
                        "level": level, "run": index, "ir": want, "c": got}
                    raise ValueError(
                        f"{name} at {level}: run {index} is {got!r}, "
                        f"the IR's is {want!r}")
        report["passed"] = True
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    destination = output / "llvmc-seh-layout-runtime.json"
    destination.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{'PASS' if report['passed'] else 'FAIL'} LLVM C SEH layout: "
          f"{destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
