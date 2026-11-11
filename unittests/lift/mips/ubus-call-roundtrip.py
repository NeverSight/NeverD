#!/usr/bin/env python3
"""Execute the Archer C7 V5 ubus call handler and its recompiled C with modeled imports."""
import argparse
import hashlib
from pathlib import Path

from unicorn import UC_HOOK_CODE
from unicorn import mips_const as mips
from unicorn.unicorn_py3.unicorn import uclib

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts


FIRMWARE_SHA256 = "6e1129aa58cb398a2453876ea4aa2395feecc84ecf78ed8ab43efd534b40e475"
ENTRY, BLOB, QUIET, TIMEOUT, STDERR = 0x4012d0, 0x4119cc, 0x4119c0, 0x411900, 0x4119a0
CTX, OBJECT, METHOD, JSON_ARG, HEAD, STREAM = (roundtrip.DATA + x for x in range(0x100, 0x700, 0x100))
OBJECT_ID = 0x8badf00d
IMPORTS = {0x400aa0: "blob_buf_init", 0x400ac0: "ubus_lookup_id",
           0x400ae0: "blobmsg_add_json_from_string", 0x400b90: "fputs", 0x400ba0: "ubus_invoke"}
# argc, parser result, quiet, lookup status, invocation status, timeout.
CASES = [(n, 1, 0, 0, 7, 30) for n in (0, 1, 4, 0xffffffff)]
CASES += [(2, 0, 1, -5, 12, 30), (2, 0, 0, 0, -17, 0xffffffff),
          (2, 0, 1, 0, 0, 0), (3, 0, 0, 0, 7, 30), (3, 0, 1, 0, 7, 30),
          (3, 1, 0, 23, 7, 30), (3, 1, 1, 0, 0, 30), (3, 2, 0, 0, 42, 0x80000001)]


def expected(case):
    argc, parse, quiet, lookup, invoke, timeout = case
    calls = []
    if argc not in (2, 3):
        return 0xfffffffe, calls
    calls.append(("blob_buf_init", BLOB, 0))
    if argc == 3:
        calls.append(("blobmsg_add_json_from_string", BLOB, JSON_ARG))
        if not parse:
            if not quiet:
                calls.append(("fputs", 0x40160c, STREAM))
            return 0xffffffff, calls
    calls.append(("ubus_lookup_id", CTX, OBJECT))
    if lookup:
        return lookup & 0xffffffff, calls
    calls.append(("ubus_invoke", CTX, OBJECT_ID, METHOD, HEAD, 0x40114c, 0,
                  (1000 * timeout) & 0xffffffff))
    return invoke & 0xffffffff, calls


def observe(elf, function, original, case):
    cpu, entry = roundtrip.emulator(elf, 32, False, function, original=original)
    argc, parse, quiet, lookup, invoke, timeout = case

    def write_word(address, value):
        cpu.mem_write(address, (value & 0xffffffff).to_bytes(4, "big"))

    def word(address):
        return int.from_bytes(cpu.mem_read(address, 4), "big")

    def reg(index):
        return cpu.reg_read(mips.UC_MIPS_REG_0 + index)

    def set_reg(index, value):
        cpu.reg_write(mips.UC_MIPS_REG_0 + index, value & 0xffffffff)

    for i, pointer in enumerate((OBJECT, METHOD, JSON_ARG)):
        write_word(roundtrip.DATA + 4 * i, pointer)
    write_word(BLOB, 0)
    write_word(TIMEOUT, timeout)
    write_word(STDERR, STREAM)
    cpu.mem_write(QUIET, bytes([quiet]))
    message = b"Failed to parse message data\n\0"
    assert bytes(cpu.mem_read(0x40160c, len(message))) == message
    preserved = (*range(16, 24), 30)
    for i in preserved:
        set_reg(i, 0x73610000 + i)
    set_reg(4, CTX)
    set_reg(5, argc)
    set_reg(6, roundtrip.DATA)
    set_reg(29, roundtrip.STACK)
    set_reg(31, roundtrip.STOP)
    calls = []

    def external(_uc, address, _size, _unused):
        name = IMPORTS[address]
        if name == "ubus_lookup_id":
            calls.append((name, reg(4), reg(5)))
            slot = reg(6)
            assert reg(29) <= slot <= roundtrip.STACK - 4 and not slot % 4
            write_word(slot, OBJECT_ID)
            result = lookup
        elif name == "ubus_invoke":
            calls.append((name, *[reg(i) for i in range(4, 8)],
                          *[word(reg(29) + off) for off in (16, 20, 24)]))
            result = invoke
        else:
            calls.append((name, reg(4), reg(5)))
            if name == "blob_buf_init":
                write_word(reg(4), HEAD)
            result = parse if name == "blobmsg_add_json_from_string" else 0
        for i in (*range(2, 16), 24, 25):
            set_reg(i, 0xc10b0000 + i)
        set_reg(2, result)

    for address in IMPORTS:
        # Bind the five external stubs; handler instructions and delay slots execute unchanged.
        cpu.mem_write(address, bytes.fromhex("03e0000800000000"))
        cpu.hook_add(UC_HOOK_CODE, external, begin=address, end=address)
    cpu.emu_start(entry, roundtrip.STOP, count=4096)
    assert cpu.reg_read(mips.UC_MIPS_REG_PC) == roundtrip.STOP
    assert reg(29) == roundtrip.STACK and all(reg(i) == 0x73610000 + i for i in preserved)
    return reg(2), calls


def run(args, artifacts):
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    if digest(args.elf) != FIRMWARE_SHA256:
        raise ValueError("expected the Archer C7 V5 US ubus ELF identified in docs/firmware/archer-c7-v5.md")
    observations = [expected(case) for case in CASES]
    for case, wanted in zip(CASES, observations):
        got = observe(args.elf, ENTRY, None, case)
        if got != wanted:
            raise RuntimeError(f"firmware {case}: {got} != {wanted}")
    work = artifacts.work
    (work / "stdio.h").write_text("typedef struct FILE FILE; extern FILE *stderr; int fputs(const char *, FILE *);\n")
    (work / "string.h").write_text("#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n")
    # Bind the original image backing, import addresses and HighC's named globals.
    layout = work / "guest.ld"
    layout.write_text("""
SECTIONS {
  . = 0x20000;
  .text : { *(.text*) }
  .MIPS.abiflags : { *(.MIPS.abiflags) }
  .reginfo : { *(.reginfo) }
  .timeout 0x411900 : { *(.data.unk_411900) }
  .quiet 0x4119c0 : { *(.bss.unk_4119C0) }
  .blob 0x4119cc : { *(.bss.unk_4119CC) }
  .data : { *(.data*) } .bss : { *(.bss*) }
}
""")
    bind = [f"--defsym={name}=0x{address:x}" for address, name in IMPORTS.items()]
    bind += ["--defsym=unk_400000=0x400000", f"--defsym=stderr=0x{STDERR:x}"]
    flags = ["--target=mips-linux-gnu", "-march=mips32r2", "-mabi=32", "-mno-abicalls",
             "-fno-pic", "-G0", "-ffreestanding", "-fno-strict-aliasing", "-fdata-sections"]
    report = {"complete": False, "original_sha256": FIRMWARE_SHA256,
              "neverd_sha256": digest(args.neverd), "reference_library_sha256": digest(Path(uclib._name)),
              "entry": hex(ENTRY), "cases": CASES,
              "expected": observations, "original_cases_passed": len(CASES), "outputs": []}
    artifacts.write("results.json", report)
    for backend in ("highc", "llvmc"):
        source = work / (backend + ".c")
        log = work / "command.log"
        roundtrip.run_command(artifacts, [args.neverd, "decompile", "--func", hex(ENTRY),
                              *(["--llvm"] if backend == "llvmc" else []), args.elf, "-o", source], log)
        try:
            for opt in ("-O0", "-O2"):
                obj, elf = work / "call.o", work / "call.elf"
                roundtrip.run_command(artifacts, [args.clang, *flags, opt, "-I", work,
                                      "-c", source, "-o", obj], log)
                roundtrip.run_command(artifacts, [args.ld, "-m", "elf32btsmip", "-static", "-T",
                                      layout, "-e", "sub_4012D0", *bind, obj, "-o", elf], log)
                for case, wanted in zip(CASES, observations):
                    got = observe(elf, "sub_4012D0", args.elf, case)
                    if got != wanted:
                        raise RuntimeError(f"{backend} {opt} {case}: {got} != {wanted}")
                report["outputs"].append({"backend": backend, "optimization": opt,
                                          "source_sha256": digest(source), "elf_sha256": digest(elf),
                                          "cases_passed": len(CASES)})
                artifacts.write("results.json", report)
                artifacts.discard(obj, elf, log)
        except Exception:
            artifacts.keep_source(source)
            raise
        artifacts.discard(source)
    report["complete"] = True
    artifacts.write("results.json", report)
    print(len(CASES), "original cases and", len(CASES) * len(report["outputs"]), "rebuilt cases passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("elf", "neverd", "clang", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "mips-ubus-call-roundtrip",
                                 (args.elf, args.neverd, args.clang, args.ld)) as artifacts:
        run(args, artifacts)
