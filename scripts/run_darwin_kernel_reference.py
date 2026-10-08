#!/usr/bin/env python3
"""Run the original Darwin workloads on a native macOS kernel, without NeverD."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "unittests/emulation/fixtures/DarwinNativeCases.def"


def read_cases(path: Path = CASES) -> list[tuple[str, int, bytes]]:
    literal = r'("(?:[^"\\]|\\.)*")'
    definitions = re.sub(r"^[ \t]*//[^\n]*$", "", path.read_text(encoding="utf-8"), flags=re.M)
    pattern = re.compile(
        rf"NEVERD_DARWIN_NATIVE_CASE\(\s*{literal},\s*(\d+),\s*{literal}\s*\)"
    )
    cases = [(json.loads(mode), int(status), json.loads(output).encode())
             for mode, status, output in pattern.findall(definitions)]
    if (not cases or pattern.sub("", definitions).strip()
            or len({case[0] for case in cases}) != len(cases)
            or any(not mode or not 0 <= status <= 255 for mode, status, _ in cases)):
        raise ValueError("invalid native Darwin reference inventory")
    return cases


def native_architecture(expected: str) -> str:
    actual = {"arm64": "arm64", "x86_64": "x86_64"}.get(platform.machine())
    if platform.system() != "Darwin" or actual != expected:
        raise ValueError("reference requires the requested native macOS host architecture")
    translated = subprocess.run(
        ["sysctl", "-in", "sysctl.proc_translated"], capture_output=True, text=True,
        check=False,
    )
    if translated.returncode not in (0, 1) or translated.stdout.strip() not in ("", "0"):
        raise ValueError("translated processes cannot establish native kernel reference evidence")
    return actual


def execute_cases(program: Path, cases: list[tuple[str, int, bytes]]) -> list[dict]:
    with (tempfile.TemporaryDirectory(prefix="neverd-darwin-files-") as directory,
          tempfile.TemporaryDirectory(prefix="neverd-darwin-links-") as links,
          tempfile.TemporaryDirectory(prefix="neverd-darwin-mixed-links-") as mixed,
          tempfile.TemporaryDirectory(prefix="neverd-darwin-created-links-") as created,
          tempfile.TemporaryDirectory(prefix="neverd-darwin-unlinked-links-") as removed):
        input_file = Path(directory) / "data"
        (Path(directory) / "empty").mkdir()
        return _execute_cases(program, cases, input_file, Path(links), Path(mixed), Path(created), Path(removed))


def _execute_cases(program: Path, cases: list[tuple[str, int, bytes]], input_file: Path,
                   symbolic_root: Path, mixed_root: Path, created_root: Path, removed_root: Path) -> list[dict]:
    results = []
    for mode, status, output in cases:
        case_input = input_file
        if mode == "symbolic-links":
            catalogue = symbolic_root / "catalogue"
            catalogue.mkdir()
            (catalogue / "empty").mkdir()
            for name, target in (("link", "data"), ("chain", "link"),
                                 ("dangling", "missing"), ("cycle", "cycle"),
                                 ("dirlink", "empty")):
                (catalogue / name).symlink_to(target)
            case_input = catalogue / "data"
        if mode in ("symbolic-link-mutations", "symbolic-link-creation", "symbolic-link-unlink"):
            roots = {"symbolic-link-mutations": mixed_root,
                     "symbolic-link-creation": created_root,
                     "symbolic-link-unlink": removed_root}
            catalogue = roots[mode] / "catalogue"
            (catalogue / "static").mkdir(parents=True)
            (catalogue / "work").mkdir()
            for name, target in (("alias", "../work"), ("data-link", "../work/data"),
                                 ("missing-link", "../work/new")):
                (catalogue / "static" / name).symlink_to(target)
            case_input = catalogue / "work" / "data"
        case_input.write_bytes(b"0123456789")
        try:
            result = subprocess.run([str(program), mode, str(case_input)], capture_output=True, timeout=5)
            results.append({
                "mode": mode, "exit_status": result.returncode,
                "stdout_hex": result.stdout.hex(), "stderr_hex": result.stderr.hex(),
                "passed": result.returncode == status and result.stdout == output
                and not result.stderr,
            })
        except subprocess.TimeoutExpired as error:
            results.append({
                "mode": mode, "exit_status": None, "passed": False,
                "stdout_hex": (error.stdout or b"").hex(),
                "stderr_hex": (error.stderr or b"").hex(), "error": "timeout",
            })
    return results


def run(evidence: Path, expected: str) -> int:
    architecture = native_architecture(expected)
    cases = read_cases()
    evidence.mkdir(parents=True, exist_ok=True)
    sdk = evidence / "empty-sdk"
    sdk.mkdir(exist_ok=True)
    source = ROOT / "unittests/emulation/fixtures/darwin_process.c"
    obj, program = evidence / "native.o", evidence / "native"
    compiler = ["xcrun", "clang"]
    subprocess.run([
        *compiler, f"--target={architecture}-apple-macos11", "-std=c11",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc",
        "-isysroot", str(sdk), "-fno-vectorize", "-fno-slp-vectorize",
        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-O1",
        "-c", str(source), "-o", str(obj),
    ], check=True)
    subprocess.run([*compiler, "-arch", architecture, str(obj), "-o", str(program)], check=True)
    results = execute_cases(program, cases)
    summary = {
        "commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_dirty": bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True,
        ).strip()),
        "host_architecture": architecture, "macos_version": platform.mac_ver()[0],
        "compiler": subprocess.check_output([*compiler, "--version"], text=True).splitlines()[0],
        "execution": "native-macos-kernel", "cases": results,
    }
    (evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary), flush=True)
    return int(any(not result["passed"] for result in results))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--architecture", choices=("arm64", "x86_64"), required=True)
    args = parser.parse_args()
    return run(args.evidence.resolve(), args.architecture)


if __name__ == "__main__":
    raise SystemExit(main())
