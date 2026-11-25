# RISC-V and PowerPC integer ELF analysis

## Experimental ELF profiles

| ELF machine | Class and byte order | ABI used for integer arguments and returns | Encoding |
| --- | --- | --- | --- |
| RISC-V | ELF32, little-endian | ILP32, a0–a7, word result in a0, double-word result in a0/a1 | RV32I, M, A, integer C |
| RISC-V | ELF64, little-endian | LP64, a0–a7, result in a0 | RV64I, M, A, integer C |
| PowerPC | ELF32, big- or little-endian | Classic System V, r3–r10, word result in r3, double-word result in r3/r4 | Fixed-width scalar subset |
| PowerPC64 | ELF64, big- or little-endian | ELFv1 or explicit ELFv2, r3–r10, result in r3 | Fixed-width scalar subset |

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

Linked RV32 and RV64 executables and shared libraries support the default
psABI PLT. The loader reads `R_RISCV_JUMP_SLOT` from the program's dynamic
table. To name an external call, it checks that the PLT entry loads that
slot and that PLT0 computes the same relocation index for lazy binding.
This works with section headers removed and with linker relaxation enabled
or disabled. Variant calling conventions and landing-pad PLTs have separate
instruction and ABI requirements and are outside this profile.

| PowerPC ABI | Admission and linkage |
| --- | --- |
| PPC32 System V | ELF32 and zero processor flags. The ninth integer argument begins eight bytes above the caller's stack pointer on entry. Cross-module calls use a secure PLT or a classic BSS-PLT. |
| PPC32 EABI | Unsupported. Embedded flags and VLE executable regions fail admission. |
| PPC64 ELFv2 | Requires ABI flag 2 in either byte order. The ninth integer argument begins at offset 96. Function symbols specify the local entry and r2 preservation. A global call to a function with two entries supplies its address in r12. A local call supplies the callee’s TOC in r2. |
| PPC64 ELFv1 | A linked image identifies its descriptor table with an allocated `.opd` section or the paired `DT_PPC64_OPD` and `DT_PPC64_OPDSZ` tags. Flags 0 and 1 admit that layout in either byte order. The ninth integer argument begins at offset 112. |

For PPC32, the loader reads `R_PPC_JMP_SLOT` from the runtime relocation table.
With `DT_PPC_GOT`, a secure PLT slot holds a four-byte function pointer.
The loader matches the stub's load into r11 and final transfer through CTR.
For a PIC stub, MedIR uses the r30 value reaching each call to identify the
slot. In the classic BSS-PLT, the runtime linker writes instructions into
the entry. Its relocation ordinal determines the entry's address and size.
Both layouts recover imports from a linked image with its section headers
removed. GNU ld can place private data and BSS-PLT code in the same writable,
executable mapping. A PIC data access into that mapping needs section bounds
to distinguish the original data from code. Separate data and linkage
mappings retain that distinction after stripping.
[GNU ld PowerPC ELF32 options](https://sourceware.org/binutils/docs/ld/PowerPC-ELF32.html).

An ELFv2 function pointer names the global entry. The loader reads the local
entry offset from `st_other` and follows the global prologue to determine the
TOC for that function. Discovery gives both entries one function identity.
MedIR checks the incoming r2 at local calls and r12 at calls to a distinct
global entry. For a single-entry symbol, encodings 0 and 1 distinguish a
function that preserves r2 from one that can change it.

An ELFv2 PLT slot holds an eight-byte function pointer. The loader matches
the stub's load into r12 and transfer through CTR to its runtime
`R_PPC64_JMP_SLOT`. MedIR models the r2 save at 24(r1) and its reaching
reload after the call. This also handles a GNU linker stub that adjusts r2
before branching to a local entry in a different TOC group. Both source
backends emit the callee's name, allowing the native linker to select the
entry and establish the provider's TOC in the rebuilt binary. For an optimized
GNU PLT call without an r2 save, the dynamic local-entry flag and the
imported global function symbol must both establish r2 preservation.
[OpenPOWER ELFv2 ABI](https://openpowerfoundation.org/specifications/64bitelfabi/).

An ELFv1 function pointer is the address of a descriptor. The loader retains
that address and reads the descriptor's code address, TOC and optional static
chain in target byte order. The ELF entry point, function symbols, exports and
initializer arrays lead discovery to the code. A stripped table can contain
both 24-byte entries and 16-byte entries with an omitted environment word.
The loader requires a unique layout and an executable target for each entry.
[ELFv1 function descriptors](https://refspecs.linuxfoundation.org/ELF/ppc64/PPC-elf64abi.html).
[GNU ld descriptor layout](https://sourceware.org/binutils/docs/ld/PowerPC64-ELF64.html).

The incoming r2 belongs to the particular code entry. A signed displacement
from this TOC can address mapped storage even when r2 itself lies beyond the
image. A descriptor call through CTR uses the code, TOC and static chain
loaded from the same descriptor. A relocated function-pointer variable keeps
its writable storage and starts at the rebuilt function's native descriptor.
A linked executable can retain this evidence with `--emit-relocs`. A call
through an unrelocated descriptor-valued initializer reports the missing
relocation.

For an ELFv1 call through the PLT, the loader pairs the executable stub with
the inline descriptor named by a runtime `R_PPC64_JMP_SLOT` relocation. The
stub's entry, TOC and optional static-chain loads must refer to the same slot.
For the GNU thread-safe form, the fallback branch must reach that slot's
`.glink` entry. HighC and LLVM emit a call to the imported function, so the
rebuilt program uses its native linker's binding and TOC switch.
GNU ld can also move the r2 save into the caller's prologue with
`R_PPC64_TOCSAVE`. Recovery follows the saved stack word to the reload and
requires a unique reaching value across the returning paths.

A GNU multi-TOC stub can save the caller's r2, adjust it and branch directly
to a local function with a different TOC. MedIR checks the reaching r2 against
the callee's descriptor and models the ABI save at 40(r1) and immediate
restore on return. Unresolved descriptor targets, ambiguous TOCs and
unrecognized save/restore sequences require additional evidence.
[GNU ld PowerPC64 options](https://sourceware.org/binutils/docs/ld/PowerPC64-ELF64.html).

PPC scalar population counts preserve each byte or word lane independently.
PPC64 also admits the doubleword count. A conditional branch through LR can
test CR, decrement CTR, or do both. The not-taken path retains the updated
CTR. Source recovery requires the incoming return address to reach the branch
on every path. A reaching call or explicit LR write, including an unproven
save/restore, leaves that source return unresolved.

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

When selected on its own, a straight-line RV32 or PPC32 leaf can establish a
64-bit sum from the first two argument pairs. Both result registers must hold
the corresponding sums and the low addition's carry must reach the high
addition. Word spills and reloads can carry the inputs through the leaf's
own frame. PPC32 places the high word in r3 on big endian and r4 on little
endian. The source projections can retain four word-sized parameters.

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
VSX, prefixed instructions in ordinary function bodies, conditional indirect
transfers and system instructions have no qualified semantic path. The ELFv2
loader recognizes the PC-relative prefixed load or address calculation in a
linker stub when its final CTR transfer and destination match. Word multiplies with an
undefined high half on PPC64 also fail strict lifting.
Relative relocations write the link-time pointer in target byte order.
Full-width absolute relocations retain the symbol and addend. RISC-V and
ELFv2 jump slots retain their import binding. PPC32 PIC stubs require a
proven r30-relative jump slot. FP arguments, variadic and
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
arguments. A separately compiled caller checks both halves of a 64-bit sum
across carry and wraparound, varying the original and rebuild optimization
levels independently.
A stripped RISC-V fixture disables linker relaxation and rebuilds
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

The RISC-V PLT fixture builds two independent providers and checks ordinary
calls and tail calls. It runs both the original and the rebuilt caller at
different module bases, with eager and lazy binding. The lazy resolver checks
the relocation index and link map before rebinding the slot. Run it with ELF
LLD or GNU ld with the `elf32lriscv` and `elf64lriscv` emulations.

```sh
python3 unittests/lift/elf-integer/riscv_plt.py \
  --neverd build-release/bin/neverd --clang /path/to/clang --ld /path/to/ld.lld
```

The ELFv1 qualification uses Clang with `-mabi=elfv1` in both byte orders,
a GNU PowerPC64 linker with `elf64ppc` and `elf64lppc` emulations, and the
same Python packages as the integer roundtrip above. Rebuilt programs link at
a different address. Single-function LLVM C can declare the loaded image as
external storage. The test supplies its data from the input ELF and lets GNU ld
apply the retained function-pointer relocation at the new address. The
fixtures check mutable TOC data, taking a function's
address, calling and replacing a stored pointer, the ninth integer argument
and a callback with its own TOC and static chain.

The PLT fixture links a separate provider at another base, exercises eager
and lazy binding, and verifies the caller's r2 and its data after return.
It includes a stripped shared-library caller relocated at run time, a
provider that reads its static chain, a caller with a prologue TOC save,
and two local functions in different TOC groups. Both
fixtures execute the original and the rebuilt HighC, LLVM C and LLVM IR.

```sh
python3 unittests/lift/elf-integer/ppc64_elfv1.py \
  --neverd build-release/bin/neverd --clang /path/to/clang \
  --ld /path/to/powerpc64-linux-gnu-ld
python3 unittests/lift/elf-integer/ppc64_elfv1_plt.py \
  --neverd build-release/bin/neverd --clang /path/to/clang \
  --ld /path/to/powerpc64-linux-gnu-ld
```

The PPC32 fixture executes callers and a separate provider in both byte orders.
It checks arguments, results, the caller's r30 and stack pointer after return,
and repeats each call after rebuilding HighC, LLVM C and LLVM IR at two
optimization levels. Run it with ELF LLD and with a GNU linker that supports
both byte orders. Both produce secure PLTs with eager and lazy binding. GNU ld
also produces the classic BSS-PLT, tested with loader-installed branches.
For shared libraries, module IR can retain an indirect call through a writable
import slot. The fixture verifies its native function-pointer relocation and
executes it with the separately loaded provider.

```sh
python3 unittests/lift/elf-integer/ppc32_plt.py \
  --neverd build-release/bin/neverd --clang /path/to/clang \
  --ld /path/to/powerpc-linux-gnu-ld
```

The ELFv2 fixture accepts either ELF LLD or a GNU PowerPC64 linker with
both endiannesses. It enters functions at each advertised entry, changes
the load address of the stripped shared-library caller, repeats the analysis
with its section headers removed, and executes the PLT
against an independently loaded provider. It checks the first lazy binding,
subsequent calls and eager binding. Moving a runtime relocation away from
the slot its stub loads must produce a diagnostic. GNU ld also produces
fixtures with multiple TOCs and with a TOC save in the caller’s prologue. The POWER10 PC-relative PLT case recompiles the linked stub’s caller
and executes the rebuilt calls. Unicorn executes the original POWER8 sequence.
Run the command once with each linker to cover both stub families.

```sh
python3 unittests/lift/elf-integer/ppc64_elfv2.py \
  --neverd build-release/bin/neverd --clang /path/to/clang \
  --ld /path/to/ld.lld
```
