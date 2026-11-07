#!/usr/bin/env python3
"""Record finite arithmetic on each native host and audit paired x64/ARM64 artifacts.

Only authored protocol inputs are executed. Native x64 x87 is the separately
identified numerical reference; generated whole-state observations are compared
across hosts without promoting undefined CPU state to an architectural claim.
"""

import argparse
import json
from pathlib import Path
import platform
import shutil
import subprocess

from check_x87_arithmetic_replay import inputs, run_driver
from check_x87_native_regression import git, linked_libraries, sha256, source_snapshot
from check_x87_portable_generated import audit_observations

SEEDS = (0x871677, 0x871678, 0x871679)
PROTOCOL = "neverd-x87-cross-host-arithmetic-v1"


def file_manifest(directory):
    return {str(path.relative_to(directory)): {"bytes": path.stat().st_size, "sha256": sha256(path)}
            for path in sorted(directory.rglob("*")) if path.is_file()}


def read_verified(directory):
    directory = directory.resolve(strict=True)
    record = json.loads((directory / "record.json").read_text())
    actual = {str(path.relative_to(directory)) for path in directory.rglob("*")
              if path.is_file() and path != directory / "record.json"}
    if actual != set(record["files"]):
        raise ValueError("unbound or missing evidence file")
    for name, expected in record["files"].items():
        path = (directory / name).resolve(strict=True)
        if (not path.is_relative_to(directory) or path.stat().st_size != expected["bytes"] or
                sha256(path) != expected["sha256"]):
            raise ValueError("changed or out-of-tree evidence file: " + name)
    return record


def record(args):
    machine = platform.machine()
    if platform.system() != "Linux" or machine not in ("x86_64", "aarch64"):
        raise ValueError("native Linux x64 or AArch64 is required")
    if (machine == "x86_64") != bool(args.original_driver):
        raise ValueError("only x64 must supply the original x87 driver")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {"protocol": PROTOCOL, "execution_completed": False,
              "scope": "masked finite numerical bits, new IE/DE/ZE/OE/UE/PE and C1",
              "comparison_pending": True, "machine": machine, "seeds": list(SEEDS)}
    try:
        result["cpu"] = json.loads(subprocess.check_output(["lscpu", "--json"]))
        result["git_head"] = git("rev-parse", "HEAD").decode().strip()
        result["submodules"] = git("submodule", "status").decode().splitlines()
        snapshot = source_snapshot()
        if snapshot != (b"", {}):
            raise ValueError("cross-host qualification requires a clean committed source tree")
        build = args.build_dir.resolve(strict=True)
        if json.loads((build / "neverd-build-receipt.json").read_text())["status"] != "success":
            raise ValueError("normal build wrapper has no successful receipt")
        bindings = {}
        drivers = {"portable": args.portable_driver.resolve(strict=True)}
        if args.original_driver:
            drivers["original"] = args.original_driver.resolve(strict=True)
        for name, driver in drivers.items():
            bindings[str(driver)] = sha256(driver)
            bindings.update(linked_libraries(driver))
            shutil.copy2(driver, output / (name + "-driver"))
        for name in ("neverd-build-receipt.json", "neverd-dependency-inputs.json", "CMakeCache.txt"):
            path = build / name
            bindings[str(path)] = sha256(path)
            shutil.copyfile(path, output / name)
        result["input_sha256"] = bindings
        result["runs"] = []
        for seed in SEEDS:
            source = output / f"{seed}-inputs.bin"
            source.write_bytes(inputs(seed))
            run = {"seed": seed, "observations": source.stat().st_size // 24, "drivers": {}}
            for name, driver in drivers.items():
                destination = output / f"{seed}-{name}.bin"
                observation = run_driver(driver, source, destination)
                run["drivers"][name] = observation
                expected_backend = ("neverd-portable-binary80" if name == "portable" else
                                    "x86-64-original-instructions")
                expected_arch = "arm64" if machine == "aarch64" else "x86-64"
                if (observation["returncode"] or destination.stat().st_size != run["observations"] * 12 or
                        observation["metadata"]["backend"] != expected_backend or
                        observation["metadata"]["process_arch"] != expected_arch):
                    raise ValueError("driver execution or native identity is invalid")
            result["runs"].append(run)
        result["stable_inputs"] = (source_snapshot() == snapshot and
            git("rev-parse", "HEAD").decode().strip() == result["git_head"] and
            git("submodule", "status").decode().splitlines() == result["submodules"] and
            all(sha256(Path(name)) == digest for name, digest in bindings.items()))
        if not result["stable_inputs"]:
            raise ValueError("arithmetic inputs changed during execution")
        result["execution_completed"] = True
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError) as error:
        result["error"] = str(error)
    result["files"] = file_manifest(output)
    (output / "record.json").write_text(json.dumps(result, indent=2) + "\n")
    return result["execution_completed"]


def arithmetic_mismatches(requests, actual, expected):
    if len(requests) % 24 or len(actual) != len(requests) // 2 or len(expected) != len(actual):
        raise ValueError("arithmetic response count differs from the frozen inputs")
    failures = []
    for index in range(len(actual) // 12):
        a, b = actual[12*index:12*index+12], expected[12*index:12*index+12]
        if a[:10] != b[:10] or (int.from_bytes(a[10:], "little") ^ int.from_bytes(b[10:], "little")) & 0x23f:
            failures.append({"index": index, "input": requests[24*index:24*index+24].hex(),
                             "actual": a.hex(), "native_x64": b.hex()})
    return failures


def require_clean_source_record(directory, record):
    # Matching HEADs alone do not bind recordings made with different patches.
    # Debug recorders may retain dirty sources; reusable cross-host and timing
    # evidence must instead identify the exact committed source.
    if record.get("untracked_source_sha256") != {} or (directory / "source.patch").read_bytes():
        raise ValueError("frozen execution requires clean committed sources")


def generated_observations(directory, native, profile="arithmetic"):
    root = directory / ("reset-evidence" if profile == "resets" else "evidence")
    record = read_verified(root)
    require_clean_source_record(root, record)
    if (not record["passed"] or not record["stable_inputs"] or
            record.get("profile", "arithmetic") != profile or
            record["environment"]["machine"] != ("x86_64" if native else "aarch64")):
        raise ValueError("generated execution lacks passing native-host evidence")
    observations = list(root.glob("artifacts/" + (
        "x87-resets*/observations.jsonl" if profile == "resets" else
        "x87-generated*/observations.jsonl")))
    if len(observations) != 1:
        raise ValueError("missing or ambiguous generated observations")
    rows = [json.loads(line) for line in observations[0].read_text().splitlines()]
    commands = [json.loads(line) for line in observations[0].with_name("commands.jsonl").read_text().splitlines()]
    audit_observations(commands, rows, native, profile)
    return record, {(row["artifact"], row["case"]):
                    {k: v for k, v in row.items() if k != "native_reference"} for row in rows}


def compare(args):
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {"protocol": "neverd-x87-cross-host-qualification-v1", "passed": False,
              "exclusions": ["all CPUs", "general OS exception delivery", "unlisted instructions",
                             "arbitrary-program or formal equivalence", "speed claims"],
              "mismatches": [], "generated_mismatches": [], "reset_mismatches": []}
    try:
        x64, arm = args.x64.resolve(strict=True), args.arm64.resolve(strict=True)
        xr, xo = generated_observations(x64, True)
        ar, ao = generated_observations(arm, False)
        if xr["git_head"] != ar["git_head"]:
            raise ValueError("generated hosts used different source commits")
        for key in sorted(xo):
            fields = {field for field in xo[key] if xo[key][field] != ao[key].get(field)}
            if fields:
                result["generated_mismatches"].append({"artifact": key[0], "case": key[1],
                    "x64": {field: xo[key][field] for field in sorted(fields)},
                    "aarch64": {field: ao[key].get(field) for field in sorted(fields)}})
        result["git_head"] = xr["git_head"]
        result["generated_observations_per_host"] = len(xo)
        xrr, xro = generated_observations(x64, True, "resets")
        arr, aro = generated_observations(arm, False, "resets")
        if xrr["git_head"] != result["git_head"] or arr["git_head"] != result["git_head"]:
            raise ValueError("reset hosts used different source commits")
        for key in sorted(xro):
            if xro[key] != aro[key]:
                result["reset_mismatches"].append({"artifact": key[0], "case": key[1]})
        result["reset_observations_per_host"] = len(xro)
        reference = read_verified(x64 / "arithmetic-evidence")
        portable = read_verified(arm / "arithmetic-evidence")
        for item, machine in ((reference, "x86_64"), (portable, "aarch64")):
            if (item["protocol"] != PROTOCOL or not item["execution_completed"] or
                    not item["stable_inputs"] or item["machine"] != machine or
                    item["git_head"] != result["git_head"] or item["seeds"] != list(SEEDS)):
                raise ValueError("arithmetic evidence is incomplete or has mismatched inputs")
        result["arithmetic_observations_per_host"] = 0
        for seed in SEEDS:
            requests = (x64 / "arithmetic-evidence" / f"{seed}-inputs.bin").read_bytes()
            if requests != inputs(seed) or requests != (arm / "arithmetic-evidence" / f"{seed}-inputs.bin").read_bytes():
                raise ValueError("arithmetic inputs differ from the pinned seed protocol")
            original = (x64 / "arithmetic-evidence" / f"{seed}-original.bin").read_bytes()
            for host, root in (("x86_64", x64), ("aarch64", arm)):
                actual = (root / "arithmetic-evidence" / f"{seed}-portable.bin").read_bytes()
                failures = arithmetic_mismatches(requests, actual, original)
                result["mismatches"].extend({"host": host, "seed": seed, **failure} for failure in failures)
            result["arithmetic_observations_per_host"] += len(requests) // 24
        result["reference_cpu"] = reference["cpu"]
        result["destination_cpu"] = portable["cpu"]
        result["record_sha256"] = {str(root / name / "record.json"): sha256(root / name / "record.json")
            for root in (x64, arm) for name in ("evidence", "reset-evidence", "arithmetic-evidence")}
        result["passed"] = not (result["mismatches"] or result["generated_mismatches"] or result["reset_mismatches"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        result["error"] = str(error)
    (output / "record.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: result.get(k) for k in ("passed", "generated_observations_per_host",
                                               "reset_observations_per_host", "arithmetic_observations_per_host", "error")}, indent=2))
    return result["passed"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    capture = commands.add_parser("record")
    capture.add_argument("--portable-driver", type=Path, required=True)
    capture.add_argument("--original-driver", type=Path)
    capture.add_argument("--build-dir", type=Path, required=True)
    capture.add_argument("--output-dir", type=Path, required=True)
    audit = commands.add_parser("compare")
    audit.add_argument("--x64", type=Path, required=True)
    audit.add_argument("--arm64", type=Path, required=True)
    audit.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    return 0 if (record(args) if args.mode == "record" else compare(args)) else 1


if __name__ == "__main__":
    raise SystemExit(main())
