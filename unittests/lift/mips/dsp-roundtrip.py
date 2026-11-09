#!/usr/bin/env python3
"""Qualify MIPS DSP source against independent state and target execution.

Use the repository-pinned Unicorn bindings/native library, clang and lld.
DSPControl is initialized and observed with machine instructions, including
after the generated function returns. No vendor binary executes here.
"""

import argparse
import hashlib
from itertools import product
from pathlib import Path
import subprocess
import sys

import roundtrip
from unicorn import mips_const as mips

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts


FUNCTIONS = ("dsp_read", "dsp_write", "dsp_partial", "dsp_add",
             "dsp_saturate", "dsp_zero", "dsp_read_then_write",
             "dsp_return_slot", "dsp_plain_add", "dsp_sub",
             "dsp_sub_saturate", "dsp_sub_zero", "dsp_half_add",
             "dsp_half_add_saturate", "dsp_half_sub", "dsp_half_sub_saturate")
INITIAL_STATES = (0, 0x01100021, 0x0fff7fbf)
INPUTS = ((0, 0), (0xff01807f, 0x02018081), (0x11223344, 0x55667788),
          (0xffffffff, 1), (0x80808080, 0x80808080),
          (0x7fff8000, 0x0001ffff), (0x80007fff, 0x7fff8000),
          (0x80008000, 0x80008000))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected(function, initial, arguments):
    a, b = arguments
    if function == "dsp_read":
        return initial, initial
    if function == "dsp_read_then_write":
        return initial, a & 0x0fff7fbf
    if function == "dsp_return_slot":
        return 7, initial
    if function == "dsp_plain_add":
        return (a + b) & 0xffffffff, initial
    if function == "dsp_write":
        return 7, a & 0x0fff7fbf
    if function == "dsp_partial":
        mask = 0x0f00203f  # position, carry and condition groups
        state = (initial & ~mask) | (a & mask)
        return state, state
    if function.startswith("dsp_half_"):
        value, state = 0, initial
        for lane in range(2):
            left, right = (a >> (lane * 16)) & 65535, (b >> (lane * 16)) & 65535
            left = left - 65536 if left >= 32768 else left
            right = right - 65536 if right >= 32768 else right
            total = left - right if "_sub" in function else left + right
            state |= 0x100000 if total < -32768 or total > 32767 else 0
            value |= ((max(-32768, min(total, 32767)) if function.endswith("saturate") else total) & 65535) << (lane * 16)
        return value, state
    value, state = 0, initial
    for lane in range(4):
        left, right = (a >> (lane * 8)) & 255, (b >> (lane * 8)) & 255
        total = left - right if function.startswith("dsp_sub") else left + right
        state |= 0x100000 if total < 0 or total > 255 else 0
        value |= (max(0, min(total, 255)) if function.endswith("saturate") else total % 256) << (lane * 8)
    return (7 if function.endswith("zero") else value), state


def observe(path, little, function, initial, arguments):
    uc, entry = roundtrip.emulator(path, 32, little, function,
                                  cpu_model=mips.UC_CPU_MIPS32_74KF)
    order = "little" if little else "big"
    setup = (0x40086000, 0x3c090100, 0x01094025, 0x40886000,
             0x000000c0, 0x7c81fcf8)  # Status.MX, then WRDSP a0,63
    start = roundtrip.STOP + 0x200
    uc.mem_write(start, b"".join(word.to_bytes(4, order) for word in setup))
    uc.reg_write(mips.UC_MIPS_REG_A0, initial)
    uc.emu_start(start, start + 4 * len(setup), count=len(setup))
    value, memory = roundtrip.invoke(uc, entry, 32, little, arguments)
    if memory != "00" * 16:
        raise RuntimeError("DSP leaf unexpectedly changed memory")
    start = roundtrip.STOP + 0x100
    uc.mem_write(start, (0x7c3f44b8).to_bytes(4, order))  # RDDSP t0,63
    uc.emu_start(start, start + 4, count=1)
    return value, uc.reg_read(mips.UC_MIPS_REG_8) & 0xffffffff


def run(args, artifacts):
    fixture = Path(__file__).with_name("dsp.S")
    report = {"complete": False, "passed": [], "failures": [],
              "fixture_sha256": digest(fixture), "neverd_sha256": digest(args.neverd)}
    artifacts.write("results.json", report)
    resource = subprocess.check_output([str(args.clang), "-print-resource-dir"], text=True).strip()
    headers = artifacts.work / "includes"
    headers.mkdir()
    (headers / "string.h").write_text("#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\nvoid *memset(void *, int, size_t);\n")
    command = lambda argv, log: roundtrip.run_command(artifacts, argv, log)
    for little in (False, True):
        profile = "mipsel" if little else "mips"
        flags = [f"--target={profile}-linux-gnu", "-march=mips32r2", "-mabi=32",
                 "-mdsp", "-mno-abicalls", "-fno-pic", "-msoft-float", "-G0",
                 "-ffreestanding", "-fno-strict-aliasing"]
        link = [args.ld, "-m", f"elf32{'l' if little else 'b'}tsmip", "-static"]
        original = artifacts.work / (profile + ".elf")
        obj = original.with_suffix(".o")
        log = original.with_suffix(".log")
        command([args.clang, *flags, "-c", fixture, "-o", obj], log)
        command([*link, "-Ttext", "0x20000", "-e", "dsp_read", obj, "-o", original], log)
        original_hash = digest(original)
        artifacts.discard(obj, log)
        for function in FUNCTIONS:
            for backend, optimization in product(("highc", "llvmc"), ("-O0", "-O2")):
                stem = artifacts.work / f"{profile}-{function}-{backend}-{optimization[1:]}"
                source, rebuilt, obj, log = (stem.with_suffix(ext) for ext in (".c", ".elf", ".o", ".log"))
                try:
                    command([args.neverd, "decompile", "--func", function,
                             *(["--llvm"] if backend == "llvmc" else []),
                             "-o", source, original], log)
                    if 'target("no-dsp,no-dspr2")' not in source.read_text():
                        raise RuntimeError("source does not constrain implicit DSP state changes")
                    command([args.clang, *flags, "-nostdinc", "-isystem", resource + "/include",
                             "-I", headers, optimization, "-c", source, "-o", obj], log)
                    command([*link, "-Ttext", "0x1020000", "--image-base=0x1000000",
                             "-e", function, obj, "-o", rebuilt], log)
                    observations = []
                    for initial in INITIAL_STATES:
                        for inputs in INPUTS:
                            wanted = expected(function, initial, inputs)
                            before = observe(original, little, function, initial, inputs)
                            after = observe(rebuilt, little, function, initial, inputs)
                            if after != wanted:
                                raise RuntimeError(f"{stem.name}: state={initial:#x}, input={inputs}, generated={after}, expected={wanted}")
                            limitation = before != wanted
                            if limitation and not (function in ("dsp_zero", "dsp_sub_zero") and before == (7, initial)
                                                   and wanted[1] == initial | 0x100000):
                                raise RuntimeError(f"{stem.name}: original={before}, expected={wanted}")
                            observations.append({"initial_dspcontrol": initial, "inputs": inputs,
                                                 "expected": wanted, "original": before, "generated": after,
                                                 "known_zero_destination_oracle_defect": limitation})
                    report["passed"].append({"profile": profile, "function": function, "backend": backend,
                                             "optimization": optimization,
                                             "original_sha256": original_hash, "source_sha256": digest(source),
                                             "rebuilt_sha256": digest(rebuilt), "observations": observations})
                    print("PASS", profile, function, backend, optimization, flush=True)
                except Exception as error:
                    artifacts.keep_source(source)
                    report["failures"].append({"case": stem.name, "error": str(error)})
                    artifacts.write("results.json", report)
                    raise
                artifacts.write("results.json", report)
                artifacts.discard(source, rebuilt, obj, log)
        artifacts.discard(original)
    report["complete"] = True
    report["known_oracle_disagreements"] = sum(
        observation["known_zero_destination_oracle_defect"]
        for group in report["passed"] for observation in group["observations"])
    artifacts.write("results.json", report)
    print(len(report["passed"]), "source groups passed;",
          report["known_oracle_disagreements"], "known reference disagreements recorded", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "mips-dsp-roundtrip",
                                 (args.neverd, args.clang, args.ld,
                                  Path(__file__).with_name("dsp.S"))) as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
