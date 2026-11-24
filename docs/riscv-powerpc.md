# RISC-V and PowerPC integer ELF analysis

## Experimental ELF profiles

| ELF machine | Class and byte order | ABI used for integer arguments and returns | Encoding |
| --- | --- | --- | --- |
| RISC-V | ELF32, little-endian | ILP32, a0–a7, result in a0 | RV32I, M, A, integer C |
| RISC-V | ELF64, little-endian | LP64, a0–a7, result in a0 | RV64I, M, A, integer C |
| PowerPC | ELF32, big- or little-endian | Classic System V, r3–r10, result in r3 | Fixed-width scalar subset |
| PowerPC64 | ELF64, big- or little-endian | Explicit ELFv2, r3–r10, result in r3 | Fixed-width scalar subset |

For RISC-V, the ELF class selects XLEN. The loader accepts the integer ABI and
reads normalized `Tag_RISCV_arch` attributes for M, A and their integer
subextensions. Zmmul permits multiplication, Zaamo permits atomic
read-modify-write operations, and Zalrsc permits load-reserved/store-conditional.
LLVM functions retain the evidenced features, including Zca on a Zca-only
image. An explicit stack alignment must match the 16-byte ILP32/LP64
convention. Either the RVC ELF
flag or the C/Zca attribute permits halfword instruction boundaries. Without
extension evidence the decoder uses the base integer profile. A 32-bit
instruction can follow a 16-bit instruction at a halfword address. The lifter
uses the physical instruction length for the link address.
[Unprivileged ISA, 20260120](https://docs.riscv.org/reference/isa/v20260120/unpriv/unpriv-index.html).
[RISC-V ELF psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/).

| PowerPC ABI | Admission and linkage |
| --- | --- |
| PPC32 System V | ELF32 and zero processor flags. The ninth integer argument begins eight bytes above the caller's stack pointer on entry. |
| PPC32 EABI | Unsupported. Embedded flags and VLE executable regions fail admission. |
| PPC64 ELFv2 | Requires ABI flag 2 in either byte order. The ninth integer argument begins at offset 96. The execution fixtures enter through the global entry with its address in r12 and require no incoming TOC. Cross-module TOC recovery, global/local entry equivalence and PLT calls remain unqualified. |
| PPC64 ELFv1 | Unsupported. Its function descriptors and TOC have a separate calling convention. An unspecified ELF64 ABI also fails admission. |

PPC64 arithmetic models the common CA, OV and SO bits. A full XER read
requires processor evidence for the ISA 3.0 CA32 and OV32 fields and is
currently rejected. [OpenPOWER ELFv2 ABI](https://openpower.foundation/specifications/64bitelfabi/).
[PowerPC ELFv1 ABI](https://refspecs.linuxfoundation.org/ELF/ppc64/PPC-elf64abi.html).
The scalar encodings and flag definitions follow Books I and II of versions
2.07b and 3.1c of the ISA. [OpenPOWER ISA](https://openpowerfoundation.org/specifications/isa/).

## Analysis coverage

| Stage | RISC-V | PowerPC |
| --- | --- | --- |
| Identify and load | ELF class, machine, flags and attributes | ELF class, machine, ABI flag and byte order |
| Decode and lift | Integer ALU and word operations, loads/stores, branches, jumps, multiply/divide, integer compressed forms, fences and atomics | Integer ALU, signed and unsigned word/doubleword division, CR compares and bit operations, LR/CTR transfers, carry and overflow forms, rotates/masks, ordinary and update-form loads/stores, reservations and barriers |
| CFG and calling convention | Direct and indirect jumps/calls, return through ra, integer register arguments | Direct and conditional branches, CTR loops, unconditional indirect calls/jumps, return through LR, integer register arguments |
| Source and LLVM | HighC, LLVM IR and LLVM-derived C for the qualified integer subset | Same stages in target byte order |
| Native rewrite | Unsupported | Unsupported |

JALR through x0 has a signed absolute target. In a stripped linked ELF,
function discovery also follows an AUIPC/JALR call when both instructions
belong to one reachable basic block and use the same base register.

PowerPC division preserves the quotient, the recorded CR0 comparison and the
OE update to XER. Recompiled C and LLVM use native division for the target's
choice of undefined bits, including division by zero and signed overflow.
Each divide executes separately even when the operands match.
The LowIR emulator tracks those bits as unknown. On PPC64, a successful word
division exposes its defined low word even when the full register and CR0
comparison depend on the processor.

RISC-V atomics retain width and acquire/release bits. HighC and LLVM-derived C
use target GNU inline assembly for ordering and reservation instructions. The
LowIR emulator evaluates aligned ordinary memory under a single-hart model.
A conditional store needs a known reservation state. Multi-hart interference,
device memory and guest exception delivery are outside that model.

The strict lifters report unsupported instructions at their original VA.
RISC-V system/CSR, FP, V and vendor extensions, and PowerPC VLE, SPE, VMX,
VSX, prefixed instructions, conditional indirect transfers and system
instructions have no qualified semantic path. Word multiplies with an
undefined high half on PPC64 also fail strict lifting.
Relative relocations write the link-time pointer in target byte order.
Full-width absolute relocations retain the symbol and addend. RISC-V and
ELFv2 jump slots retain their import binding. PPC32 jump slots can contain
instructions and need a separate PLT proof. FP arguments, variadic and
aggregate calls require separate ABI coverage. Link objects with relocations
against allocated sections before analysis. Raw firmware mapping and native
guest execution have separate entry points and coverage.

## Verification

The CTest suites exercise stripped, linked ELF, instruction widths, ABI
rejection, direct call targets and relocatable return addresses. They check
scalar results against the pinned Unicorn 2.1.4 and cover atomic old values,
reservations and ordering.

```sh
python3 scripts/build.py build-release --target NeverDELFIntegerTargetTests NeverDELFIntegerSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDELFInteger(Target|Semantic)Tests$' --output-on-failure --parallel 4
```

The cross-compiler fixture builds freestanding integer arithmetic, division,
branches, memory accesses, atomic fetch-add and a non-leaf call with reordered
arguments. A stripped RISC-V fixture disables linker relaxation and rebuilds
both the caller and its unsymbolized callee. PowerPC fixtures also consume
the quotient and CR0 bits from a recorded word divide, check sticky overflow
and count the two independent divides in the rebuilt machine code.
It recompiles HighC, LLVM-derived C and the complete LLVM module at `-O0`
and `-O2` for the same target. The GNU C17 rebuild checks the declared return
width. Single-function HighC can use an unprototyped external declaration.
Unicorn compares the result register and memory with the original calculation
and checks the stack pointer and the caller's saved registers. The fixture
needs Clang with the RISC-V and PowerPC backends and the LLVM IR dialect
emitted by the build, ELF LLD, pyelftools and
the Unicorn Python bindings. Use a matching compiler target and ABI when
rebuilding target-dependent source. For big-endian PowerPC64, pass
`-mabi=elfv2` when compiling either emitted C or LLVM IR.

```sh
python3 unittests/lift/elf-integer/roundtrip.py \
  --neverd build-release/bin/neverd --clang /path/to/clang --ld /path/to/ld.lld
```
