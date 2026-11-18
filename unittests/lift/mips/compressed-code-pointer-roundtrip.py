#!/usr/bin/env python3
"""Execute original and reconstructed o32 compressed callable identities.

Requires a Clang with the NeverSight MIPS Fi8 layout fix, GNU MIPS as/ld,
pyelftools and pinned Unicorn bindings/library. Each rebuilt function links
against the separately compiled callee at a different image address. Compare
symbol-relative observations, not coincidentally equal absolute addresses.
Linked address fragments retain --emit-relocs provenance. A separate control
without those records must keep the original literal bits, even when an equal
function symbol is present.
"""

import argparse
import importlib.util
import io
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest


FUNCTIONS = ("pointer_bits", "pointer_tag", "pointer_mask", "pointer_code",
             "pointer_offset", "pointer_equal", "call_pointer")
VALUES = (0, 1, -7, 0x10203040)
PRESERVED = {reg: 0x6a000000 + reg * 0x10101 for reg in (*range(16, 24), 30)}
CALL_VIEWS_SPEC = importlib.util.spec_from_file_location(
    "mips_floating_abi", Path(__file__).with_name("floating-abi-roundtrip.py"))
CALL_VIEWS = importlib.util.module_from_spec(CALL_VIEWS_SPEC)
CALL_VIEWS_SPEC.loader.exec_module(CALL_VIEWS)


def callable_address(path):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    symbol = elf.get_section_by_name(".symtab").get_symbol_by_name("target")[0]
    # GNU emits either a tagged value or the compressed st_other mode bit.
    return symbol["st_value"] | bool(symbol["st_other"]["local"] & 4)


def observe(path, function, little, compressed, literal_pointer=None):
    pointer = callable_address(path)
    assert pointer & 1 == int(compressed), (path, pointer)
    expected_pointer = pointer if literal_pointer is None else literal_pointer
    if function == "call_pointer":
        cases = [([pointer, value], (value + 7) & 0xffffffff)
                 for value in VALUES]
    elif function == "pointer_equal":
        cases = [([pointer], 1), ([pointer ^ 1], 0), ([pointer + 16], 0)]
    else:
        expected = {"pointer_bits": expected_pointer,
                    "pointer_tag": expected_pointer & 1,
                    "pointer_mask": expected_pointer & 15,
                    "pointer_code": expected_pointer & ~1,
                    "pointer_offset": expected_pointer + 4}[function]
        cases = [([], expected)]
    results = []
    for arguments, expected in cases:
        cpu, entry = roundtrip.emulator(path, 32, little, function)
        calls = []
        if function == "call_pointer":
            cpu.hook_add(UC_HOOK_CODE,
                         lambda uc, address, size, data: calls.append(address),
                         begin=pointer & ~1, end=pointer & ~1)
        result, memory = roundtrip.invoke(
            cpu, entry, 32, little, arguments, initial_registers=PRESERVED)
        assert result == expected, (path, function, arguments, result, expected)
        assert len(calls) == int(function == "call_pointer"), (path, calls)
        preserved = {reg: cpu.reg_read(getattr(roundtrip.mips, f"UC_MIPS_REG_{reg}"))
                     for reg in PRESERVED}
        assert preserved == PRESERVED, (path, function, preserved)
        assert cpu.reg_read(roundtrip.mips.UC_MIPS_REG_SP) == roundtrip.STACK
        assert cpu.reg_read(roundtrip.mips.UC_MIPS_REG_RA) == roundtrip.STOP
        results.append(dict(arguments=arguments, result=result, expected=expected,
                            callee_entries=len(calls), preserved=preserved,
                            memory=memory))
    return dict(pointer=pointer, observations=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("neverd", "sdk", "clang", "assembler", "ld", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--mode", action="append",
                        choices=("micromips", "mips16", "nomips16"))
    parser.add_argument("--function", action="append", choices=FUNCTIONS)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "compressed-numeric-code-pointers",
                                 (args.neverd, args.sdk, args.clang, args.assembler,
                                  args.ld)) as artifacts:
        views = CALL_VIEWS.CallSiteViews(args.sdk)
        work = artifacts.work
        log = work / "command.log"
        (work / "string.h").write_text(
            "#include <stddef.h>\nvoid *memcpy(void *, const void *, size_t);\n")
        rows, literal_controls = [], []
        for mode in args.mode or ("micromips", "mips16", "nomips16"):
            attribute = f"__attribute__(({mode},noinline,aligned(16)))"
            declaration = f"{attribute} int target(int);\n"
            target_source = work / "target.c"
            target_source.write_text(
                f"{attribute} int target(int x) {{ return x + 7; }}\n")
            original_source = work / "original.c"
            original_source.write_text(
                "typedef __UINTPTR_TYPE__ uptr;\n" + declaration +
                "uptr pointer_bits(void) { return (uptr)&target; }\n"
                "uptr pointer_tag(void) { return (uptr)&target & 1; }\n"
                "uptr pointer_mask(void) { return (uptr)&target & 15; }\n"
                "uptr pointer_code(void) { return (uptr)&target & ~(uptr)1; }\n"
                "uptr pointer_offset(void) { return (uptr)&target + 4; }\n"
                "int pointer_equal(uptr p) { return p == (uptr)&target; }\n"
                "int call_pointer(int (*callee)(int), int x) { return callee(x); }\n")
            artifacts.keep_source(target_source)
            artifacts.keep_source(original_source)
            for little in (False, True):
                flags = ["-target", "mipsel-linux-gnu" if little else "mips-linux-gnu",
                         "-march=mips32r2", "-mabi=32", "-mhard-float",
                         "-mno-abicalls", "-fno-pic", "-fno-addrsig", "-G0",
                         "-ffreestanding", "-fno-strict-aliasing", "-I", work]
                assemble = [args.assembler, "-EL" if little else "-EB",
                            "-march=mips32r2"]
                link = [args.ld, "-m", "elf32ltsmip" if little else "elf32btsmip",
                        "-static"]

                def compile_source(source, obj, optimization):
                    assembly = work / "compile.s"
                    roundtrip.run_command(artifacts, [args.clang, *flags,
                        optimization, "-g", "-S", source, "-o", assembly], log)
                    roundtrip.run_command(artifacts, [*assemble, "-o", obj,
                                                      assembly], log)
                    artifacts.discard(assembly, log)

                target_object = work / "target.o"
                compile_source(target_source, target_object, "-O2")
                for original_opt in ("-O0", "-O2"):
                    original_object, original = work / "original.o", work / "original.elf"
                    compile_source(original_source, original_object, original_opt)
                    roundtrip.run_command(artifacts, [*link, "--emit-relocs", "-Ttext", "0x100000",
                        "-e", "pointer_bits", original_object, target_object,
                        "-o", original], log)
                    for function in args.function or FUNCTIONS:
                        before = observe(original, function, little, mode != "nomips16")
                        for backend in ("highc", "llvmc"):
                            source = work / "reconstructed.c"
                            if function == "call_pointer":
                                # A saved FP is private only with a complete
                                # outgoing-call extent. Supply the original
                                # declaration through the public occurrence
                                # edit, as the floating ABI runtime does.
                                elf = ELFFile(io.BytesIO(original.read_bytes()))
                                entry = elf.get_section_by_name(".symtab").get_symbol_by_name(
                                    function)[0]["st_value"]
                                source.write_text(views.source(
                                    original, backend, "int callee(int value);",
                                    [], True, entry))
                            else:
                                roundtrip.run_command(artifacts, [args.neverd, "decompile",
                                    "--func", function,
                                    *(["--llvm"] if backend == "llvmc" else []),
                                    original, "-o", source], log)
                            artifacts.keep_source(source)
                            for optimization in ("-O0", "-O2"):
                                obj, rebuilt = work / "rebuilt.o", work / "rebuilt.elf"
                                compile_source(source, obj, optimization)
                                roundtrip.run_command(artifacts, [*link, "-Ttext", "0x200000",
                                    "-e", function, obj, target_object, "-o", rebuilt], log)
                                after = observe(rebuilt, function, little, mode != "nomips16")
                                assert before["pointer"] != after["pointer"]
                                assert [row["memory"] for row in before["observations"]] == [
                                    row["memory"] for row in after["observations"]]
                                rows.append(dict(mode=mode, little=little,
                                    original_optimization=original_opt, function=function,
                                    backend=backend, optimization=optimization,
                                    explicit_call_type=function == "call_pointer",
                                    source_sha256=digest(source), original_sha256=digest(original),
                                    rebuilt_sha256=digest(rebuilt), original=before, rebuilt=after))
                                artifacts.write("results.json", rows)
                                artifacts.discard(obj, rebuilt, log)
                            artifacts.discard(source, log)
                    # Removing the relocation evidence must not promote a
                    # coincidentally equal integer into a relocated pointer.
                    literal = work / "literal.elf"
                    roundtrip.run_command(artifacts, [*link, "-Ttext", "0x100000",
                        "-e", "pointer_bits", original_object, target_object,
                        "-o", literal], log)
                    original_pointer = callable_address(literal)
                    for backend in ("highc", "llvmc"):
                        source = work / "literal.c"
                        roundtrip.run_command(artifacts, [args.neverd, "decompile",
                            "--func", "pointer_bits",
                            *(["--llvm"] if backend == "llvmc" else []),
                            literal, "-o", source], log)
                        artifacts.keep_source(source)
                        for optimization in ("-O0", "-O2"):
                            obj, rebuilt = work / "literal.o", work / "literal-rebuilt.elf"
                            compile_source(source, obj, optimization)
                            roundtrip.run_command(artifacts, [*link, "-Ttext", "0x200000",
                                "-e", "pointer_bits", obj, target_object, "-o", rebuilt], log)
                            after = observe(rebuilt, "pointer_bits", little,
                                            mode != "nomips16", original_pointer)
                            assert after["pointer"] != original_pointer
                            literal_controls.append(dict(mode=mode, little=little,
                                original_optimization=original_opt, backend=backend,
                                optimization=optimization, original_pointer=original_pointer,
                                source_sha256=digest(source), original_sha256=digest(literal),
                                rebuilt_sha256=digest(rebuilt), rebuilt=after))
                            artifacts.write("literal-controls.json", literal_controls)
                            artifacts.discard(obj, rebuilt, log)
                        artifacts.discard(source)
                    artifacts.discard(original_object, original, literal, log)
                artifacts.discard(target_object)
            artifacts.discard(target_source, original_source)
        artifacts.write("summary.json", dict(complete=True, builds=len(rows),
            independent_rebuilt_executions=sum(len(row["rebuilt"]["observations"]) for row in rows),
            literal_control_builds=len(literal_controls),
            source_compiler_sha256=digest(args.clang), neverd_sha256=digest(args.neverd),
            sdk_sha256=digest(args.sdk)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
