#!/usr/bin/env python3
"""Paired, warm-cache deep-link acknowledgements against a running IDA.

Run only against disposable, fully analyzed instances of the same input.
This measures replies and CLI process completion, not screen rendering.
Unix domain sockets are required; no desktop handler is installed.
"""
import argparse
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import socket
import statistics
import subprocess
import time
from urllib.parse import quote


def distribution(values):
    ordered = sorted(values)
    return {"samples": len(values), "median_ms": statistics.median(values),
            "p95_ms": ordered[math.ceil(len(values) * .95) - 1],
            "p99_ms": ordered[math.ceil(len(values) * .99) - 1],
            "min_ms": ordered[0], "max_ms": ordered[-1], "raw_ms": values}


def request(endpoint, payload):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(10)
        connection.connect(endpoint)
        connection.sendall(json.dumps(payload, separators=(",", ":")).encode())
        reply = bytearray()
        while len(reply) <= 1024 * 1024:
            part = connection.recv(65536)
            if not part:
                raise RuntimeError(f"{endpoint}: incomplete reply {reply!r}")
            reply.extend(part)
            if len(reply) > 1024 * 1024:
                raise RuntimeError(f"{endpoint}: excessive reply")
            try:
                result = json.loads(reply)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            if result.get("status") != "ok":
                raise RuntimeError(f"{endpoint}: {result!r}")
            return result
        raise RuntimeError(f"{endpoint}: excessive reply")


def paired(count, warmups, actions):
    samples = {name: [] for name in actions}
    names = list(actions)
    for index in range(count + warmups):
        for name in names if index % 2 == 0 else reversed(names):
            start = time.perf_counter_ns()
            actions[name](index)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            if index >= warmups:
                samples[name].append(elapsed)
    return {name: distribution(values) for name, values in samples.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", required=True)
    parser.add_argument("--hcli", required=True)
    parser.add_argument("--neverd-endpoint", required=True)
    parser.add_argument("--ida-endpoint", required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--address", type=lambda value: int(value, 0),
                        action="append", required=True)
    parser.add_argument("--copied-uri", required=True,
                        help="A real Copy link result from this IDA instance")
    parser.add_argument("--source-revision", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ipc-samples", type=int, default=500)
    parser.add_argument("--cli-samples", type=int, default=30)
    args = parser.parse_args()
    if args.ipc_samples < 1 or args.cli_samples < 1:
        parser.error("sample counts must be positive")
    endpoints = {"neverd": args.neverd_endpoint, "ida": args.ida_endpoint}
    info = {name: request(endpoint, {"cmd": "get_info"})
            for name, endpoint in endpoints.items()}
    for name, endpoint in endpoints.items():
        assert request(endpoint, {"cmd": "is_analysis_complete"})["analysis_complete"], name
    digest = hashlib.sha256(args.input.read_bytes()).hexdigest()
    assert info["neverd"]["sha256"] == digest, info["neverd"]
    assert Path(info["ida"]["idb_path"]).stem == args.input.name, info["ida"]
    report = {"schema": 1, "kind": "deep-link-acknowledgements",
              "host": platform.platform(), "source_revision": args.source_revision,
              "cpu_count": os.cpu_count(), "load_average_before": os.getloadavg(),
              "input_sha256": digest, "versions": {
                  "neverd": info["neverd"]["version"], "ida": info["ida"].get("version"),
                  "hcli": subprocess.check_output([args.hcli, "--version"], text=True).strip()},
              "binary_sha256": {name: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                                for name, path in (("neverd", args.neverd), ("hcli_entrypoint", args.hcli))},
              "copied_uri": args.copied_uri, "conditions": [
                  "Same input; both GUI instances fully analyzed, idle, on the same display.",
                  "Warm OS caches; alternating tool order; one new Unix socket per IPC sample.",
                  "Compact JSON without a trailing newline; validate every success reply.",
                  "IPC includes connect, send, navigation request and reply; no painted-frame timing.",
                  "CLI includes process startup and forwarding; defaults and --no-launch measured separately.",
                  "Other host activity is not controlled; load averages are recorded outside the timed samples.",
                  "NeverD get_info contains more metadata than IDA get_info."], "cases": {}}

    def record(name, count, warmups, actions):
        result = paired(count, warmups, actions)
        report["cases"][name] = result
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(name, json.dumps({tool: {key: value for key, value in data.items()
                                      if key != "raw_ms"} for tool, data in result.items()}), flush=True)

    for command in ("ping", "get_info"):
        record("ipc_" + command, args.ipc_samples, 20,
               {name: lambda index, endpoint=endpoint: request(endpoint, {"cmd": command})
                for name, endpoint in endpoints.items()})
    for name in ("addresses", "copied_functions", "alternating_functions"):
        def payload(tool, index):
            resource = "functions" if name == "alternating_functions" else "addresses"
            uri = args.copied_uri if name == "copied_functions" else (
                f"ida:///{resource}?ea={hex(args.address[index % len(args.address)])}&view=disasm")
            return {"cmd": "open_ida_link" if tool == "ida" else "open_link", "uri": uri}
        record("ipc_open_" + name, args.ipc_samples, 20,
               {tool: lambda index, tool=tool: request(endpoints[tool], payload(tool, index))
                for tool in endpoints})

    def cli(command):
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, timeout=20)

    # The common named URI matches both input-name aliases and IDB basenames.
    for no_launch in (False, True):
        def command(tool, index):
            uri = (f"ida:///{quote(args.input.name + '.i64', safe='')}/addresses?"
                   f"ea={hex(args.address[index % len(args.address)])}&view=disasm")
            return ([args.neverd, *(["--no-launch"] if no_launch else []), "--link", uri]
                    if tool == "neverd" else
                    [args.hcli, "ida", "open", *(["--no-launch"] if no_launch else []), uri])
        record("cli_no_launch" if no_launch else "cli_default", args.cli_samples, 3,
               {tool: lambda index, tool=tool: cli(command(tool, index)) for tool in endpoints})
    final = request(args.neverd_endpoint, {"cmd": "get_info"})
    assert int(final["location"]["address"], 16) == args.address[(args.cli_samples + 2) % len(args.address)], final
    report["load_average_after"] = os.getloadavg()
    args.output.write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
