#!/usr/bin/env python3
"""Execute original and generated MSA scalar leaves on enabled P5600.

The oracle checks scalar results, every output-memory byte and the preserved
FP64 low views. It never uses Unicorn's unimplemented W-register API. Original
and generated ELF files are mapped into separate instances. Owned products are
retired after their readers; only manifests and failing source evidence remain.
"""
import argparse
import hashlib
import os
from itertools import product
from pathlib import Path
import random
import subprocess
import sys

import roundtrip
from unicorn import mips_const as mips

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts

VECTOR_FUNCTIONS = ("msa_add_b", "msa_add_h", "msa_add_d", "msa_sub_b", "msa_sub_h",
                    "msa_sub_w", "msa_sub_d", "msa_and", "msa_or", "msa_nor", "msa_xor") + tuple(
    f"msa_{operation}_{width}" for operation in (
        "ceq", "clt_s", "clt_u", "cle_s", "cle_u", "max_s", "max_u", "min_s", "min_u",
        "sll", "sra", "srl", "bclr", "bset", "bneg", "mul")
    for width in ("b", "h", "w", "d"))
FUNCTIONS = tuple(f"msa_lane{i}" for i in range(4)) + ("msa_mix", "msa_store", "msa_return_slot") + VECTOR_FUNCTIONS
INPUTS = [(0, 0, 0, 0), (0xffffffff, 0x7fffffff, 0x80000000, 1),
          (0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00), (1, 2, 3, 4)]
# The first D count is d | (b << 32); the first input has sign bit c[31].
INPUTS += [(0, 0xffffffff, 0x80000000, count)
           for count in (0, 63, 64, 65, 127)]
rng = random.Random(0x4d5341)
INPUTS += [tuple(rng.getrandbits(32) for _ in range(4)) for _ in range(12)]
SAVED = {index: (0x1234000000000000 | (index << 32) | (0x87654300 + index))
         for index in range(20, 32, 2)}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected(function, arguments, little):
    a, b, c, d = arguments
    memory = bytes(16)
    if function in VECTOR_FUNCTIONS:
        left = sum(v << (32 * i) for i, v in enumerate((b, c, d, b ^ d)))
        right = sum(v << (32 * i) for i, v in enumerate((d, b, c, c ^ d)))
        operation = function.removeprefix("msa_")
        if operation.startswith(("sll_", "sra_", "srl_", "bclr_", "bset_", "bneg_", "mul_")):
            bits = {"b": 8, "h": 16, "w": 32, "d": 64}[operation[-1]]
            mask, value = (1 << bits) - 1, 0
            for shift in range(0, 128, bits):
                x, y = (left >> shift) & mask, (right >> shift) & mask
                count = y & (bits - 1)
                signed = x - (1 << bits) if x & (1 << (bits - 1)) else x
                result = {"sll": x << count, "srl": x >> count, "sra": signed >> count,
                          "bclr": x & ~(1 << count), "bset": x | (1 << count),
                          "bneg": x ^ (1 << count), "mul": x * y}[operation[:-2]]
                value |= (result & mask) << shift
        elif operation.startswith(("add_", "sub_")):
            bits = {"b": 8, "h": 16, "w": 32, "d": 64}[operation[-1]]
            mask = (1 << bits) - 1
            value = 0
            for shift in range(0, 128, bits):
                x, y = (left >> shift) & mask, (right >> shift) & mask
                lane = x + y if operation.startswith("add_") else x - y
                value |= (lane & mask) << shift
        elif operation.startswith(("ceq_", "clt_", "cle_", "max_", "min_")):
            bits = {"b": 8, "h": 16, "w": 32, "d": 64}[operation[-1]]
            mask = (1 << bits) - 1
            value = 0
            for shift in range(0, 128, bits):
                x, y = (left >> shift) & mask, (right >> shift) & mask
                if "_s_" in operation:
                    x = x - (1 << bits) if x & (1 << (bits - 1)) else x
                    y = y - (1 << bits) if y & (1 << (bits - 1)) else y
                if operation.startswith(("max_", "min_")):
                    result = max(x, y) if operation.startswith("max_") else min(x, y)
                    value |= (result & mask) << shift
                else:
                    result = (x == y if operation.startswith("ceq") else
                              x < y if operation.startswith("clt") else x <= y)
                    value |= (mask if result else 0) << shift
        else:
            value = {"and": left & right, "or": left | right,
                     "nor": ~(left | right), "xor": left ^ right}[operation]
        memory = b"".join(((value >> (32 * i)) & 0xffffffff).to_bytes(
            4, "little" if little else "big") for i in range(4))
        result = 7
    elif function.startswith("msa_lane"):
        result = 2 * arguments[int(function[-1])]
    elif function == "msa_mix":
        result = (((a + b) & 0xffffffff) ^ ((c + b) & 0xffffffff))
        result = (result + a + d) & 0xffffffff
        result ^= (a + b) & 0xffffffff
    elif function == "msa_store":
        result = 7
        memory = b"".join(((2 * x) & 0xffffffff).to_bytes(4, "little" if little else "big")
                          for x in (b, c, d, 0))
    else:
        result = 7
    return result & 0xffffffff, memory.hex(), SAVED


def observe(path, function, arguments, little):
    uc, entry = roundtrip.emulator(path, 32, little, function,
                                  cpu_model=mips.UC_CPU_MIPS32_P5600)
    setup = (0x40088005, 0x3c090800, 0x01094025, 0x40888005, 0xc0,
             0x40086000, 0x3c092400, 0x01094025, 0x40886000, 0xc0)
    start = roundtrip.STOP + 0x200
    uc.mem_write(start, b"".join(w.to_bytes(4, "little" if little else "big") for w in setup))
    uc.emu_start(start, start + 4 * len(setup), count=len(setup))
    for index, value in SAVED.items():
        uc.reg_write(getattr(mips, f"UC_MIPS_REG_F{index}"), value)
    args = ((roundtrip.DATA, *arguments[1:]) if function == "msa_store" or function in VECTOR_FUNCTIONS else arguments)
    value, memory = roundtrip.invoke(uc, entry, 32, little, args)
    saved = {index: uc.reg_read(getattr(mips, f"UC_MIPS_REG_F{index}")) for index in SAVED}
    return value, memory, saved


def run(args, artifacts):
    fixture = Path(__file__).with_name("msa.S")
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
        flags = [f"--target={profile}-linux-gnu", "-march=mips32r5", "-mabi=32", "-mmsa",
                 "-mfp64", "-mhard-float", "-mno-abicalls", "-fno-pic", "-G0",
                 "-ffreestanding", "-fno-strict-aliasing"]
        link = [args.ld, "-m", f"elf32{'l' if little else 'b'}tsmip", "-static"]
        original = artifacts.work / (profile + ".elf")
        obj, log = original.with_suffix(".o"), original.with_suffix(".log")
        command([args.clang, *flags, "-c", fixture, "-o", obj], log)
        command([*link, "-Ttext", "0x20000", "-e", "msa_lane0", obj, "-o", original], log)
        original_hash = digest(original)
        artifacts.discard(obj, log)
        for function in FUNCTIONS:
            for backend, optimization in product(("highc", "llvmc"), ("-O0", "-O2")):
                stem = artifacts.work / f"{profile}-{function}-{backend}-{optimization[1:]}"
                source, rebuilt, obj, log = (stem.with_suffix(ext) for ext in (".c", ".elf", ".o", ".log"))
                try:
                    command([args.neverd, "decompile", "--func", function,
                             *(["--llvm"] if backend == "llvmc" else []), "-o", source, original], log)
                    command([args.clang, *flags, optimization, "-nostdinc", "-isystem", resource + "/include",
                             "-I", headers, "-c", source, "-o", obj], log)
                    command([*link, "-Ttext", "0x30000", "-e", function, obj, "-o", rebuilt], log)
                    observations = []
                    for inputs in INPUTS:
                        wanted = expected(function, inputs, little)
                        before = observe(original, function, inputs, little)
                        after = observe(rebuilt, function, inputs, little)
                        if before != wanted or after != wanted:
                            raise RuntimeError(f"{stem.name}: input={inputs}, original={before}, generated={after}, expected={wanted}")
                        observations.append({"inputs": inputs, "expected": wanted, "original": before, "generated": after})
                    text = source.read_text()
                    report["passed"].append({"profile": profile, "function": function, "backend": backend,
                                             "optimization": optimization, "source_sha256": digest(source),
                                             "original_sha256": original_hash, "rebuilt_sha256": digest(rebuilt),
                                             "wide_casts": text.count("(__uint128_t)") + text.count("(__int128)"),
                                             "observations": observations})
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
    log = artifacts.work / "wide-export.log"
    env = dict(os.environ, NEVERD_MIPS_CARRIER_OUTPUT=str(artifacts.work.resolve()))
    with artifacts.process([pipeline_tests, "--gtest_filter=MIPSMSAPipeline.LLVMCWide*"],
                           log, env=env) as (process, _):
        process.wait(timeout=60)
    if process.returncode:
        raise RuntimeError(log.read_text())
    artifacts.discard(log)
    report["wide_carriers"] = []
    for little in (False, True):
        profile = "mipsel" if little else "mips"
        source = artifacts.work / f"{profile}-wide.c"
        if "_BitInt(128)" not in source.read_text():
            raise RuntimeError("wide carrier fixture lost its 128-bit source type")
        for optimization in ("-O0", "-O2"):
            obj, rebuilt, log = (artifacts.work / (profile + "-wide" + ext)
                                 for ext in (".o", ".elf", ".log"))
            flags = [f"--target={profile}-linux-gnu", "-march=mips32r5", "-mabi=32", "-mmsa",
                     "-mfp64", "-mhard-float", "-mno-abicalls", "-fno-pic", "-G0",
                     "-ffreestanding", "-fno-strict-aliasing", optimization]
            try:
                command([args.clang, *flags, "-nostdinc", "-isystem", resource + "/include",
                         "-I", headers, "-c", source, "-o", obj], log)
                command([args.ld, "-m", f"elf32{'l' if little else 'b'}tsmip", "-static",
                         "-Ttext", "0x30000", "-e", "msa_i128", obj, "-o", rebuilt], log)
                observations = []
                for a, b, _, _ in INPUTS:
                    x = a | (b << 65)
                    y = x ^ 0x112233445566778899aabbccddeeff00
                    for shift in (0, 1, 31, 32, 63, 64, 65, 95, 96, 127):
                        value = (((x << 128) | y) >> shift) & 0xffffffff
                        actual = observe(rebuilt, "msa_i128", (a, b, shift), little)
                        wanted = value, "00" * 16, SAVED
                        if actual != wanted:
                            raise RuntimeError(f"wide carrier {profile}/{optimization}/{a,b,shift}: {actual} != {wanted}")
                        observations.append({"arguments": [a, b, shift], "expected": wanted, "actual": actual})
                report["wide_carriers"].append({"profile": profile, "optimization": optimization,
                                                "source_sha256": digest(source), "observations": observations})
                artifacts.write("results.json", report)
            except Exception:
                artifacts.keep_source(source)
                raise
            artifacts.discard(obj, rebuilt, log)
        artifacts.discard(source)
    report["wide_storage_syntax"] = []
    for profile, kind in product(("mips", "mipsel"), ("storage", "declaration")):
        source = artifacts.work / f"{profile}-{kind}.c"
        log = source.with_suffix(".log")
        try:
            command([args.clang, f"--target={profile}-linux-gnu", "-march=mips32r5",
                     "-mabi=32", "-mfp64", "-fsyntax-only", "-nostdinc", "-isystem",
                     resource + "/include", "-I", headers, source], log)
            report["wide_storage_syntax"].append({"profile": profile, "kind": kind,
                                                   "source_sha256": digest(source)})
        except Exception:
            artifacts.keep_source(source)
            raise
        artifacts.discard(source, log)
    report["complete"] = True
    artifacts.write("results.json", report)
    print(len(report["passed"]), "MSA source groups,", len(report["wide_carriers"]),
          "live i128 groups and", len(report["wide_storage_syntax"]), "storage/declaration checks passed",
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "mips-msa-roundtrip",
                                 (args.neverd, args.clang, args.ld, Path(__file__).with_name("msa.S"),
                                  args.neverd.with_name("NeverDMIPSPipelineTests"))) as artifacts:
        run(args, artifacts)


if __name__ == "__main__":
    main()
