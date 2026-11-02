#!/usr/bin/env python3
"""Qualify exported NeverD AMX address sources in realigned dynamic frames.

Run NeverDX86AMXAddressTests with NEVERD_TEST_AMX_ADDRESS_EXPORT first.
Controlled completions check the native address/stride, frame canaries and
saved registers. They do not execute AMX or qualify hardware faults/restart.
Requires capstone, lief and unicorn.
"""

import argparse
import itertools
import json
from pathlib import Path
import struct
import subprocess

import capstone
import lief
import unicorn
import unicorn.x86_const as regs

PAGE, CODE, STACK = 4096, 0x10000, 0x80000
MASK = (1 << 64) - 1
GPRS = "RAX RCX RDX RBX RSP RBP RSI RDI R8 R9 R10 R11 R12 R13 R14 R15".split()
SHAPES = {
    0: (0x49, 0),
    1: (0x49, 1),
    4: (0x4B, 3),
    5: (0x4B, 1),
    6: (0x4A, 3),
    7: (0x4A, 1),
    8: (0x4B, 2),
}
TRIPLES = ("x86_64-unknown-linux-gnu", "x86_64-pc-windows-msvc", "x86_64-apple-darwin")


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True)
    assert result.returncode == 0, (command, result.stdout, result.stderr)


def text_section(path):
    obj = lief.parse(str(path))
    section = next(
        s for s in obj.sections if s.name in (".text", "__text") and len(s.content)
    )
    code = bytearray(section.content)
    if isinstance(obj, lief.ELF.Binary):
        for reloc in obj.relocations:
            if reloc.section != section:
                continue
            assert reloc.type in (
                lief.ELF.Relocation.TYPE.X86_64_PC32,
                lief.ELF.Relocation.TYPE.X86_64_PLT32,
            )
            assert reloc.symbol.shndx == list(obj.sections).index(section)
            at = reloc.address
            code[at : at + 4] = (
                reloc.symbol.value + reloc.addend - at
            ).to_bytes(4, "little", signed=True)
    elif isinstance(obj, lief.COFF.Binary):
        # Internal leaf calls still carry COFF relocations in a .o. Resolve
        # only same-section REL32 references before executing the fixture.
        for reloc in section.relocations:
            assert reloc.type == lief.COFF.Relocation.TYPE.AMD64_REL32
            assert reloc.symbol.section == section, "unexpected external call"
            at = reloc.address
            addend = int.from_bytes(code[at : at + 4], "little", signed=True)
            code[at : at + 4] = (addend + reloc.symbol.value - at - 4).to_bytes(
                4, "little", signed=True
            )
    elif isinstance(obj, lief.MachO.Binary):
        # LIEF's Mach-O relocation view does not retain the symbol for these
        # private BRANCH relocations. Read the documented eight-byte record;
        # admit only external-symbol, PC-relative, four-byte same-text calls.
        raw, symbols = path.read_bytes(), list(obj.symbols)
        text_index = next(i + 1 for i, s in enumerate(obj.sections) if s == section)
        for i in range(section.numberof_relocations):
            at, info = struct.unpack_from("<II", raw, section.relocation_offset + 8 * i)
            assert info >> 24 == 0x2D, "unexpected Mach-O relocation"
            symbol = symbols[info & 0xFFFFFF]
            assert symbol.numberof_sections == text_index, "unexpected external call"
            addend = int.from_bytes(code[at : at + 4], "little", signed=True)
            displacement = addend + symbol.value - section.virtual_address - at - 4
            code[at : at + 4] = displacement.to_bytes(4, "little", signed=True)
    return bytes(code)


def native_address(code, case):
    """Read VEX/EVEX memory fields independently of the compiler descriptor."""
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    offset, accesses = 0, []
    while offset < len(code):
        start, p, prefixes = offset, offset, []
        while p < len(code) and code[p] in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x67):
            prefixes.append(code[p])
            p += 1
        evex = p < len(code) and code[p] == 0x62
        if (
            p + 5 + evex <= len(code)
            and code[p] in (0xC4, 0x62)
            and code[p + 1] & (7 if evex else 31) == 2
        ):
            p0, p1 = code[p + 1 : p + 3]
            opcode, modrm = code[p + 3 + evex : p + 5 + evex]
            if (opcode, p1 & 3) in SHAPES.values() and modrm >> 6 != 3:
                assert (opcode, p1 & 3) == SHAPES[case["operation"]]
                assert p1 & (0xF8 if evex else 0xFC) == 0x78 and modrm & 0x38 == 0
                assert evex == case["evex"], "memory encoding family changed"
                if evex:
                    assert p1 & 4 and code[p + 3] == 8
                mod, base, index, scale, end = (
                    modrm >> 6,
                    modrm & 7,
                    None,
                    1,
                    p + 5 + evex,
                )
                if base == 4:
                    sib = code[end]
                    end += 1
                    base, index, scale = sib & 7, (sib >> 3) & 7, 1 << (sib >> 6)
                    index += ((~p0 >> 6) & 1) * 8
                    if index == 4:
                        index = None
                assert not (mod == 0 and base == 5), "unexpected absolute/RIP base"
                base += ((~p0 >> 5) & 1) * 8
                size = 1 if mod == 1 else 4 if mod == 2 else 0
                displacement = int.from_bytes(
                    code[end : end + size], "little", signed=True
                )
                end += size
                assert (32 if 0x67 in prefixes else 64) == case["address_bits"]
                assert prefixes.count(0x36 if case["ss_prefix"] else 0x3E) == 1
                assert (base in (4, 5)) == case["stack"], (base, case)
                if case["operation"] >= 4:
                    assert index is not None and scale == 1 and displacement == 0
                accesses.append((start, end, base, index, scale, displacement))
                offset = end
                continue
        ordinary = next(decoder.disasm(code[offset:], offset, count=1), None)
        assert ordinary, (offset, code[offset : offset + 16].hex())
        offset += ordinary.size
    assert len(accesses) == 1, accesses
    return accesses[0]


def complete(code, native, case, windows, address, stride, allocation, *,
             entry=0, expected_result=0x123456789ABCDEF0 ^ 0xA5):
    start, end, base, index, scale, displacement = native
    patched = bytearray(code)
    patched[start:end] = b"\x90" * (end - start)
    machine = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_64)
    machine.mem_map(CODE, PAGE)
    machine.mem_map(STACK, PAGE * 4)
    machine.mem_write(CODE, bytes(patched))
    stop = CODE + PAGE - 1
    reg = lambda name: getattr(regs, "UC_X86_REG_" + name)
    for i, name in enumerate(GPRS):
        machine.reg_write(reg(name), 0xABC0000000000000 + i)
    saved_names = ("RBX", "RBP", "R12", "R13", "R14", "R15") + (
        ("RSI", "RDI") if windows else ()
    )
    saved = {name: machine.reg_read(reg(name)) for name in saved_names}
    parameters = ("RCX", "RDX", "R8") if windows else ("RDI", "RSI", "RDX")
    for name, value in zip(parameters, (address, stride, allocation)):
        machine.reg_write(reg(name), value)
    sp = STACK + PAGE * 3 - 8
    machine.reg_write(reg("RSP"), sp)
    machine.mem_write(sp, struct.pack("<Q", stop))
    machine.reg_write(reg("EFLAGS"), 0x202)
    machine.emu_start(CODE + entry, CODE + start, count=10000)
    assert machine.reg_read(reg("RIP")) == CODE + start
    mask = (1 << case["address_bits"]) - 1
    value = machine.reg_read(reg(GPRS[base])) + displacement
    if case["operation"] >= 4:
        assert (
            machine.reg_read(reg(GPRS[index])) & mask == stride & mask
        ), "stride changed"
    elif index is not None:
        value += machine.reg_read(reg(GPRS[index])) * scale
    assert value & mask == address & mask, (
        "native address",
        hex(value & mask),
        hex(address & mask),
    )
    # The controlled instruction changes no GPR. Completing it must leave the
    # compiler's dynamic frame and all calling-convention saved state intact.
    machine.emu_start(CODE + end, stop, count=10000)
    assert machine.reg_read(reg("RIP")) == stop
    if expected_result is not None:
        assert machine.reg_read(reg("RAX")) == expected_result, "frame canary"
    assert machine.reg_read(reg("RSP")) == sp + 8, "return stack"
    assert all(
        machine.reg_read(reg(n)) == v for n, v in saved.items()
    ), "saved registers"


def complete_modules(args):
    """Compile the complete LLVMC module, including prototypes and private leaves."""
    reports = []
    for operation, triple, opt, route in itertools.product(
        range(4, 9), TRIPLES, ("O0", "O2"),
        ("llvm-asm", "neverc-bitcode", "neverc-c", "bridge")
    ):
        stem = f"module-{operation}-{TRIPLES.index(triple)}-{opt}-{route}"
        source = args.exports / f"stack-leaf-{operation}"
        obj = args.keep / (stem + ".o")
        compile_c = [args.compiler, "--target=" + triple, "-" + opt,
                     "-fno-lto", "-fno-builtin-mimalloc", "-ffreestanding",
                     "-fno-function-sections", "-fno-data-sections"]
        lower_ir = [args.llc, "-mtriple=" + triple, "-" + opt,
                    "-verify-machineinstrs", "-filetype=obj"]
        if route == "llvm-asm":
            run(lower_ir + [source.with_suffix(".ll"), "-o", obj])
        elif route == "neverc-bitcode":
            bitcode = args.keep / (stem + ".bc")
            run([args.llc.parent / "llvm-as", source.with_suffix(".ll"),
                 "-o", bitcode])
            run(compile_c + ["-c", bitcode, "-o", obj])
        elif route == "bridge":
            ir = args.keep / (stem + ".ll")
            run(compile_c + ["-S", "-emit-llvm", source.with_suffix(".c"),
                             "-o", ir])
            assert " asm " not in ir.read_text(), stem
            run(lower_ir + [ir, "-o", obj])
        else:
            run(compile_c + ["-c", source.with_suffix(".c"), "-o", obj])
        binary = lief.parse(str(obj))
        section = next(s for s in binary.sections
                       if s.name in (".text", "__text") and len(s.content))
        symbol = next(s for s in binary.symbols if s.name in ("probe", "_probe"))
        entry = symbol.value - section.virtual_address
        code = text_section(obj)
        assert len(code) < PAGE - 1
        case = dict(operation=operation, address_bits=64, stack=True,
                    ss_prefix=False, evex=False)
        native = native_address(code, case)
        complete(code, native, case, "windows" in triple,
                 0xFACE000000001234, 0x8765432100000040, 0,
                 entry=entry, expected_result=None)
        reports.append(dict(case=stem, controlled_completions=1))
    (args.keep / "modules.json").write_text(json.dumps(reports, indent=2) + "\n")
    print(f"PASS: {len(reports)} complete exported LLVM/C modules")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exports", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--llc", type=Path, required=True)
    parser.add_argument("--keep", type=Path, required=True)
    args = parser.parse_args()
    args.keep.mkdir(parents=True, exist_ok=True)
    cases = []
    for line in (args.exports / "manifest.tsv").read_text().splitlines():
        name, op, bits, stack, ss, evex = line.split("\t")
        cases.append(
            dict(
                name=name,
                operation=int(op),
                address_bits=int(bits),
                stack=bool(int(stack)),
                ss_prefix=bool(int(ss)),
                evex=bool(int(evex)),
            )
        )
    assert cases
    reports = []
    for case, triple, opt, route in itertools.product(
        cases,
        TRIPLES,
        ("O0", "O2"),
        ("clang", "neverc-fallback", "neverc-native", "bridge"),
    ):
        # Stack rows need the native compiler's isolated frame. Ordinary C
        # inline assembly must not borrow the caller's live frame pointer.
        if case["stack"] and case["operation"] >= 4 and route in (
            "clang", "neverc-fallback"
        ):
            continue
        stem = f'{case["name"]}-{TRIPLES.index(triple)}-{opt}-{route}'
        source, obj = args.exports / (case["name"] + ".c"), args.keep / (stem + ".o")
        command = [
            args.clang if route == "clang" else args.compiler,
            "--target=" + triple,
            "-" + opt,
            "-ffreestanding",
            "-fno-function-sections",
            "-fno-data-sections",
            "-fno-lto",
            "-fno-stack-protector",
            "-mno-stack-arg-probe",
            source,
        ]
        if route != "clang":
            command += ["-fno-builtin-mimalloc"]
        if route in ("neverc-native", "bridge"):
            command += ["-DNEVERD_AMX_USE_NATIVE"]
        if route == "bridge":
            ir = args.keep / (stem + ".ll")
            run(command + ["-S", "-emit-llvm", "-o", ir])
            assert " asm " not in ir.read_text(), stem
            run(
                [
                    args.llc,
                    "-mtriple=" + triple,
                    "-" + opt,
                    "-verify-machineinstrs",
                    "-filetype=obj",
                    ir,
                    "-o",
                    obj,
                ]
            )
        else:
            run(command + ["-mllvm", "-verify-machineinstrs", "-c", "-o", obj])
        code = text_section(obj)
        assert len(code) < PAGE - 1
        try:
            native = native_address(code, case)
        except Exception as error:
            raise AssertionError((stem, error)) from error
        count = 0
        for address, stride, allocation in itertools.product(
            (0xFACE000000001234, 0xFFFFFFFFFFFFFFFF, 0x200000),
            (0xFEDCBA9876543210, 64),
            (65, 129),
        ):
            try:
                complete(
                    code, native, case, "windows" in triple, address, stride, allocation
                )
            except Exception as error:
                raise AssertionError(
                    (stem, address, stride, allocation, error)
                ) from error
            count += 1
        reports.append(dict(case=stem, controlled_completions=count))
    (args.keep / "qualification.json").write_text(json.dumps(reports, indent=2) + "\n")
    print(
        f"PASS: {len(reports)} AMX address objects; {sum(r['controlled_completions'] for r in reports)} controlled completions"
    )
    complete_modules(args)


if __name__ == "__main__":
    main()
