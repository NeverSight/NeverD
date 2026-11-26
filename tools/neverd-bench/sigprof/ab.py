#!/usr/bin/env python3
"""Alternate two NeverD commands and compare their CPU and wall time.

    python3 tools/neverd-bench/sigprof/ab.py --runs 7 \
      --a 'build-a/bin/neverd' --b 'NAME=VALUE build-b/bin/neverd' \
      -- decompile IMAGE --max-func=250 -o '{out}'

Each side is optional NAME=VALUE environment assignments followed by the
binary. '{out}' in the arguments becomes a per-side output file; the hashes
of every run's output are printed and must agree before any timing counts.
Runs alternate ABBA so that host load drifts affect both sides alike.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import resource
import shlex
import statistics
import subprocess
import sys
import tempfile
import time


def run(side: str, arguments: list[str], output: str):
    words = shlex.split(side)
    environment = dict(os.environ)
    while words and "=" in words[0] and not words[0].startswith(("/", ".")):
        name, value = words.pop(0).split("=", 1)
        environment[name] = value
    command = words + [arg.replace("{out}", output) for arg in arguments]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    started = time.monotonic()
    code = subprocess.run(command, env=environment, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode
    wall = time.monotonic() - started
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    digest = "-"
    if os.path.exists(output):
        with open(output, "rb") as produced:
            digest = hashlib.sha256(produced.read()).hexdigest()
        os.remove(output)
    return code, cpu, wall, digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=7)
    parser.add_argument("--a", required=True)
    parser.add_argument("--b", required=True)
    parser.add_argument("--show-pairs", action="store_true",
                        help="print each verified pair in milliseconds and its execution order")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.runs <= 0:
        parser.error("--runs must be positive")
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    results = {"A": [], "B": []}
    orders = (("A", "B"), ("B", "A"))
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as scratch:
        for index in range(args.runs):
            for name in orders[index % 2]:
                side = args.a if name == "A" else args.b
                results[name].append(run(side, arguments,
                                         os.path.join(scratch, name + ".out")))
    for name, rows in results.items():
        print(f"{name}: exit {sorted({row[0] for row in rows})} "
              f"hash {sorted({row[3] for row in rows})} "
              f"cpu {statistics.median(row[1] for row in rows):.6f}s "
              f"wall {statistics.median(row[2] for row in rows):.6f}s (medians)")
    rows = results["A"] + results["B"]
    hashes = {row[3] for row in rows}
    if any(row[0] != 0 for row in rows) or "-" in hashes or len(hashes) != 1:
        print("Paired results require every run to exit successfully and produce "
              "the same output SHA-256.", file=sys.stderr)
        return 1
    if args.show_pairs:
        print("pair order A_cpu_ms B_cpu_ms A_wall_ms B_wall_ms")
        for index, (a, b) in enumerate(zip(results["A"], results["B"])):
            order = "".join(orders[index % 2])
            print(f"{index + 1} {order} {1000 * a[1]:.3f} {1000 * b[1]:.3f} "
                  f"{1000 * a[2]:.3f} {1000 * b[2]:.3f}")

    for column, label in ((1, "cpu"), (2, "wall")):
        deltas = [1000 * (b[column] - a[column])
                  for a, b in zip(results["A"], results["B"])]
        print(f"B-A {label}: median {statistics.median(deltas):+.3f} ms, "
              f"range [{min(deltas):+.3f}, {max(deltas):+.3f}] ms")
        if any(row[column] <= 0 for row in results["A"]):
            print(f"B/A {label}: unavailable (zero baseline measurement)")
            continue
        ratios = [b[column] / a[column] - 1
                  for a, b in zip(results["A"], results["B"])]
        print(f"B/A {label}: median {statistics.median(ratios):+.1%}, "
              f"range [{min(ratios):+.1%}, {max(ratios):+.1%}]")

    return 0


if __name__ == "__main__":
    sys.exit(main())
