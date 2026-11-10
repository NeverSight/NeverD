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
from unicorn.unicorn_py3.unicorn import uclib

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts

VECTOR_FUNCTIONS = ("msa_add_b", "msa_add_h", "msa_add_d", "msa_sub_b", "msa_sub_h",
                    "msa_sub_w", "msa_sub_d", "msa_and", "msa_or", "msa_nor", "msa_xor") + tuple(
    f"msa_{operation}_{width}" for operation in (
        "ceq", "clt_s", "clt_u", "cle_s", "cle_u", "max_s", "max_u", "min_s", "min_u",
        "sll", "sra", "srl", "bclr", "bset", "bneg", "mul", "madd", "msub", "max_a", "min_a",
        "add_a", "adds_a", "adds_s", "adds_u", "subs_s", "subs_u", "ave_s", "ave_u", "aver_s", "aver_u")
    for width in ("b", "h", "w", "d"))
VECTOR_FUNCTIONS += tuple(f"msa_{op}_same_{w}" for op in ("add_a", "adds_a") for w in ("b", "h", "w", "d"))
VECTOR_FUNCTIONS += ('msa_andi_b', 'msa_ori_b', 'msa_nori_b', 'msa_xori_b', 'msa_bsel', 'msa_bmnz', 'msa_bmz', 'msa_pcnt_b', 'msa_pcnt_h', 'msa_pcnt_w', 'msa_pcnt_d', 'msa_nlzc_b', 'msa_nlzc_h', 'msa_nlzc_w', 'msa_nlzc_d', 'msa_nloc_b', 'msa_nloc_h', 'msa_nloc_w', 'msa_nloc_d', 'msa_slli_b', 'msa_slli_h', 'msa_slli_w', 'msa_slli_d', 'msa_srli_b', 'msa_srli_h', 'msa_srli_w', 'msa_srli_d', 'msa_srai_b', 'msa_srai_h', 'msa_srai_w', 'msa_srai_d', 'msa_bclri_b', 'msa_bclri_h', 'msa_bclri_w', 'msa_bclri_d', 'msa_bseti_b', 'msa_bseti_h', 'msa_bseti_w', 'msa_bseti_d', 'msa_bnegi_b', 'msa_bnegi_h', 'msa_bnegi_w', 'msa_bnegi_d')
VECTOR_FUNCTIONS += tuple(f"msa_{op}_{w}" for op in ("asub_s", "asub_u") for w in ("b", "h", "w", "d"))
VECTOR_FUNCTIONS += tuple(f"msa_{op}_{m}_{w}" for op in ("sat_s", "sat_u") for w, bits in zip(("b", "h", "w", "d"), (8, 16, 32, 64)) for m in (0, bits//2, bits-1))
FUNCTIONS = tuple(f"msa_lane{i}" for i in range(4)) + ("msa_mix", "msa_store", "msa_return_slot") + VECTOR_FUNCTIONS
INPUTS = [(0, 0, 0, 0), (0xffffffff, 0x7fffffff, 0x80000000, 1),
          (0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00), (1, 2, 3, 4), (0, 1, 0xffffffff, 0), (0, 0x80000000, 0, 0x80000000)]
# The first D count is d | (b << 32); the first input has sign bit c[31].
INPUTS += [(0, 0xffffffff, 0x80000000, count)
           for count in (0, 63, 64, 65, 127)]
INPUTS += [(0, 0, 0x80000000, 0), (0, 0x80808080, 0x80008000, 0x80000000)]
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
        if "_same_" in operation:
            right = left
            operation = operation.replace("_same_", "_")
        if operation in ("andi_b", "ori_b", "nori_b", "xori_b"):
            immediate = int.from_bytes(bytes([0xa5]) * 16, "little")
            value = left & immediate if operation == "andi_b" else left | immediate if operation == "ori_b" else ~(left | immediate) if operation == "nori_b" else left ^ immediate
            value &= (1 << 128) - 1
        elif operation in ("bsel", "bmnz", "bmz"):
            old = int.from_bytes(bytes([0x5a]) * 16, "little")
            value = ((right & old) | (left & ~old)) if operation == "bsel" else ((left & right) | (old & ~right)) if operation == "bmnz" else ((left & ~right) | (old & right))
        elif operation.startswith(("pcnt_", "nlzc_", "nloc_", "slli_", "srli_", "srai_", "bclri_", "bseti_", "bnegi_")):
            bits = {"b": 8, "h": 16, "w": 32, "d": 64}[operation[-1]]
            mask, value = (1 << bits) - 1, 0
            op = operation[:-2]
            for shift in range(0, 128, bits):
                x = (left >> shift) & mask
                signed = x - (1 << bits) if x & (1 << (bits - 1)) else x
                count = bits - 1
                if op == "pcnt": lane = x.bit_count()
                elif op == "nlzc": lane = bits - x.bit_length()
                elif op == "nloc": lane = bits - (x ^ mask).bit_length()
                elif op == "slli": lane = x << count
                elif op == "srli": lane = x >> count
                elif op == "srai": lane = signed >> count
                elif op == "bclri": lane = x & ~(1 << count)
                elif op == "bseti": lane = x | (1 << count)
                else: lane = x ^ (1 << count)
                value |= (lane & mask) << shift
        elif operation.startswith(("asub_", "sat_")):
            bits = {"b": 8, "h": 16, "w": 32, "d": 64}[operation[-1]]
            mask, value = (1 << bits) - 1, 0
            for shift in range(0, 128, bits):
                x, y = (left >> shift) & mask, (right >> shift) & mask
                if "_s_" in operation:
                    if x & (1 << (bits-1)): x -= 1 << bits
                    if y & (1 << (bits-1)): y -= 1 << bits
                if operation.startswith("asub_"): result = abs(x - y)
                else:
                    width = int(operation.split("_")[2]) + 1
                    low, high = (-(1 << (width-1)), (1 << (width-1))-1) if "_s_" in operation else (0, (1 << width)-1)
                    result = max(low, min(high, x))
                value |= (result & mask) << shift
        elif operation.startswith(("add_a_", "adds_", "subs_", "ave_", "aver_")):
            bits = {"b": 8, "h": 16, "w": 32, "d": 64}[operation[-1]]
            mask, value = (1 << bits) - 1, 0
            minimum, maximum = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
            for shift in range(0, 128, bits):
                x, y = (left >> shift) & mask, (right >> shift) & mask
                sx = x - (1 << bits) if x > maximum else x
                sy = y - (1 << bits) if y > maximum else y
                op = operation[:-2]
                if op in ("add_a", "adds_a"):
                    lane = abs(sx) + abs(sy)
                    if op == "adds_a": lane = min(maximum, lane)
                elif op in ("adds_s", "subs_s"):
                    lane = max(minimum, min(maximum, sx + sy if op == "adds_s" else sx - sy))
                elif op in ("adds_u", "subs_u"):
                    lane = max(0, min(mask, x + y if op == "adds_u" else x - y))
                else:
                    lane = ((sx + sy if op.endswith("_s") else x + y) + int(op.startswith("aver"))) // 2
                value |= (lane & mask) << shift
        elif operation.startswith(("sll_", "sra_", "srl_", "bclr_", "bset_", "bneg_", "mul_", "madd_", "msub_")):
            bits = {"b": 8, "h": 16, "w": 32, "d": 64}[operation[-1]]
            mask, value = (1 << bits) - 1, 0
            for shift in range(0, 128, bits):
                x, y = (left >> shift) & mask, (right >> shift) & mask
                count = y & (bits - 1)
                signed = x - (1 << bits) if x & (1 << (bits - 1)) else x
                result = {"sll": x << count, "srl": x >> count, "sra": signed >> count,
                          "bclr": x & ~(1 << count), "bset": x | (1 << count),
                          "bneg": x ^ (1 << count), "mul": x * y, "madd": x + x * y, "msub": x - x * y}[operation[:-2]]
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
                if "_s_" in operation or "_a_" in operation:
                    x = x - (1 << bits) if x & (1 << (bits - 1)) else x
                    y = y - (1 << bits) if y & (1 << (bits - 1)) else y
                if operation.startswith(("max_", "min_")):
                    if "_a_" in operation:
                        choose_left = abs(x) > abs(y) if operation.startswith("max_") else abs(x) < abs(y)
                        result = x if choose_left else y
                    else:
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
