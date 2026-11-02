#!/usr/bin/env python3
"""Audit IDA pseudocode after direct and ordered-prefix decompilation.

Each observation uses a fresh process and a private copy of the binary.
Exit 0 means identical observed text and provenance, 2 means a change that
needs review, and 1 means the audit could not be completed. Neither outcome
proves semantic equivalence or licenses a cross-decompiler speed claim.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def address(value: str) -> int:
    parsed = int(value, 0)
    if not 0 <= parsed < 2**64:
        raise ValueError(f"Address outside the 64-bit range: {value}")
    return parsed


def preceding_entries(path: Path, target: int) -> list[int]:
    entries = [address(word) for word in path.read_text().split()]
    if len(entries) != len(set(entries)):
        raise ValueError("The entry order contains duplicates")
    if target not in entries:
        raise ValueError("The target is absent from the entry order")
    return entries[:entries.index(target)]


def write_json(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def collect(binary: Path, target: int, preceding: list[int],
            output: Path, expected_version: str | None) -> None:
    # Imports stay inside the child: the driver needs only standard Python.
    import idapro
    import ida_auto
    import ida_bytes
    import ida_funcs
    import ida_gdl
    import ida_hexrays
    import ida_idaapi
    import ida_kernwin
    import idautils

    engine = {
        "kernel": ida_kernwin.get_kernel_version(),
        "library": list(idapro.get_library_version()),
        "library_sha256": digest(Path(idapro.idalib_path)),
        "library_path": str(idapro.idalib_path),
    }
    if expected_version and engine["kernel"] != expected_version:
        raise ValueError(f"Expected IDA {expected_version}, got {engine['kernel']}")

    def inventory():
        function = ida_funcs.get_func(target)
        if function is None or function.start_ea != target:
            raise ValueError(f"{target:#x} is not a recognized function entry")
        instructions = []
        for ea in idautils.FuncItems(target):
            if not ida_bytes.is_code(ida_bytes.get_flags(ea)):
                continue
            size = ida_bytes.get_item_size(ea)
            data = ida_bytes.get_bytes(ea, size)
            if not data or len(data) != size:
                raise ValueError(f"Cannot read instruction at {ea:#x}")
            instructions.append({"address": hex(ea), "bytes": data.hex()})
        if not instructions:
            raise ValueError("The target has no decoded instructions")
        return {
            "chunks": [[hex(start), hex(end)]
                       for start, end in idautils.Chunks(target)],
            "instructions": instructions,
            "blocks": [
                {"start": hex(block.start_ea), "end": hex(block.end_ea),
                 "successors": [hex(s.start_ea) for s in block.succs()]}
                for block in ida_gdl.FlowChart(function)
            ],
        }

    def decompile(entry, filename):
        function = ida_funcs.get_func(entry)
        if function is None or function.start_ea != entry:
            raise ValueError(f"{entry:#x} is not a recognized function entry")
        cfunc = ida_hexrays.decompile(entry)
        if cfunc is None:
            raise ValueError(f"Decompilation failed at {entry:#x}")
        data = str(cfunc).encode("utf-8")
        if not data.strip():
            raise ValueError(f"Empty pseudocode at {entry:#x}")
        (output / filename).write_bytes(data)
        return cfunc, {
            "entry": hex(entry), "file": filename, "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "type": str(cfunc.type),
        }

    with tempfile.TemporaryDirectory(prefix="database-", dir=output) as temporary:
        private_binary = Path(temporary) / binary.name
        shutil.copyfile(binary, private_binary)
        input_hash = digest(private_binary)
        if idapro.open_database(str(private_binary), True) != 0:
            raise RuntimeError("IDA could not open the private binary")
        try:
            ida_auto.auto_wait()
            if not ida_hexrays.init_hexrays_plugin():
                raise RuntimeError("Hex-Rays is unavailable")
            engine["hexrays"] = ida_hexrays.get_hexrays_version()
            before = inventory()
            previous = []
            for entry in preceding:
                _, record = decompile(entry, f"preceding-{entry:x}.c")
                previous.append(record)
            cfunc, source = decompile(target, "target.c")

            class Provenance(ida_hexrays.ctree_visitor_t):
                def __init__(self):
                    super().__init__(ida_hexrays.CV_FAST)
                    self.addresses = set()

                def visit_insn(self, instruction):
                    if instruction.ea != ida_idaapi.BADADDR:
                        self.addresses.add(instruction.ea)
                    return 0

                def visit_expr(self, expression):
                    if expression.ea != ida_idaapi.BADADDR:
                        self.addresses.add(expression.ea)
                    return 0

            visitor = Provenance()
            visitor.apply_to(cfunc.body, None)
            write_json(output / "observation.json", {
                "status": "ok", "input_sha256": input_hash,
                "target": hex(target), "engine": engine,
                "preceding": previous, "source": source,
                "native_before": before, "native_after": inventory(),
                "ctree_addresses": [hex(ea) for ea in sorted(visitor.addresses)],
            })
        finally:
            idapro.close_database(False)


def checked_observation(directory: Path, input_hash: str, target: int,
                        preceding: list[int]) -> dict:
    observation = json.loads((directory / "observation.json").read_text())
    if (observation["status"] != "ok"
            or observation["input_sha256"] != input_hash
            or observation["target"] != hex(target)
            or [row["entry"] for row in observation["preceding"]]
            != [hex(entry) for entry in preceding]):
        raise ValueError("Observation does not match the requested input and order")
    records = [*observation["preceding"], observation["source"]]
    if observation["source"]["entry"] != hex(target):
        raise ValueError("Source record does not name the target")
    for record in records:
        path = directory / record["file"]
        if path.parent != directory or not path.is_file():
            raise ValueError("Source artifact must be inside its observation directory")
        if (not record["bytes"] or path.stat().st_size != record["bytes"]
                or digest(path) != record["sha256"]):
            raise ValueError("Missing, empty or changed source artifact")
    return observation


def compare(direct: dict, prefix: dict) -> dict:
    for key in ("input_sha256", "target", "engine"):
        if direct[key] != prefix[key]:
            raise ValueError(f"Cannot compare different {key}")
    a = {address(ea) for ea in direct["ctree_addresses"]}
    b = {address(ea) for ea in prefix["ctree_addresses"]}
    text_changed = direct["source"]["sha256"] != prefix["source"]["sha256"]
    inventory_changed = direct["native_after"] != prefix["native_after"]
    changed = text_changed or a != b or inventory_changed
    return {
        "status": "changed" if changed else "stable",
        "source_text_changed": text_changed,
        "native_inventory_changed_between_orders": inventory_changed,
        "native_inventory_changed_within_observation": {
            name: item["native_before"] != item["native_after"]
            for name, item in (("direct", direct), ("prefix", prefix))
        },
        "source_bytes": {"direct": direct["source"]["bytes"],
                         "prefix": prefix["source"]["bytes"]},
        "ctree_address_counts": {"direct": len(a), "prefix": len(b)},
        "ctree_addresses_removed": [hex(ea) for ea in sorted(a - b)],
        "ctree_addresses_added": [hex(ea) for ea in sorted(b - a)],
        "interpretation": (
            "Text or provenance changes require review before reusing timings. "
            "CTree addresses are provenance, not instruction coverage. "
            "Stable observations do not prove semantic equivalence."
        ),
    }


def main() -> int:
    class Parser(argparse.ArgumentParser):
        def error(self, message):
            # Exit 2 is reserved for a completed audit with changed output.
            raise ValueError(message)

    parser = Parser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--target", type=address, required=True)
    parser.add_argument("--entries-file", type=Path, required=True,
                        help="Whitespace-separated addresses in actual visitation order")
    parser.add_argument("--output", type=Path, required=True,
                        help="New evidence directory; existing directories are refused")
    parser.add_argument("--ida-python", default=sys.executable)
    parser.add_argument("--ida-dir", type=Path)
    parser.add_argument("--expected-version")
    parser.add_argument("--timeout", type=float, default=1800)
    parser.add_argument("--collect", choices=("direct", "prefix"),
                        help=argparse.SUPPRESS)
    try:
        args = parser.parse_args()
        output = args.output.resolve()
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError("Timeout must be finite and positive")
        binary = args.binary.resolve(strict=True)
        preceding = preceding_entries(args.entries_file, args.target)
        if args.collect:
            collect(binary, args.target,
                    preceding if args.collect == "prefix" else [],
                    output, args.expected_version)
            return 0
        output.mkdir(parents=True, exist_ok=False)
        input_hash = digest(binary)
        write_json(output / "input.json", {
            "binary": str(binary), "sha256": input_hash,
            "target": hex(args.target),
            "preceding": [hex(entry) for entry in preceding],
            "script_sha256": digest(Path(__file__)),
        })
        environment = os.environ.copy()
        if args.ida_dir:
            installation = args.ida_dir.resolve(strict=True)
            environment["IDADIR"] = str(installation)
            environment["PYTHONPATH"] = os.pathsep.join(filter(None, (
                str(installation / "idalib/python"),
                environment.get("PYTHONPATH"),
            )))
        observations = {}
        for case in ("direct", "prefix"):
            directory = output / case
            directory.mkdir()
            command = [
                args.ida_python, str(Path(__file__).resolve()), str(binary),
                "--target", hex(args.target), "--entries-file",
                str(args.entries_file.resolve()), "--output", str(directory),
                "--collect", case,
            ]
            if args.expected_version:
                command.extend(("--expected-version", args.expected_version))
            with (directory / "process.log").open("w") as log:
                subprocess.run(command, env=environment, stdout=log,
                               stderr=subprocess.STDOUT, check=True,
                               timeout=args.timeout)
            observations[case] = checked_observation(
                directory, input_hash, args.target,
                preceding if case == "prefix" else [])
        assessment = compare(observations["direct"], observations["prefix"])
        write_json(output / "assessment.json", assessment)
        print(json.dumps(assessment, indent=2))
        return 2 if assessment["status"] == "changed" else 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        # Preserve partial evidence but never reuse an older assessment.
        print(f"Source-order audit failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
