#!/usr/bin/env python3
"""Qualify MIPS DSP source against independent state and target execution.

Use the repository-pinned Unicorn bindings/native library, clang and lld.
DSPControl is initialized and observed with machine instructions, including
after the generated function returns. No vendor binary executes here.
"""

import argparse
import hashlib
import io
import os
from itertools import product
from pathlib import Path
import subprocess
import sys

import roundtrip
from unicorn import mips_const as mips
from unicorn import UC_HOOK_CODE
from unicorn.unicorn_py3.unicorn import uclib
from elftools.elf.elffile import ELFFile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts


FUNCTIONS = ("dsp_read", "dsp_write", "dsp_partial", "dsp_add",
             "dsp_saturate", "dsp_zero", "dsp_read_then_write",
             "dsp_return_slot", "dsp_plain_add", "dsp_sub",
             "dsp_sub_saturate", "dsp_sub_zero", "dsp_half_add",
             "dsp_half_add_saturate", "dsp_half_sub", "dsp_half_sub_saturate",
             "dsp_shrl", "dsp_shrlv", "dsp_shra", "dsp_shrav", "dsp_shra_r",
             "dsp_shrav_r", "dsp_shra_rw", "dsp_shrav_rw", "dsp_pack_h", "dsp_pack_b",
             "dsp_bitrev", "dsp_raddu", "dsp_repl_qb", "dsp_repl_ph", "dsp_replv_qb", "dsp_replv_ph", "dsp_preceq_phl", "dsp_preceq_phr", "dsp_precequ_qbl", "dsp_precequ_qbr", "dsp_precequ_qbla", "dsp_precequ_qbra", "dsp_preceu_qbl", "dsp_preceu_qbr", "dsp_preceu_qbla", "dsp_preceu_qbra", "dsp_addq_w", "dsp_subq_w", "dsp_addsc", "dsp_addwc", "dsp_modsub", "dsp_cmpeq", "dsp_cmplt", "dsp_cmple", "dsp_pick_b", "dsp_pick_h")
INITIAL_STATES = (0, 0x01100021, 0x0fff7fbf)
INPUTS = ((0, 0), (0xff01807f, 0x02018081), (0x11223344, 0x55667788),
          (0xffffffff, 1), (0x80808080, 0x80808080),
          (0x7fff8000, 0x0001ffff), (0x80007fff, 0x7fff8000),
          (0x80008000, 0x80008000), (0x7fffffff, 1), (0xfffffffd, 1),
          (0x7fff7fff, 1), (0x80000000, 31), (0x80008000, 15))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected(function, initial, arguments):
    a, b = arguments
    if function in ("dsp_addq_w", "dsp_subq_w", "dsp_addsc", "dsp_addwc"):
        sa, sb = (v - 0x100000000 if v & 0x80000000 else v for v in (a, b))
        if function == "dsp_addsc":
            return (a + b) & 0xffffffff, (initial & ~0x2000) | (((a + b) >> 32) << 13)
        total = sa - sb if function == "dsp_subq_w" else sa + sb
        if function == "dsp_addwc": total += (initial >> 13) & 1
        state = initial | (0x100000 if total < -0x80000000 or total > 0x7fffffff else 0)
        if function != "dsp_addwc": total = max(-0x80000000, min(total, 0x7fffffff))
        return total & 0xffffffff, state
    if function == "dsp_modsub":
        return (((b >> 8) & 65535) if a == 0 else (a - (b & 255)) & 0xffffffff), initial
    if function.startswith("dsp_cmp"):
        predicates = 0
        for lane in range(4):
            x, y = (a >> (8 * lane)) & 255, (b >> (8 * lane)) & 255
            match = x == y if function == "dsp_cmpeq" else x < y if function == "dsp_cmplt" else x <= y
            predicates |= int(match) << (24 + lane)
        state = (initial & ~0x0f000000) | predicates
        return state, state
    if function.startswith("dsp_pick"):
        bits = 8 if function.endswith("_b") else 16
        value = 0
        for lane in range(32 // bits):
            selected = a if initial & (1 << (24 + lane)) else b
            value |= ((selected >> (bits * lane)) & ((1 << bits) - 1)) << (bits * lane)
        return value, initial
    if function.startswith("dsp_prece"):
        if function == "dsp_preceq_phl":
            return a & 0xffff0000, initial
        if function == "dsp_preceq_phr":
            return (a << 16) & 0xffffffff, initial
        offsets = {"qbl": (3, 2), "qbr": (1, 0), "qbla": (3, 1), "qbra": (2, 0)}[function.split("_")[-1]]
        high, low = ((a >> (8 * n)) & 255 for n in offsets)
        shift = 7 if function.startswith("dsp_precequ") else 0
        return ((high << 16) | low) << shift, initial
    if function == "dsp_bitrev":
        return int(f"{a & 65535:016b}"[::-1], 2), initial
    if function == "dsp_raddu":
        return sum(a.to_bytes(4, "little")), initial
    if function.startswith("dsp_repl"):
        value = 0xa5 if function == "dsp_repl_qb" else 0xfe00 if function == "dsp_repl_ph" else a
        width = 8 if function.endswith("qb") else 16
        return sum((value & ((1 << width) - 1)) << n for n in range(0, 32, width)), initial
    if function.startswith("dsp_pack"):
        value = ((a & 0xffff0000) | (b >> 16)) if function.endswith("_h") else (
            ((a >> 24) << 24) | (((a >> 8) & 255) << 16) | ((b >> 24) << 8) | ((b >> 8) & 255))
        return value, initial
    if function.startswith("dsp_shr"):
        bits = 8 if "shrl" in function else 32 if function.endswith("rw") else 16
        variable = "shrav" in function or "shrlv" in function
        amount = ((b & (bits - 1)) if variable else
                  7 if bits == 8 else 1 if "_r" in function else 15)
        value = 0
        for shift in range(0, 32, bits):
            lane = (a >> shift) & ((1 << bits) - 1)
            if bits != 8 and lane & (1 << (bits - 1)):
                lane -= 1 << bits
            if "_r" in function and amount:
                lane += 1 << (amount - 1)
            value |= ((lane >> amount) & ((1 << bits) - 1)) << shift
        return value, initial
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
    called = []
    if function == "dsp_standalone_wide":
        elf = ELFFile(io.BytesIO(path.read_bytes()))
        target = elf.get_section_by_name(".symtab").get_symbol_by_name("dsp_wide_helper")[0]["st_value"]
        uc.hook_add(UC_HOOK_CODE, lambda _uc, pc, _size, _: called.append(pc) if pc == target else None)
    saved = {index: 0xa5000000 | (index << 16) | (index * 0x101) for index in (*range(16, 24), 30)}
    value, memory = roundtrip.invoke(uc, entry, 32, little, arguments,
                                    initial_registers=saved)
    if uc.reg_read(mips.UC_MIPS_REG_SP) != roundtrip.STACK or any(
            uc.reg_read(getattr(mips, f"UC_MIPS_REG_{index}")) & 0xffffffff != wanted for index, wanted in saved.items()):
        raise RuntimeError("DSP source changed caller stack/saved registers")
    if function == "dsp_standalone_wide":
        if called != [target]:
            raise RuntimeError("standalone caller did not execute its non-inlined helper")
        other = uc.reg_read(mips.UC_MIPS_REG_V1) & 0xffffffff
        value = value | (other << 32) if little else (value << 32) | other
    if memory != "00" * 16:
        raise RuntimeError("DSP leaf unexpectedly changed memory")
    start = roundtrip.STOP + 0x100
    uc.mem_write(start, (0x7c3f44b8).to_bytes(4, order))  # RDDSP t0,63
    uc.emu_start(start, start + 4, count=1)
    return value, uc.reg_read(mips.UC_MIPS_REG_8) & 0xffffffff


def run(args, artifacts):
    fixture = Path(__file__).with_name("dsp.S")
    report = {"complete": False, "passed": [], "failures": [],
              "fixture_sha256": digest(fixture), "neverd_sha256": digest(args.neverd),
              "reference_source_revision": subprocess.check_output(["git", "-C", str(Path(__file__).resolve().parents[3] / "third_party/unicorn"), "rev-parse", "HEAD"], text=True).strip(),
              "reference_library_sha256": digest(Path(uclib._name))}
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
    pipeline_tests = args.neverd.with_name("NeverDMIPSPipelineTests")
    log = artifacts.work / "standalone-export.log"
    env = dict(os.environ, NEVERD_MIPS_DSP_SOURCE_OUTPUT=str(artifacts.work.resolve()))
    with artifacts.process([pipeline_tests, "--gtest_filter=MIPSDSPPipeline.PureStandaloneSource*:MIPSDSPPipeline.StandaloneWideHelpers*"],
                           log, env=env) as (process, _):
        process.wait(timeout=60)
    if process.returncode:
        raise RuntimeError(log.read_text())
    artifacts.discard(log)
    report["standalone"] = []
    for little, backend, kind in product((False, True), ("highc", "llvmc"), ("pure", "wide")):
        profile = "mipsel" if little else "mips"
        source = artifacts.work / f"{profile}-{backend}-{kind}.c"
        function = "dsp_pure_round" if kind == "pure" else "dsp_standalone_wide"
        for optimization in ("-O0", "-O2"):
            obj, rebuilt, log = (source.with_suffix(ext) for ext in (".o", ".elf", ".log"))
            flags = [f"--target={profile}-linux-gnu", "-march=mips32r2", "-mabi=32", "-mdsp",
                     "-mno-abicalls", "-fno-pic", "-msoft-float", "-G0", "-ffreestanding",
                     "-fno-inline", "-fno-optimize-sibling-calls"]
            try:
                command([args.clang, *flags, optimization, "-nostdinc", "-isystem", resource + "/include",
                         "-I", headers, "-c", source, "-o", obj], log)
                command([args.ld, "-m", f"elf32{'l' if little else 'b'}tsmip", "-static", "-Ttext", "0x20000",
                         "-e", function, obj, "-o", rebuilt], log)
                observations = []
                for initial, inputs in product(INITIAL_STATES, INPUTS):
                    wanted = (expected("dsp_shrav_rw", initial, inputs) if kind == "pure" else
                              ((((inputs[0] << 32) | inputs[1]) + 0xffffffff) & ((1 << 64) - 1), initial))
                    actual = observe(rebuilt, little, function, initial, inputs)
                    if actual != wanted:
                        raise RuntimeError(f"{source.name}/{optimization}: {inputs}/{initial}: {actual} != {wanted}")
                    observations.append({"initial": initial, "inputs": inputs, "expected": wanted, "actual": actual})
                report["standalone"].append({"profile": profile, "backend": backend, "kind": kind, "optimization": optimization,
                                             "source_sha256": digest(source), "rebuilt_sha256": digest(rebuilt), "observations": observations})
                artifacts.write("results.json", report)
            except Exception:
                artifacts.keep_source(source)
                raise
            artifacts.discard(obj, rebuilt, log)
        artifacts.discard(source)
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
                                  Path(__file__).with_name("dsp.S"),
                                  args.neverd.with_name("NeverDMIPSPipelineTests"))) as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
