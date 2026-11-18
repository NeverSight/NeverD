#!/usr/bin/env python3
"""Execute n32/n64 scalar carrier observations and logical-width controls.

Inline-assembly originals observe the compiler's GPR operand bits. The compiler
may materialize narrow operands before the assembly. Ordinary C controls
separately observe logical zero/sign extension. GCC and Clang compile originals
and both reconstructed source backends; the runner checks complete registers.
"""
import argparse
from pathlib import Path

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest

FUNCTIONS = {
    "raw_u8": ("unsigned char", 8, False),
    "raw_s8": ("signed char", 8, True),
    "raw_u16": ("unsigned short", 16, False),
    "raw_s16": ("short", 16, True),
    "raw_u32": ("unsigned int", 32, False),
    "raw_s32": ("int", 32, True),
    "raw_pointer": ("void *", 0, False),
    "logical_u32": ("unsigned int", 32, False),
    "logical_s32": ("int", 32, True),
}
MASK = (1 << 64) - 1
PRESERVED = {r: 0x71325496f9e8d000 + r for r in (*range(16, 24), 28, 30)}

def cases(function, abi):
    _, bits, signed = FUNCTIONS[function]
    bits = bits or (32 if abi == "n32" else 64)
    values = (0, 1, (1 << (bits-1))-1, 1 << (bits-1), (1 << bits)-1)
    for value in values:
        sign_extend = signed or bits == 32
        argument = value | (MASK ^ ((1 << bits)-1)) if sign_extend and value & (1 << (bits-1)) else value
        result = value if function == "logical_u32" else argument
        yield argument, result, bits

def observe(path, function, abi, little, reference=None):
    rows = []
    for index, (argument, logical_expected, bits) in enumerate(cases(function, abi)):
        cpu, entry = roundtrip.emulator(
            path, 64, little, function,
            cpu_model=roundtrip.mips.UC_CPU_MIPS64_MIPS64R2_GENERIC)
        value, memory = roundtrip.invoke(cpu, entry, 64, little, [argument],
                                         initial_registers=PRESERVED)
        # A narrow inline-asm operand guarantees its logical low bits, not
        # an unchanged ABI register above those bits. Compare every rebuilt
        # bit against the independent compiler's actual original program.
        assert value & ((1 << bits)-1) == argument & ((1 << bits)-1)
        expected = (reference[index]["result"] if reference is not None else
                    logical_expected if function.startswith("logical_") else None)
        if expected is not None:
            assert value == expected, (path, function, hex(argument), hex(value), hex(expected))
        registers = {r: cpu.reg_read(getattr(roundtrip.mips, f"UC_MIPS_REG_{r}")) for r in PRESERVED}
        assert registers == PRESERVED, (path, function, registers)
        assert cpu.reg_read(roundtrip.mips.UC_MIPS_REG_SP) == roundtrip.STACK
        assert cpu.reg_read(roundtrip.mips.UC_MIPS_REG_RA) == roundtrip.STOP
        rows.append(dict(argument=argument, result=value, memory=memory))
    return rows

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "clang", "gcc", "assembler", "ld", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--abi", action="append", choices=("n32", "n64"))
    parser.add_argument("--function", action="append", choices=FUNCTIONS)
    parser.add_argument("--producer", action="append", choices=("gcc", "clang"))
    parser.add_argument("--backend", action="append", choices=("highc", "llvmc"))
    parser.add_argument("--original-only", action="store_true")
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "scalar-entry-carriers",
                                 (args.neverd, args.clang, args.gcc, args.assembler, args.ld)) as artifacts:
        work = artifacts.work
        log = work / "command.log"
        source = work / "original.c"
        text = ""
        for name, (kind, _, _) in FUNCTIONS.items():
            body = ('unsigned long long bits; __asm__("daddu %0, %1, $0" '
                    ': "=r"(bits) : "r"(value)); return bits;')
            if name.startswith("logical_"):
                body = "return value;"
            text += f"unsigned long long {name}({kind} value) {{ {body} }}" + "\n"
        source.write_text(text)
        artifacts.keep_source(source)
        (work / "string.h").write_text(
            "#include <stddef.h>" + chr(10) +
            "void *memcpy(void *, const void *, size_t);" + chr(10))
        originals, rows = [], []
        def command(argv):
            roundtrip.run_command(artifacts, argv, log)
        for abi in args.abi or ("n32", "n64"):
            for little in (False, True):
                endian = "-EL" if little else "-EB"
                flags = [endian, "-march=mips64r2", "-mabi="+("n32" if abi == "n32" else "64"),
                         "-mno-abicalls", "-fno-pic", "-mhard-float", "-mfp64", "-G0",
                         "-ffreestanding", "-fno-strict-aliasing", "-I", work]
                assemble = [args.assembler, endian, "-march=mips64r2",
                            "-mabi="+("n32" if abi == "n32" else "64")]
                emulation = "elf"+("32" if abi == "n32" else "64")+("l" if little else "b")+"tsmip"+("n32" if abi == "n32" else "")
                def compile(src, obj, compiler, opt):
                    asm = work / "compile.s"
                    target = (["-target", "mips64el-linux-gnu" if little else "mips64-linux-gnu", "-fno-addrsig"]
                              if compiler == "clang" else [])
                    command([getattr(args,compiler), *target, *flags, opt, "-g", "-S", src, "-o", asm])
                    command([*assemble, "-o", obj, asm])
                    artifacts.discard(asm, log)
                def link(obj, elf, entry):
                    command([args.ld, "-m", emulation, "-static", "-Ttext", "0x100000", "-e", entry, obj, "-o", elf])
                for producer in args.producer or ("gcc", "clang"):
                    for original_opt in ("-O0", "-O2"):
                        obj, original = work/"original.o", work/"original.elf"
                        compile(source, obj, producer, original_opt)
                        link(obj, original, "raw_u32")
                        for function in args.function or FUNCTIONS:
                            before = observe(original, function, abi, little)
                            context = dict(abi=abi, little=little, function=function,
                                           producer=producer, original_optimization=original_opt,
                                           original_sha256=digest(original), original=before)
                            originals.append(context);artifacts.write("originals.json", originals)
                            if args.original_only:
                                continue
                            for backend in args.backend or ("highc", "llvmc"):
                                rebuilt_source = work / "reconstructed.c"
                                command([args.neverd, "decompile", "--func", function,
                                         *(["--llvm"] if backend == "llvmc" else []),
                                         original, "-o", rebuilt_source])
                                artifacts.keep_source(rebuilt_source)
                                for compiler in ("gcc", "clang"):
                                    for opt in ("-O0", "-O2"):
                                        rebuilt_obj, rebuilt = work/"rebuilt.o", work/"rebuilt.elf"
                                        compile(rebuilt_source, rebuilt_obj, compiler, opt)
                                        link(rebuilt_obj, rebuilt, function)
                                        after = observe(rebuilt, function, abi, little, before)
                                        assert after == before
                                        rows.append(dict(**context, backend=backend,
                                                         compiler=compiler, optimization=opt,
                                                         source_sha256=digest(rebuilt_source),
                                                         rebuilt_sha256=digest(rebuilt), rebuilt=after))
                                        artifacts.write("results.json", rows)
                                        artifacts.discard(rebuilt_obj, rebuilt, log)
                                artifacts.discard(rebuilt_source)
                        artifacts.discard(obj, original)
        artifacts.write("summary.json", dict(complete=True, original_only=args.original_only,
                                    original_images=len({r["original_sha256"] for r in originals}),
                                    original_executions=sum(len(r["original"]) for r in originals),
                                    rebuilt_images=len(rows),
                                    rebuilt_executions=sum(len(r["rebuilt"]) for r in rows),
                                    neverd_sha256=digest(args.neverd)))
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
