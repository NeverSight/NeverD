#!/usr/bin/env python3
"""Qualify exported native RTM regions against their original machine code."""

import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import threading


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("regions", "prefixes", "prefix-pairs"), default="regions")
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--backend-llc", type=Path)
    parser.add_argument("--new-pm", action="store_true")
    parser.add_argument("--sde", type=Path, required=True)
    parser.add_argument("--host-cc", default=shutil.which("clang"))
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--keep", type=Path, required=True)
    parser.add_argument("--jobs", type=int, choices=range(1, 9), default=4)
    args = parser.parse_args()
    if args.new_pm and not args.backend_llc:
        parser.error("--new-pm requires --backend-llc")
    if not args.host_cc:
        parser.error("host Clang is required")
    directory = args.keep.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    paths = {"compiler": args.compiler.resolve(), "sde": args.sde.resolve(),
             "host": Path(args.host_cc).resolve()}
    if args.backend_llc:
        paths["llc"] = args.backend_llc.resolve()
    tools = {name: {"path": str(path), "sha256": digest(path)}
             for name, path in paths.items()}
    resource = subprocess.run([paths["compiler"], "-print-resource-dir"],
                              capture_output=True, text=True, check=True)
    header = Path(resource.stdout.strip()) / "include" / "cpuid.h"
    tools["compiler_cpuid_header"] = {"path": str(header), "sha256": digest(header)}
    commands, sources = [], {}
    lock = threading.Lock()

    def run(command):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=directory, capture_output=True,
                                text=True, timeout=120)
        with lock:
            commands.append({"argv": command, "returncode": result.returncode,
                             "stdout": result.stdout, "stderr": result.stderr})
            (directory / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"{command}\n{result.stdout}\n{result.stderr}")

    pair = args.suite == "prefix-pairs"
    provider_name = "neverd_x64_rtm_abort_environment_v6" if pair else "neverd_x64_rtm_environment_v6"
    binding_name = "native_rtm_abort_environment" if pair else "native_rtm_environment"
    pattern = ("rtm-prefix-pair-*.test.c" if pair else
               "rtm-prefix-*.test.c" if args.suite == "prefixes" else "rtm-region-*.test.c")
    inputs = sorted(path for path in args.sources.resolve().glob(pattern)
                    if pair or not path.name.startswith("rtm-prefix-pair-"))
    expected = 48 if args.suite == "regions" else 4
    assert len(inputs) == expected, f"expected {expected} inputs for {args.suite}"
    checks = {}
    for path in inputs:
        stem = path.name.removesuffix(".test.c")
        source = path.with_name(stem + ".c")
        code = source.read_bytes()
        assert path.read_bytes().startswith(code)
        manifest = json.loads(path.with_name(stem + ".json").read_text())
        assert manifest["source_sha256"] == digest(source)
        binding = manifest[binding_name]
        assert binding["provider"] == provider_name
        checks[stem] = (64 * len(binding["regions"]) if pair else
                        128 * len(binding["begins"]) if args.suite == "prefixes" else 64)
        sources[stem] = {"source_sha256": digest(source),
                         "manifest_sha256": digest(path.with_name(stem + ".json")),
                         "test_sha256": digest(path),
                         "source_and_harness_preserved_verbatim": True}

    def check(job):
        path, opt = job
        stem = path.name.removesuffix(".test.c")
        exe = directory / (stem + "-" + opt)
        ir, obj = exe.with_suffix(".ll"), exe.with_suffix(".o")
        flags = ["--target=x86_64-unknown-linux-gnu", "-" + opt, "-std=c23",
                 "-ffreestanding", "-fno-lto", "-fno-builtin-mimalloc",
                 "-fno-pie", "-fno-stack-protector"]
        run([paths["compiler"], *flags, "-S", "-emit-llvm", path, "-o", ir])
        text = ir.read_text()
        provider = re.search(r"^define[^\n]*@" + provider_name + r"\b.*?^}",
                             text, re.MULTILINE | re.DOTALL)
        assert provider and "asm " not in provider[0], "provider must use native intrinsics"
        intrinsics = (("rtm.abort.pair", "cpuidex") if pair else
                      ("xbegin", "xend", "xabort", "xtest", "cpuidex"))
        for intrinsic in intrinsics:
            assert "@llvm.x86." + intrinsic + "(" in provider[0], intrinsic
        if args.backend_llc:
            run([paths["llc"], "-mtriple=x86_64-unknown-linux-gnu",
                 "-filetype=obj", "-verify-machineinstrs", "-" + opt,
                 *(["-enable-new-pm"] if args.new_pm else []), ir, "-o", obj])
        else:
            run([paths["compiler"], *flags, "-c", path, "-o", obj])
        run([paths["host"], "-no-pie", obj, "-o", exe])
        profiles = []
        for mode, flags, supported in (
                ("full", ["-tsx", "-rtm_mode", "full"], "1"),
                # SDE 10.13.1 clears CPUID RTM in forced-abort mode. This
                # checks refusal; it does not qualify early-abort publication.
                ("cpuid-disabled", ["-rtm_mode", "abort", "-rtm_abort_reason", "0"], "0")):
            run([paths["sde"], "-hsw", *flags, "--", exe, supported])
            profiles.append({"source": stem, "optimization": opt, "mode": mode,
                             "checks": checks[stem], "executable_sha256": digest(exe)})
        return profiles

    profiles = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = [(path, opt) for path in inputs for opt in ("O0", "O2")]
        for index, result in enumerate(pool.map(check, jobs), 1):
            profiles.extend(result)
            if index % 16 == 0:
                print(f"qualified {index}/{len(jobs)} compiler profiles", flush=True)
    assert all(digest(tool["path"]) == tool["sha256"] for tool in tools.values())
    report = {"passed": True, "suite": args.suite, "physical_rtm_qualified": False,
              "tools": tools, "new_pass_manager": args.new_pm,
              "sources": sources, "profiles": profiles,
              "commands_sha256": digest(directory / "commands.json"),
              "script_sha256": digest(__file__)}
    (directory / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "profiles": len(profiles),
                      "state_checks": sum(p["checks"] for p in profiles)}))


if __name__ == "__main__":
    main()
