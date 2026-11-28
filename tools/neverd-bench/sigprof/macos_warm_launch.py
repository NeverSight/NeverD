#!/usr/bin/env python3
"""Compare warm HighC launches with kernel exit times and a private main-entry probe."""

import argparse
from collections import deque
from contextlib import contextmanager
from functools import partial
import ctypes
import errno
import itertools
import json
import os
from pathlib import Path
import platform
import random
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

from macos_sdk_load import digest
from macos_warm_report import QUIET_BACKGROUND_MAX


def cli_link(build):
    graph = subprocess.check_output(
        ["ninja", "-C", build, "-t", "commands", "bin/neverd"], text=True
    )
    tokens = shlex.split(graph.splitlines()[-1])
    for _, group in itertools.groupby(tokens, lambda token: token == "&&"):
        args = list(group)
        if "-o" in args and args[args.index("-o") + 1] == "bin/neverd":
            return args
    raise ValueError("could not find the CLI link command in the Ninja build")


def prepare(build, scratch, counters=False):
    source = Path(__file__).with_name("macos-warm-launch.c").resolve()
    helper = scratch / "clock.dylib"
    entry = scratch / "entry.o"
    marked = scratch / "marked-neverd"
    cc = ["xcrun", "clang", "-O2", "-Wall", "-Wextra", "-Werror"]
    probe = source.with_name("macos-premain.c") if counters else source
    clock_sources = [source, probe] if counters else [source]
    subprocess.run([*cc, "-dynamiclib", *clock_sources, "-o", helper], check=True)
    link = cli_link(build)
    archs = [part for i, token in enumerate(link) if token == "-arch" for part in link[i:i + 2]]
    kind = "premain" if counters else "warm"
    subprocess.run([*cc, *archs, "-DNEVERD_" + kind.upper() + "_ENTRY",
                    "-c", probe, "-o", entry], check=True)
    link[link.index("-o") + 1] = str(marked)
    subprocess.run([*link, entry, f"-Wl,-e,_neverd_{kind}_entry"], cwd=build, check=True)
    return Clock(helper), marked


class Clock:
    def __init__(self, path):
        self.lib = ctypes.CDLL(str(path), use_errno=True)
        self.lib.warm_clock.restype = ctypes.c_uint64
        self.lib.warm_ms_per_tick.restype = ctypes.c_double
        self.lib.warm_host_ticks.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        self.lib.warm_wait_exit.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint64)]
        self.ms = self.lib.warm_ms_per_tick()

    def ticks(self):
        values = (ctypes.c_uint32 * 4)()
        if self.lib.warm_host_ticks(values):
            raise OSError("host_statistics failed")
        return tuple(values)

    def exit_record(self, pid):
        values = (ctypes.c_uint64 * 5)()
        if self.lib.warm_wait_exit(pid, values):
            raise OSError(ctypes.get_errno(), "could not read the child's exit record")
        return tuple(values)


def busy(start, end):
    delta = [(b - a) % 2**32 for a, b in zip(start, end)]
    return 1 - delta[2] / sum(delta)


def ambient(clock, seconds=1):
    start = clock.ticks()
    time.sleep(seconds)
    return busy(start, clock.ticks())


def settled_ambient(clock, max_wait):
    deadline = time.monotonic() + max_wait
    seconds = min(3, max_wait)
    windows = deque(maxlen=3)
    start = clock.ticks()
    for _ in range(max_wait // seconds):
        time.sleep(seconds)
        end = clock.ticks()
        windows.append((start, busy(start, end)))
        samples = [value for _, value in windows]
        settled = seconds == 3 and len(samples) == 3 and max(samples) <= QUIET_BACKGROUND_MAX
        if settled or time.monotonic() >= deadline:
            break
        start = end
    return busy(windows[0][0], end), samples, settled


def launch(clock, args, env, errors, packet_size, sample_entry=False):
    if sample_entry:
        parent, child = socket.socketpair()
        read_fd, write_fd = parent.detach(), child.detach()
    else:
        read_fd, write_fd = os.pipe()
    actions = [(os.POSIX_SPAWN_OPEN, 1, os.devnull, os.O_WRONLY, 0),
               (os.POSIX_SPAWN_OPEN, 2, str(errors), os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o600),
               (os.POSIX_SPAWN_DUP2, write_fd, 3)]
    with os.fdopen(read_fd, "r+b" if sample_entry else "rb", buffering=0) as pipe:
        start = clock.lib.warm_clock()
        try:
            pid = os.posix_spawn(str(args[0]), args, env, file_actions=actions)
        finally:
            os.close(write_fd)
        try:
            sample = None
            if sample_entry:
                if not select.select([pipe], [], [], 120)[0]:
                    raise TimeoutError("CLI did not reach its main-entry probe")
                if pipe.read(1) != b"+":
                    raise ChildProcessError("CLI exited before its main-entry probe")
                sample = snapshot(clock, pid)
                pipe.write(b"+")
            packet = bytearray()
            while True:
                if not select.select([pipe], [], [], 120)[0]:
                    raise TimeoutError("timed CLI did not close its marker pipe")
                chunk = pipe.read(32)
                if not chunk:
                    break
                packet += chunk
                if len(packet) > packet_size:
                    raise ValueError("unexpected entry-probe packet")
            birth, exit_at, cpu, instructions, observed = clock.exit_record(pid)
        except BaseException:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            raise
        finally:
            _, status, _ = os.wait4(pid, 0)
    if status:
        detail = errors.read_text(errors="replace")[-1000:]
        raise ChildProcessError(f"CLI wait status {status}: {detail}")
    if not start <= birth <= exit_at <= observed or len(packet) != packet_size:
        raise ValueError("invalid child timing or marker count")
    return (start, birth, exit_at, cpu, instructions, observed), packet, sample


def observe(clock, args, env, marked, errors, *, entry_counters=False):
    if marked and entry_counters:
        return observe_entry(clock, args, env, errors)
    timing, packet, _ = launch(clock, args, env, errors, 16 if marked else 0)
    start, birth, exit_at, cpu, instructions, observed = timing
    ms = clock.ms
    row = {"wall_ms": (observed - start) * ms, "spawn_to_birth_ms": (birth - start) * ms,
           "lifetime_ms": (exit_at - birth) * ms, "exit_to_observer_ms": (observed - exit_at) * ms,
           "cpu_ms": cpu * ms, "instructions": instructions}
    if marked:
        entry, leave = struct.unpack("=QQ", packet)
        if not birth <= entry <= leave <= exit_at:
            raise ValueError("entry markers outside kernel process lifetime")
        row.update(pre_main_ms=(entry - start) * ms, main_ms=(leave - entry) * ms,
                   teardown_ms=(exit_at - leave) * ms)
    return row


def snapshot(clock, pid):
    values = (ctypes.c_uint64 * 10)()
    if clock.lib.premain_snapshot(pid, values):
        error = ctypes.get_errno()
        if error == errno.ENOTSUP:
            raise ValueError("initializer threads exited before the entry sample")
        raise OSError(error, "could not sample child at main entry")
    return values


def observe_entry(clock, args, env, errors):
    timing, packet, counters = launch(clock, args, env, errors, 48, sample_entry=True)
    start, birth, exit_at, _, _, observed = timing
    entry, entry_cpu, entry_runnable, gate, resumed, leave = struct.unpack("=6Q", packet)
    seen, kernel_birth, cpu, runnable, ins, pageins, reads, faults, cow, threads = counters
    if not (kernel_birth == birth and start <= birth <= entry <= gate <= seen <= resumed
            <= leave <= exit_at <= observed):
        raise ValueError("entry sample does not match the child's lifetime")
    if threads != 1:
        raise ValueError(f"process had {threads} threads at entry; cannot partition wall time")

    # The final runnable interval closes when the child blocks. Bound a late
    # observer with the live counters and both edges of the gate.
    ms = clock.ms
    return {"birth_to_main_ms": (entry - birth) * ms, "pre_main_ms": (entry - start) * ms,
            "spawn_to_birth_ms": (birth - start) * ms, "main_ms": (leave - resumed) * ms,
            "cpu_at_entry_ms": entry_cpu * ms, "runnable_at_entry_ms": entry_runnable * ms,
            "cpu_at_stop_ms": cpu * ms, "runnable_at_stop_ms": runnable * ms,
            "probe_to_gate_ms": (gate - entry) * ms, "gate_to_observer_ms": (seen - gate) * ms,
            "observed_pause_ms": (resumed - entry) * ms, "entry_instructions": ins,
            "pageins": pageins, "bytes_read": reads, "faults": faults, "cow_faults": cow}



@contextmanager
def pressure(count):
    children = []
    try:
        for _ in range(count):
            children.append(subprocess.Popen([sys.executable, "-c", "while True: pass"]))
        yield
    finally:
        for child in children:
            child.terminate()
        for child in children:
            child.wait()


class WarmPath:
    def __init__(self, root):
        self.root = root
        self.live = root / "live"
        self.current = None

    def select(self, side):
        if self.current == side:
            return
        if self.current is not None:
            self.live.rename(self.root / self.current)
        if side is not None:
            (self.root / side).rename(self.live)
        self.current = side


def identity(path):
    st = path.stat()
    return st.st_dev, st.st_ino, st.st_ctime_ns, st.st_nlink


def measure(args, scratch, output):
    counters = getattr(args, "entry_counters", False)
    clock, marked = prepare(args.build, scratch, counters)
    run = partial(observe, entry_counters=counters)
    cli = args.build / "bin/neverd"
    sdk_name = "libneverd.dylib"
    sides = {"A": args.baseline_sdk, "B": args.candidate_sdk,
             "A1": args.baseline_sdk, "A2": args.baseline_sdk}
    machines = {"stock": cli, "marked": marked}
    switch = WarmPath(scratch)
    common = switch.live / sdk_name
    data = scratch / "out.c"
    errors = scratch / "stderr"
    options = ["decompile", "--func=" + args.function, str(args.image), "-o", str(data)]
    env = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    env.update(NEVERD_THREADS="1", NEVERD_SIGNATURE_CACHE="off", DYLD_LIBRARY_PATH=str(switch.live))
    files, hashes, identities = {}, {}, {}
    gold = None
    for side, original in sides.items():
        folder = scratch / side
        folder.mkdir()
        path = folder / sdk_name
        shutil.copyfile(original, path)
        if digest(original) != digest(path):
            raise ValueError("SDK copy differs from its source")
        subprocess.run(["codesign", "--verify", "--strict", path], check=True)
        files[side], hashes[side] = path, digest(path)
        identities[side] = identity(path)
        switch.select(side)
        for route, exe in machines.items():
            # The private entry writes to fd 3, which observe supplies. Check
            # dyld identity on the same executable before collecting samples.
            data.unlink(missing_ok=True)
            run(clock, [str(exe), *options], {**env, "DYLD_PRINT_LIBRARIES": "1"},
                    route == "marked", errors)
            images = [Path(line.split("> ", 1)[1]) for line in errors.read_text().splitlines()
                      if "> " in line and line.endswith("/" + sdk_name)]
            if len(images) != 1 or identity(images[0]) != identities[side]:
                raise ValueError(f"{route} loaded a different SDK")
            result = digest(data)
            if gold is not None and result != gold:
                raise ValueError("preflight HighC output differs")
            gold = result
    def write(event):
        output.write(json.dumps(event) + "\n")
        output.flush()
    write({"event": "setup", "sdk_sha256": hashes, "cli_sha256": digest(cli),
           "marked_sha256": digest(marked), "input_sha256": digest(args.image),
           "output_sha256": gold, "function": args.function, "cpus": os.cpu_count(),
           "macos": platform.mac_ver()[0], "arch": platform.machine(),
           "entry_counters": counters, "controlled_load": args.control_load, "quiet_wait": args.quiet_wait,
           "rounds": args.rounds, "pairs": args.pairs})
    for round_id in range(args.rounds):
        conditions = ["quiet", "busy"] if args.control_load else ["ambient"]
        if round_id % 2:
            conditions.reverse()
        for condition in conditions:
            count = 2 * os.cpu_count() if condition == "busy" else 0
            with pressure(count):
                time.sleep(1)
                settle = {}
                if args.quiet_wait and condition != "busy":
                    left, samples, settled = settled_ambient(clock, args.quiet_wait)
                    settle = {"quiet_background_samples": samples, "quiet_settled": settled}
                else:
                    left = ambient(clock)
                start_ticks = clock.ticks()
                rng = random.Random(8881 + round_id)
                for pair in range(args.pairs):
                    cases = [(kind, other) for kind in machines for other in
                             (["B", "A2"] if pair % 2 == 0 else ["B"])]
                    rng.shuffle(cases)
                    for kind, other in cases:
                        index = pair if other == "B" else pair // 2
                        # Sharing the primary A inode with A/A gives it more warm launches than B.
                        order = ["A1" if other == "A2" else "A", other]
                        if (index + round_id + (kind == "marked") + (condition == "busy")) % 2:
                            order.reverse()
                        for side in order:
                            reused = switch.current == side
                            switch.select(side)
                            if identity(common) != identities[side]:
                                raise ValueError("warm SDK identity or metadata changed")
                            data.unlink(missing_ok=True)
                            row = run(clock, [str(machines[kind]), *options], env,
                                          kind == "marked", errors)
                            if digest(data) != gold:
                                raise ValueError("timed HighC output differs")
                            write({"event": "sample", "round": round_id, "condition": condition,
                                   "pair": pair, "kind": kind, "other": other, "side": side,
                                   "reused_sdk": reused, **row})
                during = busy(start_ticks, clock.ticks())
                right = ambient(clock, 7 if settle else 1)
                write({"event": "load", "round": round_id, "condition": condition,
                       "workers": count, "before": left, "during": during, "after": right,
                       "loadavg": os.getloadavg(), **settle})
    switch.select(None)
    for side, path in files.items():
        if digest(path) != hashes[side]:
            raise ValueError("SDK changed during measurement")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ["build", "baseline-sdk", "candidate-sdk", "image", "jsonl"]:
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--function", required=True)
    parser.add_argument("--rounds", type=int, default=6)
    parser.add_argument("--pairs", type=int, default=64)
    parser.add_argument("--entry-counters", action="store_true",
                        help="sample the blocked initial thread just after main entry")
    parser.add_argument("--control-load", action="store_true",
                        help="alternate ambient runs with two CPU workers per logical CPU")
    parser.add_argument("--quiet-wait", type=int, default=0, metavar="SECONDS",
                        help="wait for three quiet three-second windows before an unpressured round")
    args = parser.parse_args()
    if min(args.rounds, args.pairs) < 1:
        parser.error("rounds and pairs must be positive")
    if args.quiet_wait < 0:
        parser.error("quiet wait must be nonnegative")
    args.build, args.image = args.build.resolve(), args.image.resolve()
    with args.jsonl.open("x") as output, tempfile.TemporaryDirectory(dir=args.jsonl.parent) as tmp:
        measure(args, Path(tmp).resolve(), output)


if __name__ == "__main__":
    main()
