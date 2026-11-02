#!/usr/bin/env python3
"""Symbolize and summarize samples written by sigprof.cpp.

    python3 tools/neverd-bench/sigprof/report.py PREFIX.PID.bin [--focus RE]

The report gives self and inclusive shares of CPU samples, CPU and thread
counts per pipeline phase, and the wall time during which at most
SERIAL_THREADS threads were sampled. A phase that holds one thread on a
many-core host costs far more wall time than its CPU share suggests.
"""
from __future__ import annotations

import argparse
import bisect
import collections
import re
import struct
import subprocess
from pathlib import Path

MAGIC = 0x4E44505246
# Time buckets in which no more threads than this were sampled are serial.
SERIAL_THREADS = 2
# In a module with exported symbols only (libc), an address this far past the
# nearest symbol lies in an unexported function.
DISTANT_EXPORT = 0x4000
PHASE = re.compile(
    r"Pipeline::(\w+)|(HighCEmitter::emit|LLVMCEmitter::\w+)|(X87\w+::\w+)"
    r"|(CFGBuilder::build)\b|(MedToHighConverter::convert)\b"
    r"|(LowToMedConverter::convert)\b|(FuncDetector\w*::\w+)"
    r"|(SignatureDB::\w+)|(\w*Loader\w*::\w+)|(MedLLVMEmitter\w*::\w+)")
# Matches that only say a pipeline is running; report the phase inside them.
PHASE_WRAPPERS = {"run", "runImpl", "runNative"}


class Module:
    """One mapped ELF file: its load segments and function symbols."""

    def __init__(self, path: str):
        self.path = path
        self.loads = []
        headers = subprocess.run(["readelf", "-lW", path], capture_output=True,
                                 text=True).stdout
        for line in headers.splitlines():
            field = line.split()
            if field and field[0] == "LOAD":
                self.loads.append((int(field[1], 16), int(field[2], 16),
                                   int(field[4], 16)))
        self.dynamic_only = False
        symbols = []
        for flags in ([], ["-D"]):
            self.dynamic_only = bool(flags)
            listing = subprocess.run(
                ["nm", "-C", "-n", "--defined-only", *flags, path],
                capture_output=True, text=True).stdout
            for line in listing.splitlines():
                field = line.split(" ", 2)
                if len(field) == 3 and field[1] in "tTwW" and field[0]:
                    symbols.append((int(field[0], 16), field[2]))
            if symbols:
                break
        symbols.sort()
        self.addresses = [address for address, _ in symbols]
        self.names = [name for _, name in symbols]

    def name_at(self, file_offset: int) -> str | None:
        for offset, vaddr, size in self.loads:
            if offset <= file_offset < offset + size:
                address = file_offset - offset + vaddr
                break
        else:
            return None
        index = bisect.bisect_right(self.addresses, address) - 1
        if index < 0:
            return None
        if self.dynamic_only and address - self.addresses[index] > DISTANT_EXPORT:
            return f"<{Path(self.path).name}+{address:#x}>"
        return self.names[index]


def short_name(name: str) -> str:
    """Drop the outer parameter list so that overloads and clones group."""
    name = re.sub(r"\s*\[clone [^\]]*\]", "", name)
    depth = 0
    for index in range(len(name) - 1, -1, -1):
        if name[index] == ")":
            depth += 1
        elif name[index] == "(":
            depth -= 1
            if depth == 0 and not name[:index].endswith("operator"):
                return name[:index]
    return name


def phase(stack: list[str]) -> str:
    """The outermost pipeline phase on a stack, root first."""
    for name in reversed(stack):
        match = PHASE.search(name)
        if match:
            label = next(group for group in match.groups() if group)
            if label not in PHASE_WRAPPERS:
                return label
    for name in reversed(stack):
        if name.startswith("neverd::") and "runTaskWorkers" not in name:
            return name[:60]
    return "-"


def load(path: str) -> list[tuple[int, int, list[str]]]:
    data = Path(path).read_bytes()
    magic, _, dropped = struct.unpack_from("<3Q", data, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: not a sigprof sample file")
    if dropped:
        print(f"warning: {dropped} samples dropped by a full buffer")
    words = struct.unpack_from(f"<{(len(data) - 24) // 8}Q", data, 24)
    maps = []
    for line in Path(path[: -len(".bin")] + ".maps").read_text().splitlines():
        field = line.split(None, 5)
        if len(field) == 6 and field[5].startswith("/"):
            low, high = (int(part, 16) for part in field[0].split("-"))
            maps.append((low, high, int(field[2], 16), field[5].strip()))
    maps.sort()
    starts = [entry[0] for entry in maps]
    modules: dict[str, Module] = {}
    names: dict[int, str] = {}

    def symbolize(pc: int) -> str:
        if pc not in names:
            index = bisect.bisect_right(starts, pc) - 1
            label = "??"
            if index >= 0 and pc < maps[index][1]:
                low, _, offset, file = maps[index]
                if file not in modules:
                    modules[file] = Module(file)
                module = modules[file]
                found = module.name_at(pc - low + offset)
                label = short_name(found) if found else f"?? ({Path(file).name})"
            names[pc] = label
        return names[pc]

    samples = []
    cursor = 0
    while cursor + 2 < len(words):
        frames = words[cursor] & 0xFFFFFFFF
        if not frames or cursor + 2 + frames > len(words):
            break
        stack = words[cursor + 2: cursor + 2 + frames]
        # Return addresses point after the call; step back into it.
        samples.append((words[cursor] >> 32, words[cursor + 1],
                        [symbolize(stack[0])] +
                        [symbolize(pc - 1) for pc in stack[1:]]))
        cursor += 2 + frames
    return samples


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("samples", help="PREFIX.PID.bin")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--focus", help="callers and callees of matching frames")
    parser.add_argument("--bucket", type=int, default=50,
                        help="timeline bucket in milliseconds")
    parser.add_argument("--timeline", action="store_true",
                        help="print every bucket's threads and phases")
    parser.add_argument("--width", type=int, default=150)
    args = parser.parse_args()
    samples = load(args.samples)
    total = len(samples)
    if not total:
        raise SystemExit("no samples")
    width = args.width
    first = min(sample[1] for sample in samples)
    span = (max(sample[1] for sample in samples) - first) / 1e9
    threads = collections.Counter(sample[0] for sample in samples)
    print(f"{total} samples over {span:.2f}s, {len(threads)} threads")

    leaf = collections.Counter(stack[0] for _, _, stack in samples)
    inclusive = collections.Counter()
    for _, _, stack in samples:
        inclusive.update(set(stack))
    print(f"\n== self ==")
    for name, count in leaf.most_common(args.top):
        print(f"{count / total:6.2%}  {name[:width]}")
    print(f"\n== inclusive ==")
    for name, count in inclusive.most_common(args.top * 2):
        print(f"{count / total:6.2%}  {name[:width]}")

    phases = collections.Counter()
    phase_threads = collections.defaultdict(set)
    buckets = collections.defaultdict(list)
    for thread, time, stack in samples:
        label = phase(stack)
        phases[label] += 1
        phase_threads[label].add(thread)
        buckets[(time - first) // (args.bucket * 1_000_000)].append((thread, label))
    print("\n== phases: CPU share, threads ==")
    for label, count in phases.most_common(args.top):
        print(f"{count / total:6.2%} {len(phase_threads[label]):4d}  {label}")

    serial = collections.Counter()
    for bucket in buckets.values():
        if len({thread for thread, _ in bucket}) <= SERIAL_THREADS:
            serial[collections.Counter(label for _, label in bucket)
                   .most_common(1)[0][0]] += 1
    print(f"\n== wall time with <= {SERIAL_THREADS} sampled threads ==")
    for label, count in serial.most_common(args.top):
        seconds = count * args.bucket / 1000
        print(f"{seconds:7.2f}s {seconds / span:6.1%}  {label}")

    if args.timeline:
        print(f"\n== timeline ({args.bucket} ms) ==")
        for index in sorted(buckets):
            bucket = buckets[index]
            labels = collections.Counter(label for _, label in bucket)
            top = ", ".join(f"{label} {count * 100 // len(bucket)}%"
                            for label, count in labels.most_common(3))
            print(f"{index * args.bucket / 1000:8.2f}s "
                  f"{len({thread for thread, _ in bucket}):3d}  {top[:width]}")

    if args.focus:
        pattern = re.compile(args.focus)
        callers, callees, hits = collections.Counter(), collections.Counter(), 0
        for _, _, stack in samples:
            index = next((i for i, name in enumerate(stack)
                          if pattern.search(name)), None)
            if index is None:
                continue
            hits += 1
            if index + 1 < len(stack):
                callers[stack[index + 1]] += 1
            callees[stack[index - 1] if index else "<self>"] += 1
        print(f"\n== /{args.focus}/: {hits / total:.2%} inclusive ==")
        for title, counter in (("callers", callers), ("callees", callees)):
            print(f"-- {title} --")
            for name, count in counter.most_common(args.top):
                print(f"{count / total:6.2%}  {name[:width]}")


if __name__ == "__main__":
    main()
