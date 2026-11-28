#!/usr/bin/env python3
"""Execute PowerPC conditional calls and local CTR transfers before and after projection."""

import argparse
import json
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory

from elftools.elf.elffile import ELFFile
from unicorn import ppc_const as reg

from extended_scalar import image_base_option, project
from roundtrip import DATA, RETURN, STACK, compile_elf, machine

PROFILES = ("powerpc", "powerpcle", "powerpc64", "powerpc64le")
FUNCTIONS = ("direct_call", "lr_call", "ctr_call", "ctr_jump", "ctr_join", "ctr_select")
SOURCE = Path(__file__).with_name("powerpc-conditional-branches.S")


def execute(path, profile, descriptors, names):
    endian = "little" if profile.endswith("le") else "big"
    for name in names:
        cpu, symbols = machine(path, profile, power8=name == "ctr_select")
        entry = symbols[name]
        toc = 0
        if descriptors:
            toc = int.from_bytes(cpu.mem_read(entry + 8, 8), endian)
            entry = int.from_bytes(cpu.mem_read(entry, 8), endian)
        for value in (0, 1, 37):
            for choice in (0, 1):
                call = name.endswith("_call")
                alternate = name in ("lr_call", "ctr_call") and choice
                arg = symbols["bump_last" if alternate else "bump"] if call else choice
                higher = (3, 5, 7, 11, 13)
                for index, initial in enumerate(higher):
                    cpu.reg_write(reg.UC_PPC_REG_6 + index, initial)
                cpu.mem_write(DATA, (19).to_bytes(4, endian))
                for register, initial in ((reg.UC_PPC_REG_1, STACK + 2048),
                                          (reg.UC_PPC_REG_2, toc),
                                          (reg.UC_PPC_REG_3, value),
                                          (reg.UC_PPC_REG_4, DATA),
                                          (reg.UC_PPC_REG_5, arg),
                                          (reg.UC_PPC_REG_12, entry),
                                          (reg.UC_PPC_REG_LR, RETURN)):
                    cpu.reg_write(register, initial)
                cpu.emu_start(entry, RETURN, count=1000)
                result = cpu.reg_read(reg.UC_PPC_REG_3)
                count = int.from_bytes(cpu.mem_read(DATA, 4), endian)
                if call:
                    increment = sum(higher) if alternate else 9
                    delta = increment if value == 0 else 0
                elif value:
                    delta = 1
                elif name == "ctr_select":
                    delta = 19 if choice else 7
                else:
                    delta = 7 if name == "ctr_jump" or not choice else 1
                expected = (RETURN, STACK + 2048, value + delta,
                            19 + (1 + int(alternate)) * int(call and value == 0))
                actual = (cpu.reg_read(reg.UC_PPC_REG_PC), cpu.reg_read(reg.UC_PPC_REG_1),
                          result, count)
                if actual != expected:
                    raise AssertionError((profile, name, value, choice, actual, expected))


def check_graph(neverd, image, name):
    command = [neverd, "cfg", "--func", name, image]
    graph = json.loads(subprocess.check_output([*command, "--json"], text=True))
    branch, = [node for node in graph["nodes"]
               if node["disasm"][-1].split(": ", 1)[1].split()[0].endswith("ctr")]
    skip = int(branch["end"], 16)
    after = sorted((int(node["start"], 16), node["id"]) for node in graph["nodes"]
                   if int(node["start"], 16) >= skip)
    if after[0][0] != skip or len(after) != (3 if name == "ctr_select" else 2):
        raise AssertionError((name, after))
    expected = [(idx, "true") for _, idx in after[1:]]
    expected.append((after[0][1], "false"))
    if name == "ctr_join":
        expected.append((after[0][1], "true"))
    observed = [(edge["to"], edge["type"]) for edge in graph["edges"]
                if edge["from"] == branch["id"]]
    if sorted(observed) != sorted(expected):
        raise AssertionError((name, observed, expected))
    dot = subprocess.check_output([*command, "--dot"], text=True)
    edges = re.findall(rf"bb{branch['id']} -> bb(\d+) \[color=(\w+)\]", dot)
    colors = [(str(idx), "green" if role == "true" else "red") for idx, role in expected]
    if sorted(edges) != sorted(colors):
        raise AssertionError((name, edges, colors))


def check(args, work, profile):
    descriptors = "64" in profile and args.ppc64_abi == "elfv1"
    flags = [f"-mabi={args.ppc64_abi}"] if "64" in profile else []
    original = work / "original.elf"
    compile_elf(args, work, profile, SOURCE, original, FUNCTIONS[0], extra_flags=flags)
    execute(original, profile, descriptors, FUNCTIONS)
    print(profile, "original passed", flush=True)
    for name in ("ctr_jump", "ctr_join", "ctr_select"):
        check_graph(args.neverd, original, name)
    with original.open("rb") as stream:
        old_code = ELFFile(stream).get_section_by_name(".text")["sh_addr"]
    placement = image_base_option(args.ld)
    for route in ("highc", "llvmc", "llvm"):
        sources = [project(args, original, name, route)
                   for name in (FUNCTIONS[:1] if route == "llvm" else FUNCTIONS)]
        if route != "llvm":
            sources.append(SOURCE)
        for opt, base in (("-O0", "0x210000"), ("-O2", "0x610000")):
            rebuilt = work / "rebuilt.elf"
            compile_elf(args, work, profile, sources[0], rebuilt, FUNCTIONS[0], opt,
                        sources[1:], [*flags, "-DHELPER_ONLY", f"-Wl,{placement}={base}"])
            with rebuilt.open("rb") as stream:
                if any(s["p_type"] == "PT_LOAD" and
                       s["p_vaddr"] <= old_code < s["p_vaddr"] + s["p_memsz"]
                       for s in ELFFile(stream).iter_segments()):
                    raise AssertionError("original code is still mapped")
            execute(rebuilt, profile, descriptors, FUNCTIONS)
            print(profile, route, opt, "passed", flush=True)

    unknown = work / "unknown.elf"
    compile_elf(args, work, profile, SOURCE, unknown, "ctr_select",
                extra_flags=[*flags, "-DUNKNOWN_CTR_ARM"])
    reason = "no proven local target set"
    for route in ([], ["--llvm"]):
        result = subprocess.run([args.neverd, "decompile", *route, "--func", "ctr_select",
                                 unknown, "-o", work / "unknown.c"], capture_output=True, text=True)
        if not result.returncode or reason not in result.stderr:
            raise AssertionError((profile, route, result.returncode, result.stderr))
    module = work / "unknown.ll"
    subprocess.run([args.neverd, "lift", unknown, "-o", module], check=True)
    lines = module.read_text().splitlines()
    if (not any(line.startswith("; neverd.not-lifted: ctr_select ") and reason in line
                for line in lines) or
            any(line.startswith("define ") and "@ctr_select(" in line for line in lines)):
        raise AssertionError((profile, "unproven CTR target in lifted module"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for tool in ("neverd", "clang", "ld"):
        parser.add_argument("--" + tool, required=True, type=Path)
    parser.add_argument("--ppc64-abi", choices=("elfv1", "elfv2"), default="elfv2")
    parser.add_argument("--profiles", nargs="+", choices=PROFILES, default=PROFILES)
    args = parser.parse_args()
    with TemporaryDirectory() as temp:
        for profile in args.profiles:
            work = Path(temp) / profile
            work.mkdir()
            check(args, work, profile)


if __name__ == "__main__":
    main()
