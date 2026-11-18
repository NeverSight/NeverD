#!/usr/bin/env python3
"""Execute n32/n64 numeric code identities across relocated microMIPS targets.

Original and rebuilt classic callers use independent GCC/Clang producers.
The separate GNU target object selects microMIPS or classic mode. High n64
addresses require Unicorn's virtual TLB; this does not test a hardware MMU.
Clang must include the NeverSight MIPS Fi8 function-pointer layout fix.
"""

import argparse
import importlib.util
import io
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE, UC_TLB_VIRTUAL

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest


FUNCTIONS = ("pointer_bits", "pointer_tag", "pointer_mask", "pointer_code",
             "pointer_offset", "pointer_equal", "call_pointer", "direct_call",
             "wide_call_pointer", "wide_direct_call")
CALLS = {
    "call_pointer": ("target", "int callee(int value);", True),
    "direct_call": ("target", "int callee(int value);", False),
    "wide_call_pointer": ("wide_target", "unsigned long long callee(unsigned long long value);", True),
    "wide_direct_call": ("wide_target", "unsigned long long callee(unsigned long long value);", False),
}
PRESERVED = {reg: 0x61395217f0e0d000 + reg
             for reg in (*range(16, 24), 28, 30)}
CALL_VIEWS_SPEC = importlib.util.spec_from_file_location(
    "mips_floating_abi", Path(__file__).with_name("floating-abi-roundtrip.py"))
CALL_VIEWS = importlib.util.module_from_spec(CALL_VIEWS_SPEC)
CALL_VIEWS_SPEC.loader.exec_module(CALL_VIEWS)


def observe(path, function, abi, little, compressed, literal_pointer=None):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    target_name = CALLS[function][0] if function in CALLS else "target"
    symbol = elf.get_section_by_name(".symtab").get_symbol_by_name(target_name)[0]
    # GNU emits either a tagged value or the compressed st_other mode bit.
    pointer = symbol["st_value"] | bool(symbol["st_other"]["local"] & 4)
    assert pointer & 1 == int(compressed), (path, pointer)
    mask = (1 << 64) - 1
    if abi == "n32" and pointer & 0x80000000:
        pointer |= 0xffffffff00000000
    expected_pointer = pointer if literal_pointer is None else literal_pointer
    if function in CALLS:
        wide = target_name == "wide_target"
        values = ((0, 1, 0x100000002, 0xfedcba9876543210,
                   0x8000000000000000, 0xffffffffffffffff) if wide
                  else (0, 1, -17, 0x10203040))
        cases = [(([pointer, value] if CALLS[function][2] else [value]),
                  (value ^ 0x81927364a5b6c7d8 if wide else value + 7) & mask)
                 for value in values]
    elif function == "pointer_equal":
        cases = [([pointer], 1), ([pointer ^ 1], 0), ([pointer + 16], 0)]
    else:
        expected = dict(pointer_bits=expected_pointer, pointer_tag=expected_pointer & 1,
                        pointer_mask=expected_pointer & 15, pointer_code=expected_pointer & ~1,
                        pointer_offset=expected_pointer + 4)[function]
        cases = [([], expected)]
    observations = []
    model = (roundtrip.mips.UC_CPU_MIPS64_MICROMIPS64R2_GENERIC if compressed
             else roundtrip.mips.UC_CPU_MIPS64_MIPS64R2_GENERIC)
    for arguments, expected in cases:
        cpu, entry = roundtrip.emulator(path, 64, little, function,
                                        cpu_model=model)
        cpu.ctl_set_tlb_mode(UC_TLB_VIRTUAL)
        # n32 ELF stores 32-bit VAs, while execution carries canonical
        # sign-extended pointers in its 64-bit registers. Map those exact
        # aliases for this fixed-address fixture's upper-half image.
        if abi == "n32" and entry & 0x80000000:
            entry |= 0xffffffff00000000
            pages = set()
            for segment in elf.iter_segments():
                if segment["p_type"] != "PT_LOAD":
                    continue
                begin = segment["p_vaddr"] | 0xffffffff00000000
                end = begin + segment["p_memsz"]
                for page in range(begin & ~4095, (end + 4095) & ~4095, 4096):
                    if page not in pages:
                        cpu.mem_map(page, 4096)
                        pages.add(page)
                cpu.mem_write(begin, segment.data())
        calls = []
        cpu.hook_add(UC_HOOK_CODE,
                     lambda uc, address, size, data: calls.append(address),
                     begin=pointer & ~1, end=pointer & ~1)
        result, memory = roundtrip.invoke(
            cpu, entry, 64, little, arguments, initial_registers=PRESERVED)
        assert result == expected, (path, abi, function, arguments, result, expected)
        assert len(calls) == int(function in CALLS), (path, calls)
        registers = {reg: cpu.reg_read(getattr(roundtrip.mips, f"UC_MIPS_REG_{reg}"))
                     for reg in PRESERVED}
        assert registers == PRESERVED, (path, function, registers)
        assert cpu.reg_read(roundtrip.mips.UC_MIPS_REG_SP) == roundtrip.STACK
        assert cpu.reg_read(roundtrip.mips.UC_MIPS_REG_RA) == roundtrip.STOP
        observations.append(dict(arguments=arguments, result=result,
                                 callee_entries=len(calls), memory=memory))
    return dict(pointer=pointer, observations=observations)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "gcc", "assembler", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sdk", type=Path)
    parser.add_argument("--abi", action="append", choices=("n32", "n64"))
    parser.add_argument("--function", action="append", choices=FUNCTIONS)
    parser.add_argument("--producer", action="append", choices=("gcc", "clang"))
    parser.add_argument("--original-only", action="store_true")
    args = parser.parse_args()
    if not args.original_only and not args.sdk:
        parser.error("--sdk is required for reconstructed call-site contracts")
    with MIPSValidationArtifacts(args.output, "wide-compressed-code-pointers",
                                 (args.neverd, args.clang, args.gcc,
                                  args.assembler, args.ld,
                                  *([args.sdk] if args.sdk else []))) as artifacts:
        work, rows, originals = artifacts.work, [], []
        controls = []
        views = CALL_VIEWS.CallSiteViews(args.sdk) if not args.original_only else None
        log = work / "command.log"
        (work / "string.h").write_text(
            "#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n")
        for abi in args.abi or ("n32", "n64"):
            for little in (False, True):
                endian = "-EL" if little else "-EB"
                target = "mips64el-linux-gnu" if little else "mips64-linux-gnu"
                flags = [endian, "-march=mips64r2", "-mabi=" + ("n32" if abi == "n32" else "64"),
                         "-mno-abicalls", "-fno-pic", "-mhard-float", "-mfp64",
                         "-G0", "-ffreestanding", "-fno-strict-aliasing", "-I", work]
                assemble = [args.assembler, endian, "-march=mips64r2",
                            "-mabi=" + ("n32" if abi == "n32" else "64")]
                emulation = ("elf" + ("32" if abi == "n32" else "64") +
                             ("l" if little else "b") + "tsmip" +
                             ("n32" if abi == "n32" else ""))
                link = [args.ld, "-m", emulation, "-static"]

                def command(argv):
                    roundtrip.run_command(artifacts, argv, log)

                def compile_source(source, obj, compiler, optimization):
                    assembly = work / "compile.s"
                    selected = getattr(args, compiler)
                    target_flags = ["-target", target, "-fno-addrsig"] if compiler == "clang" else []
                    command([selected, *target_flags, *flags, optimization,
                             "-g", "-S", source, "-o", assembly])
                    command([*assemble, "-o", obj, assembly])
                    artifacts.discard(assembly, log)

                for compressed in (True, False):
                    mode = "micromips" if compressed else "nomicromips"
                    attribute = f"__attribute__(({mode},noinline,aligned(16)))"
                    target_source, caller_source = work / "target.c", work / "original.c"
                    target_source.write_text(
                        f"{attribute} int target(int x) {{ return x + 7; }}\n" +
                        f"{attribute} unsigned long long wide_target(unsigned long long x) " +
                        "{ return x ^ 0x81927364a5b6c7d8ULL; }\n")
                    caller_source.write_text(
                        "typedef __UINTPTR_TYPE__ uptr;\n" +
                        f"{attribute} int target(int);\n" +
                        f"{attribute} unsigned long long wide_target(unsigned long long);\n" +
                        "uptr pointer_bits(void) { return (uptr)&target; }\n"
                        "uptr pointer_tag(void) { return (uptr)&target & 1; }\n"
                        "uptr pointer_mask(void) { return (uptr)&target & 15; }\n"
                        "uptr pointer_code(void) { return (uptr)&target & ~(uptr)1; }\n"
                        "uptr pointer_offset(void) { return (uptr)&target + 4; }\n"
                        "int pointer_equal(uptr p) { return p == (uptr)&target; }\n"
                        "int call_pointer(int (*callee)(int), int x) { return callee(x); }\n"
                        "int direct_call(int x) { return target(x); }\n"
                        "unsigned long long wide_call_pointer(unsigned long long (*callee)(unsigned long long), "
                        "unsigned long long x) { return callee(x); }\n"
                        "unsigned long long wide_direct_call(unsigned long long x) { return wide_target(x); }\n")
                    artifacts.keep_source(target_source)
                    artifacts.keep_source(caller_source)
                    target_object = work / "target.o"
                    compile_source(target_source, target_object, "gcc", "-O2")
                    addresses = [(0x100000, 0x200000)]
                    if abi == "n64":
                        addresses.append((0x120010000, 0x140010000))
                    else:
                        addresses.append((0x90010000, 0xb0010000))
                    for original_address, rebuilt_address in addresses:
                        for producer in args.producer or ("gcc", "clang"):
                            for original_opt in ("-O0", "-O2"):
                                original_object, original = work / "original.o", work / "original.elf"
                                compile_source(caller_source, original_object, producer, original_opt)
                                command([*link, "--emit-relocs", "-Ttext", hex(original_address), "-e", "pointer_bits",
                                         original_object, target_object, "-o", original])
                                for function in args.function or FUNCTIONS:
                                    before = observe(original, function, abi, little, compressed)
                                    context = dict(abi=abi, little=little, mode=mode, producer=producer,
                                                   original_optimization=original_opt, function=function,
                                                   original_address=original_address, original=before,
                                                   original_sha256=digest(original))
                                    originals.append(context)
                                    artifacts.write("originals.json", originals)
                                    if args.original_only:
                                        continue
                                    for backend in ("highc", "llvmc"):
                                        source = work / "reconstructed.c"
                                        if function in CALLS:
                                            elf = ELFFile(io.BytesIO(original.read_bytes()))
                                            entry = elf.get_section_by_name(".symtab").get_symbol_by_name(
                                                function)[0]["st_value"]
                                            source.write_text(views.source(
                                                original, "c" if backend == "highc" else backend,
                                                CALLS[function][1], [],
                                                CALLS[function][2], entry))
                                        else:
                                            command([args.neverd, "decompile", "--func", function,
                                                     *(["--llvm"] if backend == "llvmc" else []),
                                                     original, "-o", source])
                                        artifacts.keep_source(source)
                                        for compiler in ("gcc", "clang"):
                                            for optimization in ("-O0", "-O2"):
                                                obj, rebuilt = work / "rebuilt.o", work / "rebuilt.elf"
                                                compile_source(source, obj, compiler, optimization)
                                                command([*link, "-Ttext", hex(rebuilt_address), "-e", function,
                                                         obj, target_object, "-o", rebuilt])
                                                after = observe(rebuilt, function, abi, little, compressed)
                                                assert before["pointer"] != after["pointer"]
                                                assert [r["memory"] for r in before["observations"]] == [
                                                    r["memory"] for r in after["observations"]]
                                                rows.append(dict(**context, backend=backend, compiler=compiler,
                                                                 optimization=optimization,
                                                                 explicit_call_type=function in CALLS,
                                                                 source_sha256=digest(source),
                                                                 rebuilt_sha256=digest(rebuilt), rebuilt=after))
                                                artifacts.write("results.json", rows)
                                                artifacts.discard(obj, rebuilt, log)
                                        artifacts.discard(source, log)
                                if not args.original_only:
                                    literal = work / "literal.elf"
                                    command([*link, "-Ttext", hex(original_address), "-e", "pointer_bits",
                                             original_object, target_object, "-o", literal])
                                    before = observe(literal, "pointer_bits", abi, little, compressed)
                                    for backend in ("highc", "llvmc"):
                                        source = work / "literal.c"
                                        command([args.neverd, "decompile", "--func", "pointer_bits",
                                                 *(["--llvm"] if backend == "llvmc" else []),
                                                 literal, "-o", source])
                                        artifacts.keep_source(source)
                                        for compiler in ("gcc", "clang"):
                                            for optimization in ("-O0", "-O2"):
                                                obj, rebuilt = work / "literal.o", work / "literal-rebuilt.elf"
                                                compile_source(source, obj, compiler, optimization)
                                                command([*link, "-Ttext", hex(rebuilt_address), "-e", "pointer_bits",
                                                         obj, target_object, "-o", rebuilt])
                                                after = observe(rebuilt, "pointer_bits", abi, little,
                                                                compressed, before["pointer"])
                                                assert before["pointer"] != after["pointer"]
                                                controls.append(dict(abi=abi, little=little, mode=mode,
                                                    producer=producer, original_optimization=original_opt,
                                                    original_address=original_address, backend=backend,
                                                    compiler=compiler, optimization=optimization,
                                                    source_sha256=digest(source), original=before,
                                                    rebuilt=after, original_sha256=digest(literal),
                                                    rebuilt_sha256=digest(rebuilt)))
                                                artifacts.write("literal-controls.json", controls)
                                                artifacts.discard(obj, rebuilt, log)
                                        artifacts.discard(source, log)
                                    artifacts.discard(literal)
                                artifacts.discard(original_object, original, log)
                    artifacts.discard(target_object, target_source, caller_source)
        artifacts.write("summary.json", dict(
            complete=True, original_only=args.original_only,
            original_images=len({r["original_sha256"] for r in originals}),
            original_executions=sum(len(r["original"]["observations"]) for r in originals),
            rebuilt_images=len(rows),
            rebuilt_executions=sum(len(r["rebuilt"]["observations"]) for r in rows),
            literal_control_builds=len(controls),
            neverd_sha256=digest(args.neverd),
            sdk_sha256=digest(args.sdk) if args.sdk else None,
            memory_model="Unicorn virtual TLB, not hardware MMU qualification"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
