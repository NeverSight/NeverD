#!/usr/bin/env python3
"""Qualify exported event delivery with NeverC and both full LLVM backends."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--keep", type=Path, required=True)
    args = parser.parse_args()
    args.keep.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[3]
    tools = {"neverc": args.compiler.resolve(),
             "llc": (args.llvm_bin / "llc").resolve(),
             "clang": Path(shutil.which("clang") or ""),
             "ld.lld": Path(shutil.which("ld.lld") or ""),
             "objcopy": Path(shutil.which("objcopy") or "")}
    for name, path in tools.items():
        if not path.is_file():
            parser.error(f"missing {name}: {path}")
    records, profiles = [], []
    environment = dict(os.environ, TMPDIR=str(args.keep.resolve()))

    def run(command, stem, timeout=120):
        command = list(map(str, command))
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=timeout, env=environment)
        (args.keep / (stem + ".stdout")).write_text(result.stdout)
        (args.keep / (stem + ".stderr")).write_text(result.stderr)
        records.append({"command": command, "exit_code": result.returncode,
                        "stdout": stem + ".stdout", "stderr": stem + ".stderr"})
        (args.keep / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"{stem}: exit {result.returncode}\n{result.stderr}")

    fixture = root / "unittests/devirtualization/x86_64/fixtures/x64_continuation_kvm.c"
    runner = args.keep / "event-kvm-runner"
    run([tools["clang"], "-std=gnu11", "-O2", fixture, "-o", runner], "runner")
    for route in ("high", "llvm"):
        # Preserve the independently generated original-instruction harness
        # and freestanding bootstrap verbatim, including its outb oracle call.
        source = args.sources / f"event-{route}.O0.guest.c"
        linker = args.sources / f"event-{route}.O0.ld"
        if not source.is_file() or not linker.is_file():
            parser.error("capture EventDeliveryMatchesNativeFaultsNmiAndIrq artifacts first")
        for level in ("O0", "O2"):
            stem = f"event-{route}-{level}"
            ir = args.keep / (stem + ".ll")
            flags = ["--target=x86_64-unknown-linux-gnu", "-std=c23", "-" + level,
                     "-fno-lto", "-ffreestanding", "-fno-builtin-mimalloc",
                     "-fno-stack-protector", "-fno-pic", "-fno-pie",
                     "-mno-red-zone", "-mgeneral-regs-only"]
            run([tools["neverc"], *flags, "-S", "-emit-llvm", source, "-o", ir],
                stem + "-ir")
            body = re.search(r"^define[^\n]*@neverd_x64_deliver_event_body_v6\b.*?^}",
                             ir.read_text(), re.MULTILINE | re.DOTALL)
            if not body or "asm " in body[0]:
                raise RuntimeError("event delivery must retain its shared IR body without asm")
            for backend in ("neverc", "llvm-legacy", "llvm-new"):
                name = stem + "-" + backend
                obj, elf, binary = (args.keep / (name + suffix)
                                    for suffix in (".o", ".elf", ".bin"))
                if backend == "neverc":
                    run([tools["neverc"], *flags, "-c", source, "-o", obj], name + "-compile")
                else:
                    run([tools["llc"], "-mtriple=x86_64-unknown-linux-gnu",
                         "-filetype=obj", "-verify-machineinstrs", "-" + level,
                         *(["-enable-new-pm"] if backend == "llvm-new" else []),
                         ir, "-o", obj], name + "-compile")
                run([tools["ld.lld"], "-m", "elf_x86_64", "-T", linker,
                     obj, "-o", elf], name + "-link")
                run([tools["objcopy"], "-O", "binary", elf, binary], name + "-image")
                run([runner, binary], name + "-run", timeout=30)
                profiles.append({"route": route, "optimization": level,
                                 "backend": backend, "source_sha256": digest(source),
                                 "ir_sha256": digest(ir), "image_sha256": digest(binary),
                                 "original_cpu_comparisons": 10 * 3 * 2 * 8 * 7 * 2})
    report = {"schema_version": 1, "tools": {name: {"path": str(path),
               "sha256": digest(path)} for name, path in tools.items()},
              "fixture_sha256": digest(fixture), "profiles": profiles,
              "original_cpu_comparisons": sum(p["original_cpu_comparisons"] for p in profiles),
              "limitations": ["IA-32e event delivery, not SMM/SGX or legacy task gates",
                              "successful delivery RAM; no nested delivery faults",
                              "KVM native IRQ/NMI; original instructions produce tested faults"]}
    (args.keep / "qualification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(profiles)} profiles, {report['original_cpu_comparisons']} CPU comparisons")


if __name__ == "__main__":
    main()
