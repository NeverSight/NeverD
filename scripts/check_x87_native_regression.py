#!/usr/bin/env python3
"""Freeze native Linux x86 x87 generated-code regression artifacts.

Runs an explicitly selected owned-context or typed-projection test executable. Retains
original/generated sources, objects, executables, compiler invocations, test
results and source/build/CPU bindings. A successful observation is scoped to
the selected fixtures and host; it is not portable or formal qualification.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
        return digest.hexdigest()


def git(*args):
    return subprocess.check_output(["git", "-C", str(ROOT), *args])


def source_snapshot():
    patch = git("diff", "--binary", "HEAD", "--", ".")
    untracked = {}
    for name in git("ls-files", "--others", "--exclude-standard", "-z",
                    "--", "include", "lib", "unittests", "scripts").split(b"\0"):
        if name:
            path = Path(os.fsdecode(name))
            untracked[str(path)] = sha256(ROOT / path)
    return patch, untracked


def linked_libraries(executable):
    result = subprocess.run(["ldd", str(executable)], check=False,
                            capture_output=True, text=True)
    libraries = {}
    for line in result.stdout.splitlines():
        match = re.match(r"\s*(?:[^=]+ => )?(/.*?)\s+\(0x[0-9a-f]+\)", line)
        if match:
            library = Path(match.group(1)).resolve(strict=True)
            libraries[str(library)] = sha256(library)
    if result.returncode and "not a dynamic executable" not in result.stderr:
        raise ValueError("cannot identify driver runtime: " + result.stderr)
    return libraries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--build", required=True,
                        help="actual compiler, build configuration and build command")
    parser.add_argument("--filter", default="*",
                        help="GoogleTest filter; skipped cases never count as passing")
    parser.add_argument("--state-path", choices=("owned", "typed", "mixed"), default="owned",
                        help="actual state path exercised by the selected driver")
    parser.add_argument("--linker", choices=("default", "lld"), default="default",
                        help="linker used by the generated-program fixture")
    parser.add_argument("--timeout", type=int, default=1800)
    args = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        parser.error("this recorder currently qualifies only a Linux x86-64 host")
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    driver = args.driver.resolve(strict=True)
    destination = args.output_dir.resolve()
    destination.mkdir(parents=True, exist_ok=False)
    artifacts = destination / "artifacts"
    record = {
        "protocol": "neverd-x87-native-generated-regression-v1",
        "profiles": (["owned-x64-context-v1", "typed-x87-projection-v1"]
                     if args.state_path == "mixed" else
                     ["owned-x64-context-v1" if args.state_path == "owned" else
                      "typed-x87-projection-v1"]) + ["native-x87-host-reference-v1"],
        "scope": "selected deterministic native x86 original/generated fixtures",
        "seed": None,
        "state_observations": (
            ["CW/SW/tags and retained binary80 payloads",
             "defined or same-host selected condition/status bits",
             "instruction-specific memory and exception outcomes",
             "selected Linux x64 signal codes and saved x87 state"]
            if args.state_path == "owned" else
            ["raw binary80 values, selected status/classification and guarded memory",
             "explicit unsupported-input traps; no complete architectural state"]),
        "exclusions": ["relocated instruction/data pointer identity",
                       "compiler caller-saved XMM contents", "other CPU models",
                       "ABIs and modes outside selected fixtures",
                       "general OS exception delivery or resumption",
                       "ARM64 translation", "formal equivalence", "performance"],
        "declared_build": args.build,
        "driver": {"path": str(driver), "sha256": sha256(driver)},
        "filter": args.filter,
        "environment": {"platform": platform.platform(),
                        "machine": platform.machine(),
                        "python": platform.python_version(),
                        "locale": {key: os.environ.get(key)
                                   for key in ("LANG", "LC_ALL", "TZ")}},
        "script_sha256": sha256(Path(__file__)),
        "passed": False,
    }
    started = time.monotonic()
    try:
        record["cpu"] = json.loads(subprocess.check_output(["lscpu", "--json"]))
        compiler = Path(shutil.which("clang") or "").resolve(strict=True)
        record["compiler"] = {
            "path": str(compiler), "sha256": sha256(compiler),
            "version": subprocess.check_output([str(compiler), "--version"],
                                                text=True),
        }
        linker_name = subprocess.check_output(
            [str(compiler), "-print-prog-name=" +
             ("ld.lld" if args.linker == "lld" else "ld")], text=True).strip()
        # lld selects its personality from argv[0]. Hash the resolved file,
        # but query the actual ld.lld entry point rather than generic `lld`.
        linker_command = Path(shutil.which(linker_name) or linker_name).absolute()
        linker = linker_command.resolve(strict=True)
        record["linker"] = {"path": str(linker), "invocation": str(linker_command),
                            "sha256": sha256(linker),
                            "version": subprocess.check_output(
                                [str(linker_command), "--version"], text=True)}
        runtime = linked_libraries(driver) | linked_libraries(compiler)
        runtime.update(linked_libraries(linker))
        # Pin both C runtimes before executing typed i386/x64 programs. A
        # generated binary may use only a subset; its actual ldd closure below
        # must still be contained in this pre-run binding.
        if args.state_path != "owned":
            for target in ("i386-linux-gnu", "x86_64-linux-gnu"):
                for name in ("libc.so.6", "libm.so.6", "libgcc_s.so.1",
                             "ld-linux.so.2", "ld-linux-x86-64.so.2"):
                    candidate = subprocess.check_output(
                        [str(compiler), "--target=" + target,
                         "-print-file-name=" + name], text=True).strip()
                    path = Path(candidate)
                    if path.is_file():
                        path = path.resolve(strict=True)
                        runtime[str(path)] = sha256(path)
        record["runtime_sha256"] = runtime
        record["git_head"] = git("rev-parse", "HEAD").decode().strip()
        record["submodules"] = git("submodule", "status").decode().splitlines()
        patch, untracked = source_snapshot()
        (destination / "source.patch").write_bytes(patch)
        record["untracked_source_sha256"] = untracked
        for name in untracked:
            copy = destination / "untracked-source" / name
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, copy)
        result_path = destination / "results.xml"
        command = [str(driver), f"--gtest_filter={args.filter}",
                   f"--gtest_output=xml:{result_path}"]
        record["command"] = command
        environment = dict(os.environ, NEVERD_ISA_REGRESSION_ARTIFACT_DIR=str(artifacts))
        with (destination / "test.log").open("wb") as log:
            completed = subprocess.run(command, env=environment, stdout=log,
                                       stderr=subprocess.STDOUT, timeout=args.timeout,
                                       check=False)
        record["returncode"] = completed.returncode
        record["tests"] = [
            {"name": case.get("classname") + "." + case.get("name"),
             "status": case.get("status"), "result": case.get("result"),
             "failures": [item.get("message") for item in case.findall("failure")],
             "skipped": [item.get("message") for item in case.findall("skipped")]}
            for case in ET.parse(result_path).iter("testcase")
        ]
        binaries = list(artifacts.rglob("*.exe"))
        record["retained_executables"] = len(binaries)
        if not record["tests"] or not binaries:
            raise ValueError("no tests or generated executable artifacts were recorded")
        if any(path.stat().st_size == 0 for path in binaries):
            raise ValueError("empty generated executable artifact")
        generated_runtime = {}
        for executable in binaries:
            generated_runtime.update(linked_libraries(executable))
        record["generated_runtime_sha256"] = generated_runtime
        if any(runtime.get(path) != digest
               for path, digest in generated_runtime.items()):
            raise ValueError("generated runtime was not bound before execution")
        record["stable_inputs"] = (
            sha256(driver) == record["driver"]["sha256"] and
            sha256(compiler) == record["compiler"]["sha256"] and
            sha256(linker) == record["linker"]["sha256"] and
            linker_command.resolve(strict=True) == linker and
            all(sha256(Path(path)) == digest for path, digest in runtime.items()) and
            git("rev-parse", "HEAD").decode().strip() == record["git_head"] and
            git("submodule", "status").decode().splitlines() == record["submodules"] and
            source_snapshot() == (patch, untracked))
        record["passed"] = (
            completed.returncode == 0 and record["stable_inputs"] and
            all(case["status"] == "run" and not case["failures"] and
                not case["skipped"] for case in record["tests"]))
    except (OSError, subprocess.SubprocessError, ValueError, ET.ParseError) as error:
        record["error"] = str(error)
    record["elapsed_seconds_including_builds_and_io"] = time.monotonic() - started
    record["files"] = {
        str(path.relative_to(destination)): {"bytes": path.stat().st_size,
                                             "sha256": sha256(path)}
        for path in sorted(destination.rglob("*")) if path.is_file()
    }
    (destination / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"passed": record["passed"],
                      "tests": len(record.get("tests", [])),
                      "retained_executables": record.get("retained_executables", 0),
                      "record": str(destination / "record.json")}, indent=2))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
