#!/usr/bin/env python3
"""Qualify i386 RAO transport and C ABI; no hardware atomicity/fault claim.

Run NeverDX86LegacyEncodingSourceTests with NEVERD_LEGACY_ATOMIC_EXPORT_DIR.
--oracle is NeverC's verify-apx-atomic-completion.py (independent byte decoder).
NeverC's frontend is x64-only; i386 C projections are compiled with Clang.
"""
import argparse
import importlib.util
import itertools
import json
from pathlib import Path

TRIPLES = ("i386-unknown-linux-gnu", "i386-pc-windows-msvc", "i386-apple-darwin")
GPRS = ("EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI")


def object_image(oracle, binary, text, patched):
    if not isinstance(binary, oracle.lief.MachO.Binary):
        if isinstance(binary, oracle.lief.ELF.Binary):
            assert all(not r.has_section or r.section.name != text.name
                       for r in binary.relocations), "unresolved ELF code relocation"
        else:
            assert not list(text.relocations), "unresolved COFF code relocation"
        return bytes(patched)
    # Darwin's i386 ctpop lowering can read __literal16 through a local
    # call/pop PC anchor. Preserve the object's section layout: a uniformly
    # rebased LOCAL_SECTDIFF/PAIR needs no patch, but the constants must exist.
    sections = [s for s in binary.sections if s.content and s.name != "__eh_frame"]
    assert text.virtual_address == 0
    assert all(s.name in ("__text", "__literal16") for s in sections)
    image = bytearray(max(s.virtual_address + len(s.content) for s in sections))
    for section in sections:
        begin = section.virtual_address
        content = patched if section.name == "__text" else bytes(section.content)
        image[begin:begin + len(content)] = content
        relocations = list(section.relocations)
        assert len(relocations) % 2 == 0
        for i in range(0, len(relocations), 2):
            target, origin = relocations[i:i + 2]
            assert target.type.name == "LOCAL_SECTDIFF" and origin.type.name == "PAIR"
            assert target.is_scattered and origin.is_scattered
            assert target.size == origin.size == 32 and not target.pc_relative and not origin.pc_relative
            offset = target.address - section.offset
            assert 0 <= offset <= len(content) - 4
            assert int.from_bytes(content[offset:offset + 4], "little") == (target.value - origin.value) & 0xffffffff
            assert all(any(s.virtual_address <= value < s.virtual_address + len(s.content)
                           for s in sections) for value in (target.value, origin.value))
    return bytes(image)


def qualify(oracle, obj, matrix):
    binary = oracle.lief.parse(str(obj))
    text = oracle.text_section(binary)
    code = bytes(text.content)
    symbols = {s.name.lstrip("_"): s.value - getattr(text, "virtual_address", 0)
               for s in binary.symbols if s.name.lstrip("_").startswith("rao32_")}
    assert set(symbols) == {f"rao32_{i}" for i in range(len(matrix))}
    native = oracle.decode(code, mode64=False)
    assert len(native) == sum(c["repeats"] for c in matrix)
    patched = bytearray(code)
    for at, instruction in native.items():
        assert instruction["legacy"] and instruction["bits"] == 32
        patched[at:instruction["end"]] = b"\x90" * (instruction["end"] - at)
    uc = oracle.unicorn.Uc(oracle.unicorn.UC_ARCH_X86, oracle.unicorn.UC_MODE_32)
    image = object_image(oracle, binary, text, patched)
    span = (len(image) + 2 * oracle.PAGE - 1) // oracle.PAGE * oracle.PAGE
    stop = oracle.CODE + span - 1
    uc.mem_map(oracle.CODE, span)
    uc.mem_write(oracle.CODE, image)
    uc.mem_map(oracle.STACK, 4 * oracle.PAGE)
    uc.mem_map(0x4000, oracle.PAGE)
    reg = lambda name: getattr(oracle.regs, "UC_X86_REG_" + name)
    def boundary(machine, address, size, user):
        if address == stop or address - oracle.CODE in native:
            machine.emu_stop()
    uc.hook_add(oracle.unicorn.UC_HOOK_CODE, boundary)
    checks = 0
    for index, case in enumerate(matrix):
        for add, alias in itertools.product((0, 1, 0xffffffff, 0x80000000, 0x31415926), (False, True)):
            offset = 0x12344080 if case["address_bits"] == 16 else 0x4080
            address = offset & ((1 << case["address_bits"]) - 1)
            ordinary = address if alias else 0x4180
            before = offset ^ add
            memory = before if alias else 0x87654321
            for i, name in enumerate(GPRS):
                uc.reg_write(reg(name), 0xa0000000 + i)
            saved = {name: uc.reg_read(reg(name)) for name in ("EBX", "EBP", "ESI", "EDI")}
            sp = oracle.STACK + 2 * oracle.PAGE - 16
            uc.reg_write(reg("ESP"), sp)
            uc.mem_write(sp, oracle.struct.pack("<4I", stop, offset, add, ordinary))
            uc.mem_write(address, oracle.struct.pack("<I", 0x87654321))
            uc.mem_write(ordinary, oracle.struct.pack("<I", 0xdeadbeef))
            uc.reg_write(reg("EFLAGS"), 0x202)
            pc = oracle.CODE + symbols[f"rao32_{index}"]
            for repeat in range(case["repeats"]):
                uc.emu_start(pc, stop, count=10000)
                at = uc.reg_read(reg("EIP")) - oracle.CODE
                assert at in native, (obj, index, repeat, "missing RAO")
                instruction = native[at]
                assert tuple(instruction[k] for k in ("op", "address", "segment")) == (
                    case["operation"], case["address_bits"], case["segment"]), (obj, index, instruction)
                actual_offset = uc.reg_read(reg(GPRS[instruction["base"]]))
                assert actual_offset & ((1 << case["address_bits"]) - 1) == address
                assert uc.reg_read(reg(GPRS[instruction["add"]])) == add
                assert int.from_bytes(uc.mem_read(address, 4), "little") == memory
                if not repeat:
                    assert int.from_bytes(uc.mem_read(ordinary, 4), "little") == before
                memory = {"aadd": lambda: (memory + add) & 0xffffffff,
                          "aand": lambda: memory & add,
                          "aor": lambda: memory | add,
                          "axor": lambda: memory ^ add}[case["operation"]]()
                uc.mem_write(address, oracle.struct.pack("<I", memory))
                # RAO does not modify flags or its source register.
                pc = oracle.CODE + instruction["end"]
                checks += 1
            uc.emu_start(pc, stop, count=10000)
            assert uc.reg_read(reg("EIP")) == stop, (obj, index, "missing return")
            assert uc.reg_read(reg("EAX")) == before ^ (memory if alias else before), (obj, index, "result")
            assert uc.reg_read(reg("ESP")) == sp + 4, (obj, index, "cdecl stack")
            assert all(uc.reg_read(reg(name)) == value for name, value in saved.items()), (obj, index, "C ABI")
    return {"functions": len(matrix), "boundaries": len(native), "controlled_completions": checks}


def qualify_pipeline(oracle, obj, case):
    binary = oracle.lief.parse(str(obj))
    section = oracle.text_section(binary)
    code = bytes(section.content)
    start = next(s.value - getattr(section, "virtual_address", 0) for s in binary.symbols
                 if s.name.lstrip("_") == "rao32_pipeline")
    native = oracle.decode(code, mode64=False)
    assert len(native) == 1, (obj, native)
    at, instruction = next(iter(native.items()))
    assert instruction["legacy"] and instruction["bits"] == 32 and instruction["address"] == 32
    assert instruction["op"] == case["operation"] and instruction["segment"] == case["segment"]
    patched = bytearray(code)
    patched[at:instruction["end"]] = b"\x90" * (instruction["end"] - at)
    uc = oracle.unicorn.Uc(oracle.unicorn.UC_ARCH_X86, oracle.unicorn.UC_MODE_32)
    image = object_image(oracle, binary, section, patched)
    span = (len(image) + 2 * oracle.PAGE - 1) // oracle.PAGE * oracle.PAGE
    stop = oracle.CODE + span - 1
    uc.mem_map(oracle.CODE, span)
    uc.mem_write(oracle.CODE, image)
    uc.mem_map(oracle.STACK, 4 * oracle.PAGE)
    reg = lambda name: getattr(oracle.regs, "UC_X86_REG_" + name)
    def boundary(machine, address, size, user):
        if address in (oracle.CODE + at, stop):
            machine.emu_stop()
    uc.hook_add(oracle.unicorn.UC_HOOK_CODE, boundary)
    for i, name in enumerate(GPRS):
        uc.reg_write(reg(name), 0xa0000000 + i)
    saved = {name: uc.reg_read(reg(name)) for name in ("EBX", "EBP", "ESI", "EDI")}
    sp = oracle.STACK + 2 * oracle.PAGE - 4
    uc.reg_write(reg("ESP"), sp)
    uc.mem_write(sp, oracle.struct.pack("<I", stop))
    uc.reg_write(reg("EFLAGS"), 0x202)
    uc.emu_start(oracle.CODE + start, stop, count=10000)
    assert uc.reg_read(reg("EIP")) == oracle.CODE + at, (obj, "missing RAO")
    offset = 0x208 if case["address_bits"] == 16 else 0x76540208
    assert uc.reg_read(reg(GPRS[instruction["base"]])) == offset, (obj, "effective address")
    assert uc.reg_read(reg(GPRS[instruction["add"]])) == 0xfedcba98, (obj, "source value")
    uc.emu_start(oracle.CODE + instruction["end"], stop, count=10000)
    assert uc.reg_read(reg("EIP")) == stop, (obj, "missing return")
    flags = oracle.flags_sub(0x89abcdef, 0xfedcba98, 32)
    expected = ((flags & 0xd5) | 2) << 8 | ((flags >> 11) & 1)
    assert uc.reg_read(reg("EAX")) == expected, (obj, "flags", uc.reg_read(reg("EAX")), expected)
    assert uc.reg_read(reg("ESP")) == sp + 4, (obj, "cdecl stack")
    assert all(uc.reg_read(reg(name)) == value for name, value in saved.items()), (obj, "C ABI")
    return 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--exports", type=Path, required=True)
    p.add_argument("--oracle", type=Path, required=True)
    p.add_argument("--llc", type=Path, required=True)
    p.add_argument("--clang", type=Path, required=True)
    p.add_argument("--keep", type=Path, required=True)
    args = p.parse_args()
    spec = importlib.util.spec_from_file_location("atomic_oracle", args.oracle)
    oracle = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(oracle)
    args.keep.mkdir(parents=True, exist_ok=True)
    reports = []
    for i, triple in enumerate(TRIPLES):
        stem = args.exports / f"i386-{i}"
        matrix = json.loads(stem.with_suffix(".json").read_text())
        reports.append({"route": "production", "triple": triple,
                        **qualify(oracle, stem.with_suffix(".o"), matrix)})
        for opt in ("O0", "O2"):
            obj = args.keep / f"llvm-{i}-{opt}.o"
            oracle.run([args.llc, "-mtriple=" + triple, "-" + opt, "-verify-machineinstrs",
                        "-filetype=obj", stem.with_suffix(".ll"), "-o", obj])
            reports.append({"route": "llvm", "triple": triple, "optimization": opt,
                            **qualify(oracle, obj, matrix)})
            for route in ("call", "llvm"):
                obj = args.keep / f"c-{route}-{i}-{opt}.o"
                oracle.run([args.clang, "--target=" + triple, "-" + opt, "-ffreestanding",
                            "-fno-stack-protector", "-fno-function-sections", "-fno-lto",
                            "-c", str(stem) + "-" + route + ".c", "-o", obj])
                reports.append({"route": "c-" + route, "triple": triple, "optimization": opt,
                                **qualify(oracle, obj, matrix)})
        print({"triple": triple, "objects": len(reports)}, flush=True)
    pipeline = []
    objects = sorted((args.exports / "i386-pipeline").glob("*.o"))
    assert len(objects) == 288, "missing i386 decoded pipeline exports"
    for obj in objects:
        case = json.loads(obj.with_suffix(".json").read_text())
        checks = qualify_pipeline(oracle, obj, case)
        for route, opt in itertools.product(("high", "llvm"), ("O0", "O2")):
            output = args.keep / "pipeline.o"
            oracle.run([args.clang, "--target=" + case["triple"], "-" + opt, "-ffreestanding",
                        "-fno-stack-protector", "-fno-function-sections", "-fno-lto", "-c",
                        str(obj.with_suffix("")) + "-" + route + ".c", "-o", output])
            try:
                checks += qualify_pipeline(oracle, output, case)
            except AssertionError as error:
                raise AssertionError((obj.name, route, opt, error.args)) from error
        pipeline.append({"fixture": obj.name, "objects": 5, "controlled_completions": checks})
    (args.keep / "qualification.json").write_text(json.dumps({
        "programs": reports, "pipeline": pipeline,
        "physical_atomicity_qualified": False}, indent=2) + "\n")


if __name__ == "__main__":
    main()
