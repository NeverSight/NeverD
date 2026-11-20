"""Execute genuine GCC MIPS16 indirect-call stubs and optionally rebuilt callers.

The GCC runtime source directory supplies mips16.S and its configured headers.
GNU GCC compiles the caller and each target separately. GNU as and ld produce
the linked o32 image. The reference checks the function pointer's mode, both
parts of the returned double, the caller's stack and its preserved s2.
"""
import argparse
import importlib.util
import io
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import mips_const as mips

import roundtrip
from mips_validation_artifacts import MIPSValidationArtifacts, digest

spec = importlib.util.spec_from_file_location(
    "floating_views", Path(__file__).with_name("floating-abi-roundtrip.py"))
views = importlib.util.module_from_spec(spec)
spec.loader.exec_module(views)

VALUE = """typedef unsigned int u32;
typedef unsigned long long u64;
typedef union { u64 u; double d; } bits;
"""
FIXED_TARGET = "double target(double value) { return value; }"
VAR_TARGET = """
double target(unsigned which, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, which);
  double a = __builtin_va_arg(ap, double);
  double b = __builtin_va_arg(ap, double);
  __builtin_va_end(ap);
  return which & 1 ? a : b;
}
"""


def caller(variadic):
    arguments = "unsigned, ..." if variadic else "double"
    call = "fn(which, a.d, b.d)" if variadic else "fn(a.d)"
    return VALUE + f"""
u32 kernel(double (*fn)({arguments}), u32 hi, u32 lo, u32 which) {{
  bits a = {{ .u = ((u64)hi << 32) | lo }};
  bits b = {{ .u = ((u64)lo << 32) | (hi ^ 0x13572468u) }};
  bits r = {{ .d = {call} }};
  return (u32)r.u ^ ((u32)(r.u >> 32) >> 3);
}}
"""


def observe(path, little, fp64, compressed, variadic):
    cpu, entry = roundtrip.emulator(path, 32, little, "kernel",
                                    cpu_model=mips.UC_CPU_MIPS32_74KF)
    cpu.reg_write(mips.UC_MIPS_REG_CP0_STATUS,
                  (cpu.reg_read(mips.UC_MIPS_REG_CP0_STATUS) & ~0x04000000)
                  | (0x24000000 if fp64 else 0x20000000))
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    symbol, = elf.get_section_by_name(".symtab").get_symbol_by_name("target")
    target = symbol["st_value"] | int(compressed)
    cpu.mem_write(roundtrip.STOP, (0x24030051).to_bytes(4, "little" if little else "big"))
    rows = []
    for hi, lo in [(0, 1), (0x80000000, 0), (0x3ff80000, 0x12345678),
                   (0x7ff80000, 0x13579bdf)]:
        for which in (0, 1):
            for i, value in enumerate((target, hi, lo, which)):
                cpu.reg_write(mips.UC_MIPS_REG_A0 + i, value)
            cpu.reg_write(mips.UC_MIPS_REG_S2, 0xa5181818)
            cpu.reg_write(mips.UC_MIPS_REG_SP, roundtrip.STACK)
            cpu.reg_write(mips.UC_MIPS_REG_RA, roundtrip.STOP)
            cpu.emu_start(entry, roundtrip.STOP + 4, count=10000)
            high, low = (lo, hi ^ 0x13572468) if variadic and not which else (hi, lo)
            value = cpu.reg_read(mips.UC_MIPS_REG_V0)
            assert value == (low ^ (high >> 3)), (hi, lo, which, value)
            assert cpu.reg_read(mips.UC_MIPS_REG_V1) == 0x51
            assert cpu.reg_read(mips.UC_MIPS_REG_S2) == 0xa5181818
            assert cpu.reg_read(mips.UC_MIPS_REG_SP) == roundtrip.STACK
            rows.append(value)
    return rows


def run(args, artifacts):
    sdk = views.CallSiteViews(args.sdk) if args.sdk else None
    evidence = []
    work = artifacts.work
    log = work / "command.log"

    def command(argv):
        roundtrip.run_command(artifacts, argv, log)

    for little in (False, True):
        endian = "-EL" if little else "-EB"
        for fp64 in (False, True):
            fp = "-mfp64" if fp64 else "-mfp32"
            flags = ["-mabi=32", "-march=mips32r2", "-mhard-float", fp, endian,
                     "-mno-abicalls", "-fno-pic", "-G0"]
            def compile_c(source, name, compressed):
                c, asm, obj = (work / (name + ext) for ext in (".c", ".s", ".o"))
                c.write_text(source)
                command([args.gcc, *flags, "-ffreestanding", "-fno-stack-protector",
                         "-O2", "-g", "-mips16" if compressed else "-mno-mips16",
                         "-S", c, "-o", asm])
                command([args.assembler, *flags[:2], fp, endian, asm, "-o", obj])
                artifacts.discard(c, asm)
                return obj
            runtime, asm = work / "runtime.o", work / "runtime.s"
            command([args.clang, "-target", "mipsel-linux-gnu" if little else "mips-linux-gnu",
                     *flags, "-DL_m16stubdf2", "-DL_m16stubdf0", "-DL_m16retdf",
                     "-I", args.runtime, "-E", "-P", "-x", "assembler-with-cpp",
                     args.runtime / "mips16.S", "-o", asm])
            command([args.assembler, "-mabi=32", "-march=mips32r2", fp, endian,
                     asm, "-o", runtime])
            artifacts.discard(asm)
            for variadic in (False, True):
                original_caller = compile_c(caller(variadic), "caller", True)
                for compressed in (False, True):
                    callee = compile_c(VAR_TARGET if variadic else FIXED_TARGET,
                                       "callee", compressed)
                    def link(obj, name):
                        elf = work / name
                        command([args.ld, "-m", "elf32ltsmip" if little else "elf32btsmip",
                                 "-Ttext", "0x10000", "-e", "kernel", obj, callee, runtime,
                                 "-o", elf])
                        return elf
                    elf = link(original_caller, "original.elf")
                    profile = dict(little=little, fp64=fp64, variadic=variadic,
                                   compressed_target=compressed)
                    values = observe(elf, little, fp64, compressed, variadic)
                    evidence.append(dict(**profile, route="original", elf=digest(elf),
                                         values=values))
                    artifacts.write("observations.json", evidence)
                    if sdk:
                        symbol, = ELFFile(io.BytesIO(elf.read_bytes())).get_section_by_name(
                            ".symtab").get_symbol_by_name("kernel")
                        for route in ("c", "llvmc"):
                            declaration = ("double target(unsigned, ...);" if variadic else
                                           "double target(double);")
                            source = sdk.source(elf, route, declaration,
                                                ["double", "double"] if variadic else [],
                                                False, entry=symbol["st_value"] & ~1)
                            obj = compile_c(source, "rebuilt", True)
                            rebuilt = link(obj, "rebuilt.elf")
                            result = observe(rebuilt, little, fp64, compressed, variadic)
                            assert result == values
                            evidence.append(dict(**profile, route=route, values=result))
                            artifacts.write("observations.json", evidence)
                            artifacts.discard(obj, rebuilt)
                    artifacts.discard(callee, elf)
                artifacts.discard(original_caller)
            artifacts.discard(runtime)
    print(f"{len(evidence)} images, {sum(len(row['values']) for row in evidence)} executions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("gcc", "clang", "assembler", "ld", "runtime", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--sdk", type=Path)
    args = parser.parse_args()
    with MIPSValidationArtifacts(args.output, "mips16-indirect-call",
                                 (args.gcc, args.clang, args.assembler, args.ld,
                                  args.runtime / "mips16.S")) as artifacts:
        run(args, artifacts)
