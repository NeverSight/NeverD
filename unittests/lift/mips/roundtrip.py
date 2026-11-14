#!/usr/bin/env python3
"""Compare owned MIPS kernels with recompiled NeverD HighC and LLVMC.

Requires clang/lld with MIPS targets, pyelftools, and the repository's pinned
Unicorn Python bindings/native library (PYTHONPATH and LIBUNICORN_PATH). No
vendor executable is run. Generated source is compiled for its original target,
including byte order, instead of being retargeted to the host.
"""

import argparse
import hashlib
import io
import json
import random
import re
import subprocess
import sys
from functools import partial
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UcError, UC_ARCH_MIPS, UC_ERR_READ_UNMAPPED, UC_ERR_EXCEPTION,
                     UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_MODE_BIG_ENDIAN,
                     UC_MODE_MIPS32, UC_MODE_MIPS64)
from unicorn import mips_const as mips

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from mips_validation_artifacts import MIPSValidationArtifacts


PROFILES = [("mips", 32, False), ("mipsel", 32, True),
            ("mips64", 64, False), ("mips64el", 64, True)]
MICRO_FUNCTIONS = ["mips_add", "mips_load", "mips_store", "mips_stack", "mips_branch_capture",
                   "mips_merge_word", "mips_merge_store_word",
                   "mips_div_lo", "mips_div_hi", "mips_divu_lo", "mips_divu_hi"]
KERNEL_FORMS = Path(__file__).with_name("MIPSBitfieldKernels.def").read_text()
FIELDS = {name: tuple(map(int, (width, insert, pos, size)))
          for name, width, insert, pos, size in re.findall(
              r"MIPS_FIELD_KERNEL\((\w+),\s*\w+,\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)",
              KERNEL_FORMS)}
SHUFFLES = {name: tuple(map(int, (width, lane, permutation)))
            for name, width, lane, permutation in re.findall(
                r"MIPS_SHUFFLE_KERNEL\((\w+),\s*\w+,\s*(\d+),\s*(\d+),\s*(\d+)\)",
                KERNEL_FORMS)}
DATA, STACK, STOP = 0x600000, 0x702000, 0x900000
FUNCTIONS = ["mips_add", "mips_likely", "mips_load", "mips_store", "mips_stack",
             "mips_branch_capture", "mips_likely_store", "mips_call_slot",
             "mips_ro_global", "mips_rw_global", "mips_jump_slot_load",
             "mips_jalr_slot", "mips_jalr_memory", "mips_conditional_link",
             "mips_data_pointer", "mips_code_pointer", "mips_mul",
             "mips_merge_word", "mips_merge_word_reverse", "mips_merge_full", "mips_merge_store_word",
             "mips_jalr_merge_slot", "mips_merge_ro_global", "mips_merge_rw_global",
             "mips_merge_discard", "mips_merge_zero", "mips_likely_load",
             "mips_uninitialized_word", "mips_uninitialized_word_reverse",
             "mips_uninitialized_full", "mips_uninitialized_full_reverse"]


FUNCTIONS += ["mips_div_lo", "mips_div_hi", "mips_divu_lo", "mips_divu_hi", "mips_div_dead"]
FUNCTIONS += ["mips_store_byte", "mips_store_half"]
DOUBLEWORD_STORES = ("mips_merge_store_left_double", "mips_merge_store_right_double",
                     "mips_merge_store_double")
DOUBLEWORD_DIVISIONS = ("mips_ddiv_lo", "mips_ddiv_hi", "mips_ddivu_lo", "mips_ddivu_hi")
DOUBLEWORD_KERNELS = DOUBLEWORD_STORES + DOUBLEWORD_DIVISIONS + ("mips_ddiv_dead",)
FUNCTIONS += list(DOUBLEWORD_KERNELS)
FUNCTIONS += list(FIELDS) + list(SHUFFLES) + ["mips_word_likely", "mips_word_zero"]


def word_value(value):
    value &= 0xffffffff
    return value - (1 << 32) if value & (1 << 31) else value


def independent_result(function, inputs):
    if function in ("mips_store_byte", "mips_store_half"):
        return inputs[1] & (0xff if function.endswith("_byte") else 0xffff)
    if function in ("mips_div_dead", "mips_ddiv_dead"):
        return 7
    if function.startswith("mips_div") or function in DOUBLEWORD_DIVISIONS:
        wide = function in DOUBLEWORD_DIVISIONS
        bits = 64 if wide else 32
        x, y = inputs
        if "divu" in function:
            x, y = (v & ((1 << bits) - 1) for v in inputs)
        q = abs(x) // abs(y)
        if (x < 0) != (y < 0):
            q = -q
        result = x - q * y if function.endswith("_hi") else q
        return result if wide else word_value(result)
    if function in FIELDS:
        width, insert, pos, size = FIELDS[function]
        source, old = inputs[0], inputs[1] if insert else 0
        result = 0
        for bit in range(width):
            if insert:
                value = (source >> (bit - pos)) if pos <= bit < pos + size else old >> bit
            else:
                value = source >> (bit + pos) if bit < size else 0
            result |= (value & 1) << bit
        return word_value(result) if width == 32 else result
    if function in SHUFFLES:
        width, lane, permutation = SHUFFLES[function]
        source = inputs[0] & ((1 << width) - 1)
        if not permutation:
            return int.from_bytes((source & ((1 << (lane * 8)) - 1)).to_bytes(lane, "little"),
                                  "little", signed=True)
        source_bytes = source.to_bytes(width // 8, "little")
        if permutation == 1:
            result_bytes = b"".join(source_bytes[i:i + 2][::-1]
                                    for i in range(0, len(source_bytes), 2))
        else:
            result_bytes = b"".join(source_bytes[i:i + 2]
                                    for i in reversed(range(0, len(source_bytes), 2)))
        result = int.from_bytes(result_bytes, "little")
        return word_value(result) if width == 32 else result
    if function == "mips_word_likely":
        return word_value(inputs[0] + 1) if inputs[1] == 0 else 7
    if function == "mips_word_zero":
        return 7
    return None


def run_command(artifacts, args, log):
    with artifacts.process(args, log) as (process, _):
        process.wait(timeout=60)
    if process.returncode:
        raise RuntimeError(f"{args[0]} exited {process.returncode}: {log.read_text()[-8192:]}")


def emulator(path, bits, little, function, original=None, cpu_model=None):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    uc = Uc(UC_ARCH_MIPS, (UC_MODE_MIPS64 if bits == 64 else UC_MODE_MIPS32)
            | (0 if little else UC_MODE_BIG_ENDIAN))
    micro = bool(elf["e_flags"] & 0x02000000)
    uc.ctl_set_cpu_model(cpu_model if cpu_model is not None else
                        mips.UC_CPU_MIPS64_MIPS64R2_GENERIC if bits == 64 else
                        mips.UC_CPU_MIPS32_M14KC if micro else
                        mips.UC_CPU_MIPS32_24KC)
    pages = set()
    images = [ELFFile(io.BytesIO(original.read_bytes())), elf] if original else [elf]
    for image in images:
        for segment in image.iter_segments():
            if segment["p_type"] != "PT_LOAD":
                continue
            begin, size = segment["p_vaddr"], segment["p_memsz"]
            for page in range(begin & ~4095, (begin + size + 4095) & ~4095, 4096):
                if page not in pages:
                    uc.mem_map(page, 4096)
                    pages.add(page)
            uc.mem_write(begin, segment.data())
    uc.mem_map(DATA, 4096)
    uc.mem_map(STACK - 8192, 16384)
    uc.mem_map(STOP, 4096)
    if isinstance(function, int):  # Explicit entry in a sectionless ELF.
        return uc, function
    symbols = elf.get_section_by_name(".symtab").get_symbol_by_name(function)
    if not symbols:
        raise RuntimeError(f"missing {function} in {path}")
    # pyelftools represents st_other bits 7:5 as `local`; bit 7 is the
    # microMIPS symbol mode even when st_value itself is an even instruction VA.
    entry = symbols[0]["st_value"] | bool(symbols[0]["st_other"]["local"] & 4)
    return uc, entry


def invoke(uc, entry, bits, little, arguments, old_destination=0, initial_registers=None):
    width, registers = bits // 8, (8 if bits == 64 else 4)
    mask = (1 << bits) - 1
    for reg in range(1, 32):
        uc.reg_write(getattr(mips, f"UC_MIPS_REG_{reg}"), 0)
    for reg, value in (initial_registers or {}).items():
        uc.reg_write(getattr(mips, f"UC_MIPS_REG_{reg}"), value & mask)
    uc.reg_write(mips.UC_MIPS_REG_SP, STACK)
    uc.reg_write(mips.UC_MIPS_REG_RA, STOP)
    uc.reg_write(mips.UC_MIPS_REG_V0, old_destination & mask)
    for index, value in enumerate(arguments):
        value &= mask
        if index < registers:
            uc.reg_write(getattr(mips, f"UC_MIPS_REG_{4 + index}"), value)
        else:
            slot = STACK + (0 if bits == 64 else 16) + (index - registers) * width
            uc.mem_write(slot, value.to_bytes(width, "little" if little else "big"))
    uc.emu_start(entry, STOP, count=100000)
    if uc.reg_read(mips.UC_MIPS_REG_PC) != STOP:
        raise RuntimeError("kernel did not return within the instruction bound")
    return uc.reg_read(mips.UC_MIPS_REG_V0) & mask, bytes(uc.mem_read(DATA, 16)).hex()


def cases(function, bits):
    if function in DOUBLEWORD_DIVISIONS:
        limit = 1 << 63
        rng = random.Random(0xDD17)
        return [[0, -1], [-10, 3], [10, -3], [-10, -3], [-limit, -1],
                [-limit, 3], [-1, -limit], [limit - 1, -limit],
                [0x100000002, 0x100000001], [-1, 1]] + [
                    [rng.randrange(-limit, limit), rng.randrange(-limit, limit) or 1]
                    for _ in range(24)]
    if function in ("mips_div_dead", "mips_ddiv_dead"):
        return [[0, 0], [7, 0], [-1, 0]]
    if function.startswith("mips_div"):
        return [[10, 3], [-10, 3], [10, -3], [-10, -3], [-2147483648, -1],
                [-2147483648, 3], [-1, -2147483648], [0, 7], [-1, 1]]
    if function in FIELDS or function in SHUFFLES:
        width = (FIELDS.get(function) or SHUFFLES[function])[0]
        values = [0, -1, 0x00800000, 0x80000000, 0x0123456789abcdef,
                  0x807fff0180fe0180, 0x8000000000000000, 0x7fffffffffffffff]
        if width == 32:
            values = [word_value(value) for value in values]
        return [[value, values[(i + 1) % len(values)]]
                if function in FIELDS and FIELDS[function][1] else [value]
                for i, value in enumerate(values)]
    if function == "mips_word_likely":
        return [[0, 0], [-1, 0], [2147483647, 0], [0x100000001, 1]]
    if function == "mips_word_zero":
        return [[0], [-1], [-2147483648], [2147483647]]
    if function.startswith("mips_uninitialized_"):
        return [[DATA + offset] for offset in range(8)]
    if function == "mips_likely_load":
        return [[DATA, 0], [DATA, 1], [DATA + 4096, 1]]
    if function in ("mips_merge_discard", "mips_merge_zero"):
        return [[DATA]]
    if function in ("mips_merge_ro_global", "mips_merge_rw_global"):
        return [[]]
    if function == "mips_jalr_merge_slot":
        return [[7, 11, DATA], [-1, 1, DATA], [0x12345678, 9, DATA]]
    if function == "mips_mul":
        return [[0, 0], [7, 11], [-1, 1], [-2147483648, 19],
                [2147483647, 2147483647], [-2147483647, -1]]
    if function.startswith("mips_merge_"):
        return [[DATA + offset, old] for offset in range(8)
                for old in (0, 0x12345678ABCDEF98, -1)]
    if function == "mips_jalr_memory" and bits == 64:
        return [[0, 0], [7, 11], [-1, 1], [-2147483648, 19],
                [0x1234567800000001, 9], [0xFFFFFFFF, 1]]
    if function in ("mips_add", "mips_jalr_slot", "mips_jalr_memory", "mips_code_pointer"):
        return [[0, 0], [7, 11], [-1, 1], [-2147483648, 19]]
    if function == "mips_likely":
        return [[0, 10], [1, 10], [0, -2]]
    if function in ("mips_branch_capture", "mips_conditional_link"):
        return [[0], [1], [-1]]
    if function in ("mips_load",):
        return [[DATA]]
    if function == "mips_store":
        return [[DATA, 0x55667788], [DATA, -1]]
    if function in ("mips_store_byte", "mips_store_half"):
        return [[DATA, value] for value in (0, -1, 0xff01807f, 0x1122334455667788,
                                            0xfedcba987654ffff)]
    if function == "mips_likely_store":
        return [[DATA, 0, 0x55667788], [DATA, 1, 0x55667788]]
    if function == "mips_stack":
        return [list(range(1, 10 if bits == 64 else 6))]
    if function == "mips_call_slot":
        return [[7], [-1]]
    if function == "mips_jump_slot_load":
        return [[DATA, 0], [DATA, 2]]
    if function == "mips_rw_global":
        return [[0, 0x55667788]]
    return [[]]


def fault_observation(uc, entry, bits, little, inputs, callees, prefix_read=None):
    entered = []
    data_reads = []
    uc.hook_add(UC_HOOK_CODE,
                lambda _, address, _size, _data: entered.append(address)
                if address in callees else None)
    if prefix_read is not None:
        uc.hook_add(UC_HOOK_MEM_READ,
                    lambda _, _access, address, _size, _value, _data:
                    data_reads.append(address) if DATA <= address < DATA + 4096 else None)
    try:
        invoke(uc, entry, bits, little, inputs)
    except UcError as error:
        if error.errno != UC_ERR_READ_UNMAPPED or entered:
            raise RuntimeError(f"expected read fault before callee: {error}, entered={entered}") from error
        result = {"error": error.errno, "callee_entered": False}
        if prefix_read is not None:
            if bool(data_reads) != prefix_read:
                raise RuntimeError(f"wrong fault ordering: preceding data reads={data_reads}") from error
            result["read_valid_data_before_fault"] = bool(data_reads)
        return result
    raise RuntimeError("required memory read did not fault")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--ld", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--function", action="append", choices=FUNCTIONS)
    parser.add_argument("--encoding", choices=("classic", "micro"), default="classic")
    args = parser.parse_args()
    if args.encoding == "micro" and any(f not in MICRO_FUNCTIONS for f in args.function or []):
        parser.error("selected function has no microMIPS fixture")
    with MIPSValidationArtifacts(args.output, "mips-roundtrip", (args.neverd, args.clang, args.ld)) as artifacts:
        result = run(args, artifacts)
        if result:
            raise SystemExit(result)
    return 0


def run(args, artifacts):
    output = artifacts.work
    artifacts.write("results.json", {"passed": [], "failures": [], "complete": False})
    command = partial(run_command, artifacts)
    headers = output / "includes"
    headers.mkdir(exist_ok=True)
    (headers / "string.h").write_text(
        "#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n"
        "void *memset(void *, int, size_t);\n")
    resource = subprocess.check_output([str(args.clang), "-print-resource-dir"],
                                       text=True).strip()
    micro = args.encoding == "micro"
    fixture = Path(__file__).with_name("micromips.S" if micro else "integer.S")
    profiles = [p for p in PROFILES if not micro or p[1] == 32]
    tasks = []
    for profile, bits, little in profiles:
        triple = profile + "-linux-gnu" + ("abi64" if bits == 64 else "")
        flags = [f"--target={triple}", f"-march=mips{bits}r2",
                 f"-mabi={64 if bits == 64 else 32}", "-mno-abicalls", "-fno-pic",
                 "-msoft-float", "-G0", "-ffreestanding", "-fno-strict-aliasing"]
        link = [args.ld, "-m", f"elf{bits}{'l' if little else 'b'}tsmip",
                "-static", "--emit-relocs", "-Ttext", "0x20000"]
        original = output / f"{profile}.elf"
        obj = output / f"{profile}.o"
        command([args.clang, *flags, *(["-DWIDE"] if bits == 64 else []),
                 "-c", fixture, "-o", obj], output / f"{profile}-assembly.log")
        command([*link, "-e", "mips_add", obj, "-o", original],
                output / f"{profile}-link.log")
        for function in args.function or (MICRO_FUNCTIONS if micro else FUNCTIONS):
            if bits < 64 and function in DOUBLEWORD_KERNELS:
                continue
            if function in FIELDS or function in SHUFFLES:
                if (FIELDS.get(function) or SHUFFLES[function])[0] > bits:
                    continue
            for backend in ("highc", "llvmc"):
                tasks.append((profile, bits, little, flags, link, original, function, backend))

    def compare_inner(task):
        profile, bits, little, flags, link, original, function, backend = task
        stem = output / f"{profile}-{function}-{backend}"
        source, obj, rebuilt = stem.with_suffix(".c"), stem.with_suffix(".o"), stem.with_suffix(".elf")
        command([args.neverd, "decompile", "--func", function,
                 *(["--llvm"] if backend == "llvmc" else ["--callees"]),
                 "-o", source, original], stem.with_suffix(".decompile.log"))
        command([args.clang, *flags, "-nostdinc", "-isystem", resource + "/include",
                 "-I", headers, "-O2", "-c", source, "-o", obj],
                stem.with_suffix(".compile.log"))
        # Linked-image LLVMC deliberately refers to original loaded storage.
        # Recompile at a distinct address and bind declared dependencies.
        # --callees follows direct machine calls; a recovered indirect target
        # can still be an external dependency in either source route.
        bindings = []
        bound_callees = []
        if backend == "llvmc":
            for name, address in re.findall(r"extern uint8_t (unk_([0-9A-Fa-f]+))\[", source.read_text()):
                bindings.append(f"--defsym={name}=0x{address}")
        image = ELFFile(io.BytesIO(original.read_bytes()))
        generated = ELFFile(io.BytesIO(obj.read_bytes()))
        for symbol in generated.get_section_by_name(".symtab").iter_symbols():
            name = symbol.name
            if symbol["st_shndx"] == "SHN_UNDEF" and name in ("mips_add", "mips_return13"):
                callee = image.get_section_by_name(".symtab").get_symbol_by_name(name)[0]["st_value"]
                bindings.append(f"--defsym={name}={callee}")
                bound_callees.append(name)
        rebuilt_link = ["0x1020000" if str(x) == "0x20000" else x for x in link]
        command([*rebuilt_link, "--image-base=0x1000000", *bindings,
                 "-e", function, obj, "-o", rebuilt], stem.with_suffix(".link.log"))
        observations = []
        uninitialized = function.startswith("mips_uninitialized_")
        old_values = [0]
        if uninitialized:
            rng = random.Random(97)
            old_values += [0x12345678ABCDEF98, -1] + [rng.getrandbits(bits) for _ in range(8)]
        data = (0xF2345678 if uninitialized else 0x12345678).to_bytes(
            4, "little" if little else "big") + bytes(range(4, 16))
        for inputs, old in ((values, old) for values in cases(function, bits)
                            for old in old_values):
            results = []
            for path in (original, rebuilt):
                # These HighC globals must survive relocation without the
                # original image mapped. LLVMC explicitly binds original
                # storage; other cases can bind original omitted callees.
                standalone = backend == "highc" and function in (
                    "mips_merge_ro_global", "mips_merge_rw_global")
                uc, entry = emulator(path, bits, little, function,
                                     original if path == rebuilt and not standalone else None)
                uc.mem_write(DATA, data)
                value = invoke(uc, entry, bits, little, inputs, old)
                if function == "mips_rw_global":
                    value = (value, invoke(uc, entry, bits, little, inputs))
                results.append(value)
            if results[0] != results[1]:
                raise RuntimeError(f"{stem.name} inputs={inputs}: original={results[0]} generated={results[1]}")
            if function in DOUBLEWORD_STORES:
                offset = inputs[0] - DATA
                value = inputs[1] & ((1 << 64) - 1)
                encoded = value.to_bytes(8, "little" if little else "big")
                memory = bytearray(data)
                if function == "mips_merge_store_double":
                    memory[offset:offset + 8] = encoded
                elif (function == "mips_merge_store_left_double") != little:
                    memory[offset:8] = encoded[:8 - offset]
                else:
                    memory[:offset + 1] = encoded[7 - offset:]
                if results[0] != (value, memory.hex()):
                    raise RuntimeError(f"{stem.name} inputs={inputs}: unexpected partial store or return")
            expected = independent_result(function, inputs)
            if expected is not None and results[0][0] != expected & ((1 << bits) - 1):
                raise RuntimeError(f"{stem.name}: result disagrees with independent field/lane expectation")
            if uninitialized:
                count = bits // 8 if "full" in function else 4
                offset = inputs[0] - DATA
                expected = int.from_bytes(data[offset:offset + count],
                                          "little" if little else "big", signed=True)
                if results[0][0] != expected & ((1 << bits) - 1):
                    raise RuntimeError(f"{stem.name}: pair disagrees with independent byte assembly")
            observations.append({"inputs": inputs, "old_destination": old,
                                 "result": results[0]})
        faults = []
        if function in ("mips_jalr_merge_slot", "mips_merge_discard", "mips_merge_zero"):
            inputs = ([7, 11, DATA + 4096] if function == "mips_jalr_merge_slot"
                      else [DATA + 4096])
            callees = set()
            for path in (original, rebuilt):
                symbols = ELFFile(io.BytesIO(path.read_bytes())).get_section_by_name(".symtab")
                callees.update(symbol["st_value"] for symbol in
                               (symbols.get_symbol_by_name("mips_add") or [])
                               if symbol["st_shndx"] != "SHN_UNDEF")
            outcomes = []
            for path in (original, rebuilt):
                uc, entry = emulator(path, bits, little, function,
                                     original if path == rebuilt else None)
                outcomes.append(fault_observation(uc, entry, bits, little, inputs, callees))
            assert outcomes[0] == outcomes[1]
            faults.append({"inputs": inputs, "result": outcomes[0]})
        if uninitialized or function == "mips_likely_load":
            count = bits // 8 if "full" in function else 4
            # Across either edge of the mapped page, one load succeeds and
            # the other faults. Swapping order swaps the required read prefix.
            fault_cases = [([DATA + 4096, 0], False)]
            if uninitialized:
                first_left = not function.endswith("_reverse")
                upper_first = first_left == little
                fault_cases = [([DATA - 1], upper_first),
                               ([DATA + 4096 - count + 1], not upper_first)]
            for inputs, prefix in fault_cases:
                outcomes = []
                for path in (original, rebuilt):
                    uc, entry = emulator(path, bits, little, function,
                                         original if path == rebuilt else None)
                    outcomes.append(fault_observation(uc, entry, bits, little,
                                                      inputs, set(), prefix))
                assert outcomes[0] == outcomes[1]
                faults.append({"inputs": inputs, "result": outcomes[0]})
        strict_refusals = []
        if bits == 64 and function in ("mips_word_likely", "mips_word_zero"):
            # Architectural UNPREDICTABLE input: test NeverD's explicit policy,
            # never use the original machine's chosen result as an oracle.
            for invalid in (0x100000001, 0x80000000, 0xffffffff00000001):
                inputs = [invalid, 0] if function == "mips_word_likely" else [invalid]
                uc, entry = emulator(rebuilt, bits, little, function,
                                     original if backend == "llvmc" else None)
                try:
                    invoke(uc, entry, bits, little, inputs)
                except UcError as error:
                    if error.errno != UC_ERR_EXCEPTION:
                        raise RuntimeError(f"strict word refusal raised an unexpected error: {error}") from error
                    strict_refusals.append({"inputs": inputs, "error": error.errno,
                                           "original_executed": False})
                else:
                    raise RuntimeError(f"{stem.name}: invalid word input was silently accepted")
        if function in DOUBLEWORD_DIVISIONS:
            # The original's zero result is unspecified. Generated sources
            # enforce their nonzero divisor domain for a retained result.
            uc, entry = emulator(rebuilt, bits, little, function,
                                 original if backend == "llvmc" else None)
            try:
                invoke(uc, entry, bits, little, [7, 0])
            except UcError as error:
                if error.errno != UC_ERR_EXCEPTION:
                    raise
            else:
                raise RuntimeError(f"{stem.name}: live division accepted a zero divisor")
        receipt = {"profile": profile, "encoding": args.encoding,
                "function": function, "backend": backend,
                "cases": observations,
                "faults": faults,
                "strict_word_refusals": strict_refusals,
                "original_image_binding": backend == "llvmc",
                "original_image_unmapped": standalone,
                "original_callee_bindings": bound_callees,
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "rebuilt_sha256": hashlib.sha256(rebuilt.read_bytes()).hexdigest(),
                "removed_bytes": obj.stat().st_size + rebuilt.stat().st_size}
        return receipt

    def compare(task):
        stem = output / f"{task[0]}-{task[6]}-{task[7]}"
        try:
            return compare_inner(task)
        except BaseException:
            artifacts.keep_source(stem.with_suffix(".c"))
            raise
        finally:
            artifacts.discard(*(stem.with_suffix(suffix) for suffix in
                                (".o", ".elf", ".c", ".decompile.log", ".compile.log", ".link.log")))

    receipts, failures = [], []
    pool = ThreadPoolExecutor(max_workers=4)
    try:
        futures = [pool.submit(compare, task) for task in tasks]
        for task, future in zip(tasks, futures):
            try:
                receipt = future.result()
                receipts.append(receipt)
                print(f"PASS {task[0]} {task[6]} {task[7]}", flush=True)
            except Exception as error:
                failures.append(str(error))
                print(f"FAIL {task[0]} {task[6]} {task[7]}: {error}", flush=True)
            artifacts.write("results.json", {"passed": receipts, "failures": failures, "complete": False})
    except BaseException:
        artifacts.cancel()
        raise
    finally:
        pool.shutdown(wait=True, cancel_futures=True)
    cleanup = {"removed_bytes": sum(r["removed_bytes"] for r in receipts),
               "originals": {}}
    for profile, _, _ in profiles:
        for suffix in (".o", ".elf"):
            artifact = output / (profile + suffix)
            cleanup["originals"][artifact.name] = hashlib.sha256(artifact.read_bytes()).hexdigest()
            cleanup["removed_bytes"] += artifact.stat().st_size
            artifacts.discard(artifact)
    artifacts.write("results.json", {"passed": receipts, "failures": failures,
                                      "cleanup": cleanup, "complete": True})
    print(f"{len(receipts)} passed, {len(failures)} failed", flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
