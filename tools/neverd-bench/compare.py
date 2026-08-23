#!/usr/bin/env python3
"""Compare NeverD-side and reference JSON dumps and emit `bench_report.md`.

Looks for the current consolidated NeverD result and reference-side dumps:
    <stem>.nd.bench.json       /  <stem>.ref.functions.json
    <stem>.nd.imports.json     /  <stem>.ref.imports.json
    <stem>.nd.strings.json     /  <stem>.ref.strings.json
                                  <stem>.ref.timings.json

Pass/fail thresholds match the plan (Step 7.2).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


PASS_FUNC_OVERLAP = 0.97
PASS_LLVM_RATE    = 0.90
PASS_STR_RATE     = 0.99
PASS_XREF_RECALL  = 0.90


def load_json(p: Path) -> Any | None:
    if not p.exists():
        return None
    try:
        with open(p) as f:
            return json.load(f)
    except Exception:
        return None


def find_stems(d: Path) -> list[str]:
    stems = set()
    for p in d.glob("*.nd.bench.json"):
        stems.add(p.name.replace(".nd.bench.json", ""))
    return sorted(stems)


def parse_addr(value: Any) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def load_bench_sample(stem: str, d: Path) -> dict:
    sample = load_json(d / f"{stem}.nd.bench.json")
    if not isinstance(sample, dict):
        raise ValueError("invalid JSON or top-level value is not an object")
    if not isinstance(sample.get("functions"), list):
        raise ValueError("missing functions array")
    if not sample["functions"]:
        raise ValueError("functions array is empty")
    if not isinstance(sample.get("audit_functions"), list):
        raise ValueError("missing audit_functions array")
    if not sample["audit_functions"]:
        raise ValueError("audit_functions array is empty")
    if not isinstance(sample.get("total_time_ms"), (int, float)):
        raise ValueError("missing numeric total_time_ms")
    return sample


def jaccard(a: set, b: set) -> float:
    if not a and not b:
        return 1.0
    u = a | b
    if not u:
        return 1.0
    return len(a & b) / len(u)


def compare_one(stem: str, d: Path) -> dict:
    nd_bench = load_bench_sample(stem, d)
    nd_funcs = nd_bench.get("functions", [])
    nd_imps  = load_json(d / f"{stem}.nd.imports.json")   or []
    nd_strs  = load_json(d / f"{stem}.nd.strings.json")   or []
    nd_lift  = nd_bench.get("audit_functions", [])

    ref_funcs = load_json(d / f"{stem}.ref.functions.json") or []
    ref_imps  = load_json(d / f"{stem}.ref.imports.json") or []
    ref_strs  = load_json(d / f"{stem}.ref.strings.json") or []
    ref_time  = load_json(d / f"{stem}.ref.timings.json") or {}

    r: dict = {"stem": stem}

    # --- Functions ---
    nd_addrs = {parse_addr(f["entry"]) for f in nd_funcs}
    ref_addrs = {parse_addr(f["addr"]) for f in ref_funcs}
    inter = nd_addrs & ref_addrs
    r["nd_func_count"] = len(nd_addrs)
    r["ref_func_count"] = len(ref_addrs)
    r["func_overlap"] = (len(inter) / len(ref_addrs)) if ref_addrs else 1.0
    r["func_overlap_pass"] = r["func_overlap"] >= PASS_FUNC_OVERLAP

    # --- Imports (compare by (dll, symbol) tuple, case-insensitive on symbol) ---
    def imp_key(e):
        return (str(e.get("dll", "")).lower(),
                str(e.get("symbol", e.get("name", ""))).lower())
    nd_iset = {imp_key(e) for e in nd_imps}
    ref_iset = {imp_key(e) for e in ref_imps}
    # When the reference dump omits DLL field, fall back to symbol-only comparison
    if all(k[0] == "" for k in ref_iset):
        nd_iset = {("", k[1]) for k in nd_iset}
    r["nd_imp_count"] = len(nd_iset)
    r["ref_imp_count"] = len(ref_iset)
    only_nd  = nd_iset - ref_iset
    only_ref = ref_iset - nd_iset
    r["imp_only_nd"] = len(only_nd)
    r["imp_only_ref"] = len(only_ref)
    r["imp_pass"] = (len(only_nd) == 0 and len(only_ref) == 0)

    # --- Strings (compare by addr; content equality) ---
    nd_smap = {int(s["addr"]): s.get("content", "") for s in nd_strs}
    ref_smap = {int(s["addr"]): s.get("content", "") for s in ref_strs}
    shared = nd_smap.keys() & ref_smap.keys()
    same_content = sum(1 for a in shared if nd_smap[a] == ref_smap[a])
    r["nd_str_count"] = len(nd_smap)
    r["ref_str_count"] = len(ref_smap)
    r["str_addr_overlap"] = (len(shared) / len(ref_smap)) if ref_smap else 1.0
    r["str_content_match"] = (same_content / len(shared)) if shared else 1.0
    r["str_pass"] = (
        r["str_addr_overlap"] >= PASS_XREF_RECALL
        and r["str_content_match"] >= PASS_STR_RATE
    )

    # --- Lift (only against reference-known funcs) ---
    lift_by_addr = {parse_addr(x["entry"]): x for x in nd_lift}
    ref_func_addrs = ref_addrs
    n_total = len(ref_func_addrs) or 1
    n_low = sum(1 for a in ref_func_addrs if lift_by_addr.get(a, {}).get("low_ir"))
    n_med = sum(1 for a in ref_func_addrs if lift_by_addr.get(a, {}).get("med_ir"))
    # The consolidated audit has no separate per-function HighIR flag.  An
    # LLVM definition is the available positive witness that the function
    # traversed both HighIR and LLVM emission successfully.
    n_high = sum(
        1 for a in ref_func_addrs
        if lift_by_addr.get(a, {}).get("llvm_definition")
    )
    n_llvm = n_high
    r["lift_low_rate"]  = n_low / n_total
    r["lift_med_rate"]  = n_med / n_total
    r["lift_high_rate"] = n_high / n_total
    r["lift_llvm_rate"] = n_llvm / n_total
    r["lift_pass"] = r["lift_llvm_rate"] >= PASS_LLVM_RATE

    # --- Timings ---
    r["nd_total_ms"]  = int(nd_bench.get("total_time_ms") or 0)
    r["ref_total_ms"] = int(ref_time.get("total_ms") or 0)
    r["nd_rss_mb"]    = float(nd_bench.get("peak_rss_mb") or 0.0)
    r["speed_pass"] = (r["nd_total_ms"] > 0 and r["ref_total_ms"] > 0
                        and r["nd_total_ms"] < r["ref_total_ms"])
    if r["nd_total_ms"] > 0 and r["ref_total_ms"] > 0:
        r["speedup"] = r["ref_total_ms"] / max(1, r["nd_total_ms"])
    else:
        r["speedup"] = 0.0

    r["overall_pass"] = (
        r["func_overlap_pass"]
        and r["imp_pass"]
        and r["str_pass"]
        and r["lift_pass"]
        and r["speed_pass"]
    )
    return r


def fmt_pct(x: float) -> str:
    return f"{x * 100:.1f}%"


def write_report(rows: list[dict], out: Path) -> None:
    n_pass = sum(1 for r in rows if r["overall_pass"])
    lines = []
    lines.append("# NeverD vs Reference Bench Report\n")
    lines.append(f"**Binaries:** {len(rows)}  |  **Overall pass:** {n_pass}/{len(rows)}\n\n")

    lines.append("## Summary\n")
    lines.append("| binary | nd_funcs | ref_funcs | func_overlap | llvm_rate | str_match | nd_ms | ref_ms | speedup | overall |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|:---:|")
    for r in rows:
        overall = "✅" if r["overall_pass"] else "❌"
        speedup = f"{r['speedup']:.1f}x" if r["speedup"] > 0 else "n/a"
        lines.append(
            f"| {r['stem']} | {r['nd_func_count']} | {r['ref_func_count']} | "
            f"{fmt_pct(r['func_overlap'])} | {fmt_pct(r['lift_llvm_rate'])} | "
            f"{fmt_pct(r['str_content_match'])} | {r['nd_total_ms']} | "
            f"{r['ref_total_ms']} | {speedup} | {overall} |"
        )

    lines.append("\n## Detail\n")
    for r in rows:
        lines.append(f"### {r['stem']}")
        lines.append("")
        lines.append("| metric | value | pass |")
        lines.append("|---|---|:---:|")
        lines.append(f"| Function overlap | {fmt_pct(r['func_overlap'])} | {'✅' if r['func_overlap_pass'] else '❌'} |")
        lines.append(f"| Imports match | only_nd={r['imp_only_nd']} only_ref={r['imp_only_ref']} | {'✅' if r['imp_pass'] else '❌'} |")
        lines.append(f"| String content match | {fmt_pct(r['str_content_match'])} | {'✅' if r['str_pass'] else '❌'} |")
        lines.append(f"| Lift Low  | {fmt_pct(r['lift_low_rate'])} | |")
        lines.append(f"| Lift Med  | {fmt_pct(r['lift_med_rate'])} | |")
        lines.append(f"| Lift High | {fmt_pct(r['lift_high_rate'])} | |")
        lines.append(f"| Lift LLVM | {fmt_pct(r['lift_llvm_rate'])} | {'✅' if r['lift_pass'] else '❌'} |")
        lines.append(f"| NeverD time | {r['nd_total_ms']} ms (rss {r['nd_rss_mb']:.1f}MB) | |")
        lines.append(f"| Ref time    | {r['ref_total_ms']} ms | |")
        lines.append(f"| Speed (NeverD < Ref) | speedup={r['speedup']:.1f}x | {'✅' if r['speed_pass'] else '❌'} |")
        lines.append("")

    out.write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench-dir", required=True,
                    help="directory containing *.nd.*.json and *.ref.*.json")
    ap.add_argument("--out", default=None, help="output bench_report.md path")
    ap.add_argument("--stem", action="append", default=[],
                    help="compare only this binary stem (repeatable)")
    ap.add_argument("--schema-only", action="store_true",
                    help="validate NeverD samples without reference comparison")
    args = ap.parse_args()

    d = Path(args.bench_dir)
    if not d.exists():
        sys.exit(f"bench dir not found: {d}")
    out = Path(args.out) if args.out else d / "bench_report.md"

    stems = sorted(set(args.stem)) if args.stem else find_stems(d)
    if not stems:
        sys.exit(f"no .nd.bench.json samples found in {d}")

    if args.schema_only:
        valid_samples = 0
        for stem in stems:
            try:
                load_bench_sample(stem, d)
                valid_samples += 1
            except (KeyError, TypeError, ValueError) as error:
                print(f"invalid benchmark sample {stem}: {error}", file=sys.stderr)
        if valid_samples != len(stems):
            raise SystemExit(1)
        print(f"validated {valid_samples} benchmark sample(s)")
        return

    rows = []
    invalid_samples = 0
    for stem in stems:
        try:
            rows.append(compare_one(stem, d))
        except (KeyError, TypeError, ValueError) as error:
            invalid_samples += 1
            print(f"invalid benchmark sample {stem}: {error}", file=sys.stderr)
    if not rows:
        sys.exit(f"no valid .nd.bench.json samples found in {d}")
    rows.sort(key=lambda r: (not r["overall_pass"], r["stem"]))

    write_report(rows, out)
    print(f"wrote {out} ({len(rows)} binaries, {sum(1 for r in rows if r['overall_pass'])} passing)")
    if invalid_samples or any(not row["overall_pass"] for row in rows):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
