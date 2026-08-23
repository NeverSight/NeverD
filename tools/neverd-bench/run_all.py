#!/usr/bin/env python3
"""Iterate bin_examples/, run neverd-bench and ref_dump per binary, write report.

Resumable: maintains a `progress.json` in the output directory so we don't
re-run binaries that have already been processed. Pass --force to ignore it.

Each entry records:
  {stem, nd_total_ms, ref_total_ms, nd_funcs, ref_funcs, status, error}
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
DEFAULT_BUILD_DIR = REPO / "build"
REF_DUMP = REPO / "tools/neverd-bench/ref_dump.py"
COMPARE = REPO / "tools/neverd-bench/compare.py"
PROGRESS = "progress.json"
ND_OUTPUT_SUFFIXES = (
    ".nd.bench.json",
    ".nd.decode.json",
    ".nd.imports.json",
    ".nd.strings.json",
)
REF_OUTPUT_SUFFIXES = (
    ".ref.functions.json",
    ".ref.imports.json",
    ".ref.strings.json",
    ".ref.timings.json",
)


def load_progress(p: Path) -> dict:
    if p.exists():
        try:
            return json.loads(p.read_text())
        except Exception:
            return {}
    return {}


def save_progress(p: Path, data: dict) -> None:
    p.write_text(json.dumps(data, indent=2))


def remove_stem_outputs(out_dir: Path, stem: str, suffixes: tuple[str, ...]) -> None:
    for suffix in suffixes:
        (out_dir / f"{stem}{suffix}").unlink(missing_ok=True)


def run(cmd: list, timeout: int) -> tuple[int, str, str]:
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return 124, "", f"timeout after {timeout}s"
    except Exception as e:
        return 1, "", str(e)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", default=str(REPO / "bin_examples"))
    ap.add_argument("--out-dir", default=str(REPO / "build/bench_out"))
    ap.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR),
                    help="CMake build directory containing bin/neverd-bench")
    ap.add_argument("--bench", default=None,
                    help="explicit neverd-bench executable (overrides --build-dir)")
    ap.add_argument("--nd-timeout", type=int, default=600, help="seconds per neverd-bench run")
    ap.add_argument("--ref-timeout", type=int, default=1800, help="seconds per ref_dump run")
    ap.add_argument("--limit", type=int, default=0, help="process at most N binaries (0=all)")
    ap.add_argument("--max-size-mb", type=float, default=0.0, help="skip binaries above this size (0=no limit)")
    ap.add_argument("--skip-ref", action="store_true", help="only run neverd-bench")
    ap.add_argument("--include", default="", help="comma-sep list of stems to include (default: all)")
    ap.add_argument("--exclude", default="", help="comma-sep list of stems to skip")
    ap.add_argument("--force", action="store_true", help="ignore progress.json and re-run all")
    args = ap.parse_args()

    bin_dir = Path(args.bin_dir).resolve()
    out_dir = Path(args.out_dir).resolve()
    bench_name = "neverd-bench.exe" if sys.platform == "win32" else "neverd-bench"
    ndbench = (Path(args.bench).resolve() if args.bench else
               Path(args.build_dir).resolve() / "bin" / bench_name)
    out_dir.mkdir(parents=True, exist_ok=True)

    progress_path = out_dir / PROGRESS
    progress = {} if args.force else load_progress(progress_path)

    include = {x for x in args.include.split(",") if x}
    exclude = {x for x in args.exclude.split(",") if x}

    bins = []
    for p in sorted(bin_dir.iterdir()):
        if not p.is_file():
            continue
        if p.suffix.lower() not in (".exe", ".dll", ".so", ".dylib", ""):
            continue
        if include and p.stem not in include:
            continue
        if p.stem in exclude:
            continue
        if args.max_size_mb > 0 and p.stat().st_size > args.max_size_mb * 1024 * 1024:
            continue
        bins.append(p)

    if args.limit:
        bins = bins[: args.limit]

    if not bins:
        sys.exit(f"no input binaries selected from {bin_dir}")

    print(f"Total binaries to process: {len(bins)}")

    had_failures = False
    for i, b in enumerate(bins, 1):
        stem = b.stem
        if (not args.force) and stem in progress and progress[stem].get("status") == "ok":
            print(f"[{i}/{len(bins)}] SKIP {stem} (already done)")
            continue

        entry = {"stem": stem, "size_bytes": b.stat().st_size, "status": "running",
                  "started_at": time.time()}
        progress[stem] = entry
        save_progress(progress_path, progress)

        # neverd-bench
        remove_stem_outputs(out_dir, stem, ND_OUTPUT_SUFFIXES)
        nd_t0 = time.monotonic()
        rc, nd_out, nd_err = run([str(ndbench), "--out-dir", str(out_dir), "-q", str(b)],
                                  timeout=args.nd_timeout)
        nd_elapsed = int((time.monotonic() - nd_t0) * 1000)
        if rc != 0:
            had_failures = True
            entry["status"] = "nd_failed"
            entry["error"] = nd_err[-2000:] if nd_err else nd_out[-2000:]
            entry["nd_elapsed_ms"] = nd_elapsed
            save_progress(progress_path, progress)
            print(f"[{i}/{len(bins)}] FAIL neverd-bench {stem}: rc={rc}")
            continue
        entry["nd_elapsed_ms"] = nd_elapsed
        entry["nd_summary"] = nd_out.strip().splitlines()[-1] if nd_out.strip() else ""

        if args.skip_ref:
            entry["status"] = "ok_nd_only"
            save_progress(progress_path, progress)
            print(f"[{i}/{len(bins)}] OK(nd-only) {stem}: {entry['nd_summary']}")
            continue

        # Reference dump
        remove_stem_outputs(out_dir, stem, REF_OUTPUT_SUFFIXES)
        ref_t0 = time.monotonic()
        rc, ref_out, ref_err = run([sys.executable, str(REF_DUMP),
                                     "--out-dir", str(out_dir), str(b)],
                                    timeout=args.ref_timeout)
        ref_elapsed = int((time.monotonic() - ref_t0) * 1000)
        if rc != 0:
            had_failures = True
            entry["status"] = "ref_failed"
            entry["error"] = ref_err[-2000:] if ref_err else ref_out[-2000:]
            entry["ref_elapsed_ms"] = ref_elapsed
            save_progress(progress_path, progress)
            print(f"[{i}/{len(bins)}] FAIL ref-dump {stem}: rc={rc}")
            continue

        entry["status"] = "ok"
        entry["ref_elapsed_ms"] = ref_elapsed
        entry["ref_summary"] = ref_out.strip().splitlines()[-1] if ref_out.strip() else ""
        entry["completed_at"] = time.time()
        save_progress(progress_path, progress)
        print(f"[{i}/{len(bins)}] OK {stem}: nd={nd_elapsed}ms ref={ref_elapsed}ms")

    # Generate report
    compare_command = [sys.executable, str(COMPARE), "--bench-dir", str(out_dir)]
    for stem in sorted({binary.stem for binary in bins}):
        compare_command.extend(("--stem", stem))
    if args.skip_ref:
        compare_command.append("--schema-only")
    rc, out, err = run(compare_command, timeout=120)
    if rc == 0:
        print(out.strip())
        if had_failures:
            raise SystemExit(1)
    else:
        print("compare.py failed:", err, file=sys.stderr)
        raise SystemExit(rc)


if __name__ == "__main__":
    main()
