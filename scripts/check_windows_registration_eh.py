#!/usr/bin/env python3
"""Run the checked-in PE32 EH probes, optionally against rewritten images.

Baseline execution is explicitly separate from reconstruction evidence. A
differential run requires a changed PE32 image for every original probe. Changed
bytes alone do not prove EH reconstruction or execution of a replaced function.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]


def probe_cases() -> list[tuple[Path, str]]:
    cases = []
    for security in ("no-gs", "gs"):
        for optimization in ("o0", "o2"):
            for source, banner in (("seh_probe", "SEH probe passed"),
                                   ("cxx_eh_probe", "C++ EH probe passed")):
                name = f"{source}-msvc-x86-native-{security}-{optimization}.exe"
                cases.append((Path(security) / optimization / "abi-probe" / name,
                              banner))
    return cases


def image_digest(path: Path) -> str:
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"not a PE image: {path}")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe > len(data) - 26 or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"invalid PE headers: {path}")
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    if (machine, magic) != (0x14C, 0x10B):
        raise ValueError(f"expected an x86 PE32 image: {path}")
    return hashlib.sha256(data).hexdigest()


def run_image(path: Path, launcher: list[str], environment: dict[str, str],
              timeout: float) -> dict:
    command = [*launcher, str(path.resolve())]
    started = time.monotonic()
    try:
        # Wine services can inherit the launcher's output handles and outlive
        # the PE process. File capture waits for the process exit, not for
        # unrelated services to close a pipe. A live probe still times out.
        with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
            observation = {"command": command}
            try:
                result = subprocess.run(command, env=environment, stdout=output,
                                        stderr=errors, timeout=timeout, check=False)
                observation["exit_code"] = result.returncode
            except subprocess.TimeoutExpired:
                observation["error"] = f"timeout after {timeout}s"
            output.seek(0)
            errors.seek(0)
            observation.update(
                stdout=output.read().decode(errors="replace").replace("\r\n", "\n"),
                stderr=errors.read().decode(errors="replace").replace("\r\n", "\n"),
                seconds=round(time.monotonic() - started, 3))
            return observation
    except OSError as error:
        return {"command": command, "error": str(error),
                "seconds": round(time.monotonic() - started, 3)}


def observe_case(relative: Path, banner: str, originals: Path,
                 patched: Path | None, launcher: list[str],
                 environment: dict[str, str], timeout: float) -> dict:
    observation = {"case": relative.as_posix(), "passed": False}
    try:
        original = originals / relative
        observation["original_sha256"] = image_digest(original)
        # Preflight the replacement before executing either member of a pair.
        replacement = patched / relative if patched else None
        if replacement is not None:
            observation["patched_sha256"] = image_digest(replacement)
            if observation["original_sha256"] == observation["patched_sha256"]:
                raise ValueError("replacement is byte-identical to the original")
        original_run = run_image(original, launcher, environment, timeout)
        observation["original"] = original_run
        if original_run.get("exit_code") != 0 or \
                original_run.get("stdout", "").strip() != banner:
            raise ValueError("original runtime probe failed")
        if replacement is not None:
            patched_run = run_image(replacement, launcher, environment, timeout)
            observation["patched"] = patched_run
            if patched_run.get("exit_code") != original_run["exit_code"] or \
                    patched_run.get("stdout") != original_run["stdout"]:
                raise ValueError("rewritten runtime behavior differs from original")
        observation["passed"] = True
    except (OSError, ValueError) as error:
        observation["error"] = str(error)
    return observation


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original-root", type=Path,
                        default=ROOT / "unittests/corpus/corpus/windows-eh/msvc/x86/native")
    parser.add_argument("--patched-root", type=Path,
                        help="matching directory of rewritten probes; requires all eight pairs")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--wine", help="Wine launcher (Linux); Windows executes PE32 directly")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"schema": 1, "platform": platform.platform(),
              "evidence": "changed-image-runtime" if args.patched_root else "original-runtime",
              "original_root": str(args.original_root.resolve()), "cases": [],
              "passed": False}
    environment = os.environ.copy()
    launcher = []
    if os.name != "nt":
        wine = args.wine or shutil.which("wine") or shutil.which("wine64-stable")
        if not wine:
            report["error"] = "Wine is unavailable; PE32 runtime coverage was not executed"
        else:
            launcher = [wine]
            prefix = (args.wine_prefix or args.output / "wine-prefix").resolve()
            prefix.mkdir(parents=True, exist_ok=True)
            environment.update(WINEPREFIX=str(prefix), WINEDEBUG="-all")
            report["wine_prefix"] = str(prefix)
    if "error" not in report:
        for relative, banner in probe_cases():
            observation = observe_case(relative, banner, args.original_root,
                                       args.patched_root, launcher, environment,
                                       args.timeout)
            report["cases"].append(observation)
            print(f"{'PASS' if observation['passed'] else 'FAIL'} {relative}", flush=True)
        report["passed"] = all(case["passed"] for case in report["cases"])
    destination = args.output / "registration-eh.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['evidence']}: {destination}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
