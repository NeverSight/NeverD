#!/usr/bin/env python3
"""Qualify actual NeverD RTM exports against explicit Intel SDE outcomes.

Generate the eight inputs with RtmAbortNativeCompilerPreservesProviderState
and NEVERD_ISA_REGRESSION_ARTIFACT_DIR. Only the appended harness changes;
the emitted HighC/LLVMC source remains byte-for-byte intact.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--backend-llc", type=Path)
    parser.add_argument("--new-pm", action="store_true")
    parser.add_argument("--sde", type=Path, required=True)
    parser.add_argument("--host-cc", default=shutil.which("clang"))
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--keep", type=Path, required=True)
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
    tools = {key: {"path": str(path), "sha256": digest(path)}
             for key, path in paths.items()}
    commands, profiles, sources = [], [], {}

    def run(command):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=directory, capture_output=True,
                                text=True, timeout=120)
        commands.append({"argv": command, "returncode": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr})
        (directory / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"{command}\n{result.stdout}\n{result.stderr}")

    inputs = sorted(args.sources.resolve().glob("rtm*.test.c"))
    assert len(inputs) == 8, "expected rel16/rel32, HighC/LLVMC, switch/direct"
    for path in inputs:
        stem = path.name.removesuffix(".test.c")
        source = path.with_name(stem + ".c")
        code, test = source.read_text(), path.read_text()
        assert test.startswith(code)
        harness = test[len(code):]
        substitutions = {
            "int main(void) {": "int main(int argc, char **argv) {\n"
                                  "  if(argc!=3)abort();\n"
                                  "  const uint64_t expected=strtoull(argv[1],0,0);",
            "for(unsigned n=0;n<64;++n)": "if(supported!=atoi(argv[2]))abort();\n"
                                        "  for(unsigned n=0;n<64;++n)",
            "uint64_t status=s.GPR[0];": "uint64_t status=s.GPR[0];"
                                       "if(status!=expected)abort();",
        }
        for before, after in substitutions.items():
            assert harness.count(before) == 1, f"test harness changed: {before}"
            harness = harness.replace(before, after)
        inp = directory / (stem + ".c")
        inp.write_text(code + harness)
        sources[stem] = {
            "source_sha256": digest(source),
            "manifest_sha256": digest(path.with_name(stem + ".json")),
            "exported_test_sha256": digest(path),
            "compiler_input_sha256": digest(inp),
            "source_preserved_verbatim": True,
            "harness_extension": "Require CPUID admission and exact status from arguments",
        }
        for opt in ("O0", "O2"):
            exe = directory / (stem + "-" + opt)
            ir, obj = exe.with_suffix(".ll"), exe.with_suffix(".o")
            flags = ["--target=x86_64-unknown-linux-gnu", "-" + opt, "-std=c23",
                     "-ffreestanding", "-fno-lto", "-fno-builtin-mimalloc",
                     "-fno-pie", "-fno-stack-protector"]
            run([paths["compiler"], *flags, "-S", "-emit-llvm", inp, "-o", ir])
            text = ir.read_text()
            assert "asm " not in text
            assert "@llvm.x86.rtm.abort.pair(i32 167)" in text
            assert "@llvm.x86.cpuidex(" in text
            if args.backend_llc:
                run([paths["llc"], "-mtriple=x86_64-unknown-linux-gnu",
                     "-filetype=obj", "-verify-machineinstrs", "-" + opt,
                     *(["-enable-new-pm"] if args.new_pm else []), ir, "-o", obj])
            else:
                run([paths["compiler"], *flags, "-c", inp, "-o", obj])
            assert bytes([0xc7, 0xf8, 3, 0, 0, 0, 0xc6, 0xf8, 167]) in obj.read_bytes()
            run([paths["host"], "-no-pie", obj, "-o", exe])
            for mode in ("full", "0", "2", "6", "8"):
                # -tsx overrides abort mode and enables CPUID.RTM in SDE 10.13.1.
                # Forced abort mode clears CPUID.RTM; the provider must decline.
                flags = (["-tsx", "-rtm_mode", "full"] if mode == "full" else
                         ["-rtm_mode", "abort", "-rtm_abort_reason", mode])
                run([paths["sde"], "-hsw", *flags, "--", exe,
                     str((167 << 24) | 1) if mode == "full" else mode,
                     "1" if mode == "full" else "0"])
                profiles.append({
                    "source": stem, "optimization": opt, "mode": mode,
                    "checks": 64, "executable_sha256": digest(exe),
                    "qualified": ("explicit-abort state publication" if mode == "full"
                                  else "CPUID refusal without state publication"),
                })
    assert all(digest(tool["path"]) == tool["sha256"] for tool in tools.values())
    report = {
        "passed": True, "physical_rtm_qualified": False,
        "early_abort_provider_publication_qualified": False,
        "tools": tools, "new_pass_manager": args.new_pm,
        "sources": sources, "profiles": profiles,
        "commands_sha256": digest(directory / "commands.json"),
        "script_sha256": digest(__file__),
    }
    (directory / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "profiles": len(profiles),
                      "state_checks": sum(p["checks"] for p in profiles)}))


if __name__ == "__main__":
    main()
