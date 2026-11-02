#!/usr/bin/env python3
"""Freeze native-host execution of the explicit portable x87 leaf interface.

Only the repository's authored fixture driver is run. This Linux x64/AArch64
protocol checks LLVM, HighC and LLVM-C artifacts and all deterministic state and
refusal observations. It does not qualify translation or a source machine ABI.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET

from check_x87_native_regression import git, linked_libraries, sha256, source_snapshot, ROOT


TEST_NAME = "X87PortableGenerated.AllRoutesExecuteExactStateAndRefusalBoundaries"
RESET_TEST_NAME = "X87PortableGenerated.AllRoutesExecuteFnopEmmsAndFemms"
PROTOCOL = "neverd-x87-portable-generated-leaf-v1"


def expected_cases(shape):
    exact = {
        f"exact-{top}-{pc}-{rc}-{count}"
        for top in range(8) for pc in (0, 2, 3) for rc in range(4)
        for count in ((1,) if shape == 0 else (1, 2))
    }
    return exact | {f"refusal-{kind}" for kind in range(7)} | {
        "null-execution", "null-output"
    } | ({"untaken-branch"} if shape == 1 else set())


def expected_artifacts():
    return {
        f"{mode}-{shape}-{route}-O{opt}": shape
        for mode in ("default", "noopt") for shape in range(3)
        for route in range(3) for opt in (0, 2)
    }


def reset_cases():
    return {f"reset-{top}-{tags}-{known}" for top in range(8)
            for tags in (0, 85, 255) for known in (0, 150, 255)} | {
                f"reset-refusal-{kind}" for kind in range(3)}


def reset_input(case):
    state = bytearray.fromhex("4e563837010010020100000000000000") + bytearray(512)
    state[16:18] = (0x37f).to_bytes(2, "little")
    state[19] = 5 << 3
    for index in range(32, 160):
        state[16 + index] = index ^ 0xa5
    if case.startswith("reset-refusal-"):
        refusal = int(case.rsplit("-", 1)[1])
        state[9] = state[20] = 255
        if refusal == 0:
            state[8] = 0
        elif refusal == 1:
            state[16] &= ~1
            state[18] = 0x81
        elif refusal == 2:
            state[17] = 1
        else:
            raise ValueError("unknown reset refusal")
    else:
        _, top, tags, known = case.split("-")
        state[9] = int(known)
        state[18] = 0x25
        state[19] = 0x47 | (int(top) << 3)
        state[20] = int(tags)
        state[22:24] = b"\x71\x05"
        state[24] = 0x45
        state[32] = 0x67
    return state


def audit_reset_observations(commands, observations, native_reference):
    expected, cases = expected_artifacts(), reset_cases()
    if (type(native_reference) is not bool or len(commands) != len(expected) or
            {item["artifact"] for item in commands} != set(expected)):
        raise ValueError("reset audit needs native host identity and every compiled artifact")
    architecture = "x86_64" if native_reference else "aarch64"
    for command in commands:
        code = "488D04374801D0" + ("D9D0", "0F77", "0F0E")[expected[command["artifact"]]] + "C3"
        if command["source_bytes"].upper() != code or not command["target"].startswith(architecture + "-"):
            raise ValueError("wrong reset instruction or target")
    observed = {name: set() for name in expected}
    canonical, counts, native_count = {}, {}, 0
    for item in observations:
        name, case = item["artifact"], item["case"]
        if name not in expected or case not in cases or case in observed[name]:
            raise ValueError("unexpected or duplicate reset observation")
        observed[name].add(case)
        kind = expected[name]
        before, after = bytes.fromhex(item["before"]), bytes.fromhex(item["after"])
        if before != reset_input(case):
            raise ValueError("reset input differs from its complete seeded state")
        result = bytearray(before)
        refused = case.startswith("reset-refusal-")
        status = (2, 6, 5)[int(case.rsplit("-", 1)[1])] if refused else 0
        if not refused:
            if kind == 0:
                result[24:32] = (0x1007).to_bytes(8, "little")
            else:
                top = (before[19] >> 3) & 7
                result[19] &= 0xc7
                result[20] = 0
                for physical in range(8):
                    old, new = 48 + ((physical + 8 - top) & 7) * 16, 48 + physical * 16
                    result[new:new + 16] = before[old:old + 16]
            if kind == 2:
                result[9] = 0
        if (after != result or type(item["status"]) is not int or item["status"] != status or
                int(item["return"], 16) != (2**64 - 1 if refused else 0x4002) or
                item["instruction_completed"] is not (not refused) or item["memory"] != [] or
                item["memory_access"] is not False or item["has_scalar_result"] is not False or
                int(item["instruction_address"], 16) != 0x1007 or int(item["data_address"], 16) != 0):
            raise ValueError("reset state, refusal or publication result is incorrect")
        reference = item["native_reference"]
        required = native_reference and not refused and kind != 2 and case.endswith("-255")
        if (required and not isinstance(reference, dict)) or (not required and reference is not None):
            raise ValueError("missing or unexpected native reset reference")
        if required:
            mask = 0xb8ff if kind == 0 else 0xffff
            if (reference["control"] != int.from_bytes(after[16:18], "little") or
                    reference["status_defined"] != int.from_bytes(after[18:20], "little") & mask or
                    reference["tags"] != after[20] or
                    bytes.fromhex(reference["retained_result"]) != after[160:170] or
                    bytes.fromhex(reference["retained_second"]) != after[144:154]):
                raise ValueError("native reset reference disagrees with defined state")
            native_count += 1
        comparison = {key: value for key, value in item.items()
                      if key not in ("artifact", "native_reference")}
        key = f"{kind}/{case}"
        if key in canonical and canonical[key] != comparison:
            raise ValueError("reset routes or optimization modes disagree")
        canonical[key] = comparison
        counts[str(status)] = counts.get(str(status), 0) + 1
    if any(found != cases for found in observed.values()):
        raise ValueError("missing reset state or refusal observation")
    return {"artifacts": len(commands), "observations": len(observations),
            "distinct_cases": len(canonical), "statuses": counts,
            "native_x64_original_comparisons": native_count,
            "cross_route_and_optimization_agreement": True,
            "canonical_state_sha256": hashlib.sha256(json.dumps(canonical, sort_keys=True).encode()).hexdigest()}


def audit_observations(commands, observations, native_reference=None, profile="arithmetic"):
    if profile == "resets":
        return audit_reset_observations(commands, observations, native_reference)
    expected = expected_artifacts()
    seen_commands = {item["artifact"] for item in commands}
    if seen_commands != set(expected) or len(commands) != len(expected):
        raise ValueError("missing, duplicate or unexpected compiled artifact")
    observed = {name: set() for name in expected}
    canonical = {}
    counts = {}
    native_count = 0
    for item in observations:
        name, case = item["artifact"], item["case"]
        if name not in expected or case not in expected_cases(expected[name]):
            raise ValueError("unexpected artifact or observation case")
        if case in observed[name]:
            raise ValueError("duplicate observation")
        observed[name].add(case)
        status = item["status"]
        expected_status = (2, 6, 5, 9, 10, 12, 13)[int(case[8:])] if case.startswith("refusal-") else (1 if case.startswith("null-") else 0)
        if type(status) is not int or status != expected_status:
            raise ValueError("unexpected success or refusal status")
        for field in ("before", "after"):
            if len(bytes.fromhex(item[field])) != 528:
                raise ValueError("state observation does not contain the complete v1 object")
        reference = item.get("native_reference")
        if case.startswith("exact-"):
            if native_reference is True and not isinstance(reference, dict):
                raise ValueError("missing native x64 original observation")
            if native_reference is False and reference is not None:
                raise ValueError("non-x64 host claims a native x87 observation")
            native_count += reference is not None
        comparison = {key: value for key, value in item.items() if key != "artifact"}
        key = expected[name], case
        if key in canonical and canonical[key] != comparison:
            raise ValueError("generated routes or optimization modes disagree")
        canonical[key] = comparison
        counts[str(status)] = counts.get(str(status), 0) + 1
    if any(observed[name] != expected_cases(shape) for name, shape in expected.items()):
        raise ValueError("missing deterministic state or refusal observation")
    return {"artifacts": len(commands), "observations": len(observations),
            "distinct_cases": len(canonical), "statuses": counts,
            "native_x64_original_comparisons": native_count,
            "cross_route_and_optimization_agreement": True}


def audit_tests(path, test_name=TEST_NAME):
    cases = list(ET.parse(path).iter("testcase"))
    if len(cases) != 1:
        raise ValueError("expected exactly the generated execution test")
    case = cases[0]
    if (case.get("classname") + "." + case.get("name") != test_name or
            case.get("status") != "run" or case.get("result") != "completed" or
            case.findall("failure") or case.findall("skipped")):
        raise ValueError("generated execution did not pass without skips")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--profile", choices=("arithmetic", "resets"), default="arithmetic")
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "aarch64"):
        parser.error("this frozen protocol requires native Linux x64 or AArch64")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    driver, runtime, compiler = (path.resolve(strict=True) for path in
                                 (args.driver, args.runtime, args.compiler))
    build = args.build_dir.resolve(strict=True)
    record = {
        "protocol": PROTOCOL, "profile": args.profile, "passed": False, "seed": None,
        "scope": ("authored x64 owned FNOP/EMMS/FEMMS leaf with an explicit host C interface"
                  if args.profile == "resets" else
                  "authored x64 owned-context leaf/branch/loop source with an explicit host C interface"),
        "properties": ["raw binary80", "CW/SW/TOP/tags and physical-payload provenance",
                       "native x64 original results and defined state where this host supports x87",
                       "numeric guest IP/DP", "success-only source return", "checked memory and refusal order"],
        "exclusions": ["source long-double calling conventions", "i386 portable generated execution",
                       "translation", "native reference identity for relocated IP/DP or undefined C0/C2/C3",
                       "general OS exception delivery", "symbolic or formal equivalence",
                       "throughput or speed improvements", "unlisted instruction forms or programs"],
        "environment": {"platform": platform.platform(), "machine": platform.machine(),
                        "python": platform.python_version()},
        "script_sha256": sha256(Path(__file__)),
    }
    started = time.monotonic()
    try:
        record["cpu"] = json.loads(subprocess.check_output(["lscpu", "--json"]))
        record["git_head"] = git("rev-parse", "HEAD").decode().strip()
        record["submodules"] = git("submodule", "status").decode().splitlines()
        patch, untracked = source_snapshot()
        (output / "source.patch").write_bytes(patch)
        record["untracked_source_sha256"] = untracked
        for name in untracked:
            destination = output / "untracked-source" / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        receipt = json.loads((build / "neverd-build-receipt.json").read_text())
        if receipt["status"] != "success":
            raise ValueError("normal build wrapper has no successful receipt")
        build_inputs = {}
        for name in ("neverd-build-receipt.json", "neverd-dependency-inputs.json", "CMakeCache.txt"):
            path = build / name
            build_inputs[str(path)] = sha256(path)
            shutil.copyfile(path, output / name)
        linker_name = subprocess.check_output([str(compiler), "-print-prog-name=ld"], text=True).strip()
        linker_command = Path(shutil.which(linker_name) or linker_name).absolute()
        linker = linker_command.resolve(strict=True)
        bindings = {str(path): sha256(path) for path in (driver, runtime, compiler, linker)}
        bindings.update(linked_libraries(driver))
        bindings.update(linked_libraries(runtime))
        record["input_sha256"] = bindings
        record["build_inputs_sha256"] = build_inputs
        record["compiler"] = {"path": str(compiler), "version": subprocess.check_output([str(compiler), "--version"], text=True)}
        record["linker"] = {"path": str(linker), "version": subprocess.check_output([str(linker_command), "--version"], text=True)}
        xml = output / "results.xml"
        test_name = RESET_TEST_NAME if args.profile == "resets" else TEST_NAME
        command = [str(driver), "--gtest_filter=" + test_name, "--gtest_output=xml:" + str(xml)]
        record["command"] = command
        with (output / "test.log").open("wb") as log:
            result = subprocess.run(command, env=dict(os.environ, NEVERD_X87_GENERATED_ARTIFACT_DIR=str(output / "artifacts")),
                                    stdout=log, stderr=subprocess.STDOUT, timeout=1800, check=False)
        record["returncode"] = result.returncode
        if result.returncode:
            raise ValueError("generated fixture driver failed")
        audit_tests(xml, test_name)
        directories = list((output / "artifacts").glob(
            "x87-resets*" if args.profile == "resets" else "x87-generated*"))
        if len(directories) != 1:
            raise ValueError("expected one isolated artifact directory")
        directory = directories[0]
        commands = [json.loads(line) for line in (directory / "commands.jsonl").read_text().splitlines()]
        observations = [json.loads(line) for line in (directory / "observations.jsonl").read_text().splitlines()]
        record["coverage"] = audit_observations(commands, observations, platform.machine() == "x86_64", args.profile)
        generated_runtime = {}
        for item in commands:
            arguments = item["command"]
            if Path(arguments[0]).resolve(strict=True) != compiler or str(runtime) not in arguments:
                raise ValueError("generated artifact used an unbound compiler or runtime")
            binary = Path(arguments[arguments.index("-o") + 1]).resolve(strict=True)
            if not binary.is_relative_to(directory) or not binary.stat().st_size:
                raise ValueError("generated binary is empty or outside the artifact directory")
            generated_runtime.update(linked_libraries(binary))
        if any(bindings.get(name) != digest for name, digest in generated_runtime.items()):
            raise ValueError("generated artifact used an unbound runtime library")
        record["generated_runtime_sha256"] = generated_runtime
        record["stable_inputs"] = (
            all(sha256(Path(name)) == digest for name, digest in {**bindings, **build_inputs}.items()) and
            git("rev-parse", "HEAD").decode().strip() == record["git_head"] and
            git("submodule", "status").decode().splitlines() == record["submodules"] and
            source_snapshot() == (patch, untracked) and linker_command.resolve(strict=True) == linker)
        if not record["stable_inputs"]:
            raise ValueError("source, compiler, runtime or build inputs changed during recording")
        record["passed"] = True
    except (OSError, subprocess.SubprocessError, ValueError, KeyError, TypeError, ET.ParseError) as error:
        record["error"] = str(error)
    record["elapsed_seconds_including_compilation"] = time.monotonic() - started
    record["files"] = {str(path.relative_to(output)): {"bytes": path.stat().st_size, "sha256": sha256(path)}
                       for path in sorted(output.rglob("*")) if path.is_file()}
    (output / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"passed": record["passed"], "coverage": record.get("coverage"),
                      "error": record.get("error"), "record": str(output / "record.json")}, indent=2))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
