#!/usr/bin/env python3
"""Replay the exact reconstructed PE32 evidence on the native Windows loader.

Linux may use --wine to check this replay harness locally. Every input hash,
case, code owner, relocation base and expected negative cookie result is
revalidated rather than inheriting a prior runner's success flag.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess

if __package__:
    from .check_windows_registration_cookie import require_cookie_outcome
    from .check_windows_registration_eh import run_image
    from .check_windows_registration_rewrite import CASES, CONTROL_FP_MODES, PE32, observe
    from .windows_registration_runtime import validate_runtime
else:
    from check_windows_registration_cookie import require_cookie_outcome
    from check_windows_registration_eh import run_image
    from check_windows_registration_rewrite import CASES, CONTROL_FP_MODES, PE32, observe
    from windows_registration_runtime import validate_runtime


def source_cases(profile: str) -> dict[str, tuple[int, int]]:
    if profile not in ("all", "r5", "r4", "seh3-eh4"):
        raise ValueError("unknown source reconstruction profile")
    limit = 46 if profile == "r5" else 30
    return {name: case for name, case in CASES.items()
            if (profile == "all" or case[0] < limit) and
            (profile == "all" or not (name.startswith("eh4-") and "longjmp-" in name)) and
            (profile != "seh3-eh4" or not name.startswith("eh4-gs-"))}


def source_records(source: dict, case: str, trace: int) -> list[dict]:
    control_fp_mode = source.get("control_fp_mode", "output-word")
    if control_fp_mode not in CONTROL_FP_MODES or \
            (control_fp_mode != "output-word" and case != "longjmp-control-fp"):
        raise ValueError("source floating-point control mode is invalid")
    control_fp_veneer = source.get("control_fp_veneer", False)
    if not isinstance(control_fp_veneer, bool) or \
            (control_fp_veneer and case != "longjmp-control-fp"):
        raise ValueError("source floating-point call form is invalid")
    labels = ["original", "patched", "product-patched", "collision-patched"]
    if source.get("schema") == 2:
        labels.extend(f"cli-{mode}-{kind}" for mode in ("section", "inplace")
                      for kind in ("ir", "exe-no-opt", "exe-opt"))
    elif source.get("schema") == 1:
        labels.extend(("cli-section", "cli-inplace"))
    else:
        raise ValueError("unknown source reconstruction evidence schema")
    expected = {(label + suffix + ".exe", label != "original", base)
                for label in labels
                for suffix, base in (("", 0x400000), ("-rebased", 0x18000000))}
    records = source.get("observations", [])
    if source.get("case") != case or source.get("passed") is not True or \
            source.get("expected_trace") != trace or len(records) != len(expected) or \
            {(Path(r["image"]).name, r["generated"], r["runtime_base"])
             for r in records} != expected:
        raise ValueError("source reconstruction evidence is incomplete")
    return records


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--wine")
    parser.add_argument("--wine-prefix", type=Path)
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--source-only", action="store_true",
                        help="replay every source case without separate cookie-object fixtures")
    parser.add_argument("--source-profile", choices=("all", "r5", "r4", "seh3-eh4"),
                        default="all", help="select an explicitly older evidence checkpoint")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be positive and finite")
    if os.name != "nt" and not args.wine:
        parser.error("native Windows is required unless --wine is explicit")
    launcher = [args.wine] if args.wine else []
    runtime_name = "Wine" if launcher else "Windows"
    environment = os.environ.copy()
    if args.wine_prefix:
        environment.update(WINEPREFIX=str(args.wine_prefix.resolve()), WINEDEBUG="-all")
    root = args.evidence_root.resolve()
    report = {"schema": 1, "platform": platform.platform(),
              "evidence": "wine-replay" if launcher else "native-windows-replay",
              "passed": False, "source_profile": args.source_profile,
              "source_only": args.source_only,
              "source_cases": [], "cookies": [], "failures": []}

    def checked_image(parent: Path, record: dict) -> Path:
        name = Path(record["image"]).name
        image = parent / name
        if image.parent != parent or not image.is_file() or \
                hashlib.sha256(image.read_bytes()).hexdigest() != record["sha256"]:
            raise ValueError("runtime evidence image is missing or changed")
        return image

    try:
        for case, (_, trace) in source_cases(args.source_profile).items():
            parent = root / ("source-" + case)
            source = json.loads((parent / "registration-rewrite.json").read_text())
            records = source_records(source, case, trace)
            runtime = source.get("seh_runtime")
            case_environment = environment.copy()
            if runtime is not None:
                validate_runtime(parent, runtime)
                case_environment["WINEDLLOVERRIDES"] = "vcruntime140=n"
            observations = []
            control_fp_mode = source.get("control_fp_mode", "output-word")
            report["source_cases"].append({"case": case, "observations": observations,
                                           "control_fp_mode": control_fp_mode,
                                           "control_fp_veneer": source.get("control_fp_veneer", False)})
            for record in records:
                try:
                    observation = observe(checked_image(parent, record), record["generated"],
                                          launcher, case_environment, args.timeout, trace,
                                          control_fp_mode)
                    if observation["runtime_base"] != record["runtime_base"]:
                        raise ValueError("native loader changed the forced image base")
                    observations.append(observation)
                except (OSError, ValueError) as error:
                    report["failures"].append({"case": case,
                                               "image": Path(record["image"]).name,
                                               "error": str(error)})
            complete = len(observations) == len(records)
            print(f"{'PASS' if complete else 'FAIL'} {runtime_name} source {case}: "
                  f"{len(observations)}/{len(records)} executions", flush=True)
        cookie_cases = () if args.source_only else (("eh4-cookie-runtime", False),
                                                   ("eh4-gs-cookie-runtime", True))
        for name, gs in cookie_cases:
            parent = root / name
            source = json.loads((parent / "cookie-runtime.json").read_text())
            records = source["observations"]
            expected = {(base, label) for base in (0x400000, 0x18000000)
                        for label in (("valid", "corrupt-eh", "corrupt-gs") if gs
                                      else ("valid", "corrupt-eh"))}
            if not source.get("passed") or source.get("gs_required") != gs or \
                    len(records) != len(expected) or \
                    {(r["image_base"], r["cookie_corrupted"]) for r in records} != expected:
                raise ValueError("EH4 cookie runtime evidence is incomplete")
            for record in records:
                image = checked_image(parent, record)
                pe = PE32(image.read_bytes())
                if pe.base != record["image_base"] or \
                        pe.u16(pe.optional + 70) & 0x40:
                    raise ValueError("cookie probe lost its forced relocation base")
                result = run_image(image, launcher, environment, args.timeout)
                report["cookies"].append({"image": str(image), "runtime": result})
                try:
                    require_cookie_outcome(result, record["cookie_corrupted"] != "valid")
                except ValueError as error:
                    report["failures"].append({"image": str(image),
                                               "error": str(error), "runtime": result})
            print(f"CHECKED {runtime_name} {name}: {len(records)} executions", flush=True)
        if report["failures"]:
            raise ValueError(f"{len(report['failures'])} replay executions failed")
        report["passed"] = True
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'} PE32 replay: {args.output}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
