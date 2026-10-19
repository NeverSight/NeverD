#!/usr/bin/env python3
"""Replay bounded driver calls through the public NeverD SDK, without source edits.

Requires Python 3.10+ and a built NeverD shared library. All image, PDB and
scenario inputs are explicit; the runner never searches the host system.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pluginsdk/python"))
from neverd_plugin.abi import NeverDExecutionOptionsV1
from neverd_plugin.ffi import HostAPI


def number(text: str) -> int:
    return int(text, 0)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(data)
    return result.hexdigest()


def save(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


class SDK:
    def __init__(self, library: Path):
        self.directory = os.add_dll_directory(str(library.parent)) if sys.platform == "win32" else None
        self.api = HostAPI(ctypes.CDLL(str(library)))

    def take(self, pointer: int) -> str:
        try:
            return ctypes.string_at(pointer).decode("utf-8")
        finally:
            self.api.function("neverd_free_string")(ctypes.cast(pointer, ctypes.c_char_p))

    def call(self, name: str, *arguments: object) -> int:
        error = ctypes.c_void_p()
        result = self.api.function(name)(*arguments, ctypes.byref(error))
        message = self.take(error.value) if error.value else None
        if not result or message:
            raise RuntimeError(f"{name}: {message or 'operation failed'}")
        return result

    def report(self, name: str, *arguments: object) -> dict:
        return json.loads(self.take(self.call(name, *arguments)))

    def command(self, run: int, operation: str, **fields: object) -> dict:
        text = json.dumps({"id": "replay", "op": operation, **fields}).encode("utf-8")
        response = self.report("neverd_execution_command_json", run, text)
        if not response.get("ok"):
            raise RuntimeError(f"{operation}: {response}")
        return response["result"]

    def close(self) -> None:
        if self.directory:
            self.directory.close()


def replay(args: argparse.Namespace) -> int:
    library, image, scenario_file = (p.resolve(strict=True) for p in (args.library, args.image, args.scenario))
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    scenario = json.loads(scenario_file.read_text(encoding="utf-8-sig"))
    if not isinstance(scenario, dict):
        raise ValueError("scenario must be a JSON object")
    if "kernel_image" in scenario or "kernel_pdb" in scenario:
        raise ValueError("register kernel files in --modules and select --kernel-module; remove legacy kernel_image/kernel_pdb")
    capacities = args.capacity if args.capacity is not None else [0, 2, 18, 260, 1024]
    if not capacities or any(n < 0 for n in capacities) or not 0 <= args.fill <= 255:
        raise ValueError("capacities must be nonnegative and fill must fit one byte")
    if not 1 <= args.buffer_bytes <= 1048576 or not 1 <= args.report_tail <= 1000000:
        raise ValueError("buffer bytes/report tail exceeds its positive bound")
    environment = {"kind": "windows-driver", "image_module": args.image_id,
                   "base_directory": str(scenario_file.parent), "backend": "unicorn",
                   "contract": args.contract, "scenario": scenario,
                   "primary_import_policy": args.primary_import_policy,
                   "report": {"call_tail": args.report_tail, "write_tail": args.report_tail}}
    if args.kernel_module:
        environment["kernel_module"] = args.kernel_module
    configuration = {"environment": environment, "run": {
        "pause_stages": ["after-entry"], "instruction_quantum": 100000,
        "enable_resource_pauses": True, "limits": {"instructions": args.instructions,
        "events": args.events, "timeout_microseconds": int(args.timeout_seconds * 1000000)}}}
    api, source, run = SDK(library), 0, 0
    result = {"schema_version": 1, "interface": "public NeverD execution C API",
              "library": str(library), "library_sha256": digest(library),
              "image": str(image), "image_sha256": digest(image),
              "scenario": str(scenario_file), "scenario_sha256": digest(scenario_file),
              "configuration": configuration, "calls": [], "completed": False}
    started = time.monotonic()
    try:
        if args.modules:
            modules = args.modules.resolve(strict=True)
            result["modules_file"] = str(modules)
            result["modules_sha256"] = digest(modules)
            source = api.call("neverd_module_set_from_file", str(modules).encode("utf-8"))
        else:
            source = api.call("neverd_module_set_create_v1", None)
        declaration = {"id": args.image_id, "image": str(image), "load": "lazy"}
        if args.pdb:
            declaration["debug"] = {"kind": "pdb", "path": str(args.pdb.resolve(strict=True))}
        api.call("neverd_module_set_add_json", source, json.dumps(declaration).encode("utf-8"))
        text = json.dumps(configuration).encode("utf-8")
        options = NeverDExecutionOptionsV1(ctypes.sizeof(NeverDExecutionOptionsV1), source, text)
        run = api.call("neverd_execution_create_v1", ctypes.byref(options))
        result["sources"] = api.report("neverd_module_set_inspect_json", source)
        api.call("neverd_module_set_destroy", source)
        source = 0
        result["source_handle_released_before_execution"] = True

        def advance() -> dict:
            while True:
                event = api.command(run, "run")
                if event.get("reason") != "quantum":
                    return event
                api.command(run, "resume")

        print(json.dumps({"phase": "driver_entry", "image_sha256": result["image_sha256"]}), flush=True)
        event = advance()
        result["entry_event"] = event
        entry = api.command(run, "inspect")
        result["entry_snapshot"] = entry
        if event.get("reason") != "stage" or event.get("stage") != "after-entry" or entry["environment_report"].get("nt_status") != 0:
            raise RuntimeError("driver did not reach the successful after-entry boundary")
        print(json.dumps({"phase": "after-entry", "instructions": entry["instructions"],
                          "nt_status": entry["environment_report"]["nt_status"]}), flush=True)
        previous = entry["environment_report"]["trace_view"]["total_calls"]
        for capacity in capacities:
            request = {"entry": hex(args.entry), "abi": "win64", "arguments": [
                {"buffer": 0}, {"value": capacity}], "buffers": [{"size": args.buffer_bytes,
                "input_hex": (bytes([args.fill]) * args.buffer_bytes).hex(), "capture": True}]}
            api.command(run, "invoke", invocation=request)
            event = advance()
            row = {"capacity": capacity, "event": event}
            result["calls"].append(row)
            observation = api.command(run, "inspect")
            report = observation["environment_report"]
            trace = report["trace_view"]
            row["api_count"] = trace["total_calls"] - previous
            row["api_calls_complete"] = trace["first_call_index"] <= previous
            row["api_calls"] = report["calls"][max(0, previous - trace["first_call_index"]):]
            previous = trace["total_calls"]
            if event.get("reason") != "invocation_complete":
                result["failure_snapshot"] = observation
                raise RuntimeError(f"invocation at capacity {capacity} did not complete")
            captured = event["invocation"]["buffers"][0]["bytes_hex"]
            row["output_modified"] = bytes.fromhex(captured) != bytes([args.fill]) * args.buffer_bytes
            print(json.dumps({"phase": "invocation_complete", "capacity": capacity,
                              "value": event["invocation"]["value"], "api_count": row["api_count"],
                              "instructions": event["invocation"]["instructions"]}), flush=True)
            save(output, result)
        result["final_snapshot"] = api.command(run, "inspect")
        result["instances"] = api.command(run, "instances")
        result["completed"] = True
    except Exception as error:
        result["error"] = str(error)
        print(str(error), file=sys.stderr, flush=True)
    finally:
        result["elapsed_seconds"] = time.monotonic() - started
        result["image_unchanged"] = digest(image) == result["image_sha256"]
        save(output, result)
        if run:
            api.call("neverd_execution_destroy", run)
        if source:
            api.call("neverd_module_set_destroy", source)
        api.close()
    return 0 if result["completed"] and result["image_unchanged"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("library", "image", "scenario", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--modules", type=Path)
    parser.add_argument("--kernel-module")
    parser.add_argument("--image-id", default="replay-driver")
    parser.add_argument("--pdb", type=Path)
    parser.add_argument("--entry", type=number, required=True)
    parser.add_argument("--capacity", type=number, action="append")
    parser.add_argument("--buffer-bytes", type=number, default=4096)
    parser.add_argument("--fill", type=number, default=0xa5)
    parser.add_argument("--report-tail", type=number, default=64)
    parser.add_argument("--contract", choices=("driver-strict", "checked-x64-v1"), default="driver-strict")
    parser.add_argument("--primary-import-policy", choices=("modules", "model-traps"), default="modules",
                        help="modules resolves primary imports through bindings; model-traps retains one-shot model/trap binding")
    parser.add_argument("--instructions", type=number, default=300000000)
    parser.add_argument("--events", type=number, default=12000000)
    parser.add_argument("--timeout-seconds", type=float, default=600)
    return replay(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
