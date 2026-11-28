# RISC-V and PowerPC ELF analysis

## Experimental ELF profiles

| ELF machine | Class and byte order | ABI used for integer arguments and returns | Encoding |
| --- | --- | --- | --- |
| RISC-V | ELF32, little-endian | ILP32, a0–a7, word result in a0, double-word result in a0/a1 | RV32I, M, A, integer C, Zba, Zbb, Zbs, Zicond, Zicntr cycle reads |
| RISC-V | ELF64, little-endian | LP64, a0–a7, result in a0 | RV64I, M, A, integer C, Zba, Zbb, Zbs, Zicond, Zicntr cycle reads |
| RISC-V | ELF32 or ELF64, little-endian | ILP32D or LP64D, eight floating argument registers and two floating result registers | Integer profile plus scalar F/D arithmetic, conversions, comparisons, sign injection, register and memory transfers, compressed floating memory forms and floating-point control/status registers |
| PowerPC | ELF32, big- or little-endian | Classic System V, r3–r10, word result in r3, double-word result in r3/r4 | Fixed-width scalar subset and VMX byte addition |
| PowerPC64 | ELF64, big- or little-endian | ELFv1 or explicit ELFv2, r3–r10, result in r3 | Classic scalar, POWER10 prefixed fixed-point forms and VMX byte addition |

For RISC-V, the ELF class selects XLEN. The float ABI flag selects the integer or double-float calling convention. A double-float image with an explicit ISA attribute must advertise D. The loader
reads normalized `Tag_RISCV_arch` attributes for M, A and their integer
subextensions. Zmmul permits multiplication, Zaamo permits atomic
read-modify-write operations, and Zalrsc permits load-reserved/store-conditional.
LLVM functions retain the evidenced features, including Zca on a Zca-only
image. An explicit stack alignment must match the 16-byte ILP32/LP64
convention, including its D variants. Either the RVC ELF
flag or the C/Zca attribute permits halfword instruction boundaries. Without
extension evidence the decoder uses the base integer profile. A 32-bit
instruction can follow a 16-bit instruction at a halfword address. The lifter
uses the physical instruction length for the link address.
[Unprivileged ISA, 20260120](https://docs.riscv.org/reference/isa/v20260120/unpriv/unpriv-index.html).
[RISC-V ELF psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/).

The double-float ABI passes named floating values in `fa0`–`fa7` while integer arguments independently consume `a0`–`a7`. A qualifying one- or two-member record can use both register files. Unnamed variadic arguments follow the integer convention. Single-precision register values are NaN-boxed in the 64-bit floating register file. Loads, stores and raw register moves retain payload bits. Sign injection checks narrow operand boxing. [RISC-V hardware floating-point calling convention](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/#_hardware_floating_point_calling_convention).

Scalar F/D arithmetic supports the five standard rounding modes. An instruction with dynamic rounding reads `frm`. Arithmetic, square root, fused multiply-add, conversions and comparisons accrue the applicable exceptions in `fflags`. CSR reads, swaps, bit sets and clears access `fflags`, `frm` and their shared representation in `fcsr`. The concrete emulator takes an explicit initial FCSR. Native RISC-V output executes these operations in the guest's floating-point environment. [RISC-V F extension](https://docs.riscv.org/reference/isa/v20260120/unpriv/f-st-ext.html). [RISC-V D extension](https://docs.riscv.org/reference/isa/v20260120/unpriv/d-st-ext.html).

Zba 1.0 enables the three XLEN-wide shift-and-add forms. On RV64,
`add.uw`, `sh1add.uw`, `sh2add.uw`, `sh3add.uw` and `slli.uw`
zero-extend the first source word before arithmetic. Zbs 1.0 enables
single-bit clear, extract, invert and set, with register and immediate forms.
A register index uses its low five bits on RV32 and its low six on RV64.
Zbb 1.0 adds logical operations with a complemented operand, signed and
unsigned min/max, rotates, zero and population counts, sign and zero
extension, `orc.b` and `rev8`. RV64 word rotates sign-extend bit 31. Word
counts examine the low 32 bits, and counting the leading or trailing zeros
of a zero word yields 32. The ELF ISA attribute selects each extension.
[RISC-V Bit Manipulation specification](https://docs.riscv.org/reference/isa/unpriv/b-st-ext.html).

Zicond 1.0 adds `czero.eqz` and `czero.nez`. They compare the whole second
source register with zero. When the named condition holds, the result is
zero. Otherwise the result is the first source register. Linked ELF inputs
need the `zicond1p0` ISA attribute, and rebuilt LLVM functions retain
`+zicond`. [RISC-V Zicond specification](https://docs.riscv.org/reference/isa/unpriv/zicond.html).

With the Zicntr 2.0 ELF attribute, `rdcycle` reads XLEN bits from the guest
cycle counter with either the integer or double-float ABI. On RV32, `rdcycleh` reads its upper 32 bits. Each instruction
samples separately, including a read whose destination is x0. Native C and
LLVM projections read the guest CSR. LowIR emulation accepts a caller-supplied
64-bit counter reader and stops if a sample is unavailable.
[RISC-V counter extensions](https://docs.riscv.org/reference/isa/unpriv/counters.html).

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

PPC32 and PPC64 lift conditional direct calls (relative and absolute), calls
through LR or CTR, and conditional jumps through CTR. Linking writes the
next instruction address to LR on both paths. A call through LR uses its
previous value. Branches through CTR clear its low two bits and require a BO
encoding that leaves CTR unchanged. A link to the next instruction preserves
the counter update and materializes the PC without introducing a callee.
For a conditional CTR jump, LowIR retains the register target and the
fallthrough. Source and LLVM projections require a proven set of local taken
targets, including cases where the fallthrough is also a taken target.
An `isel` can choose between local label addresses before `mtctr`. Both
addresses are checked after alignment, independently of the condition on
`bcctr`. An unknown selectable address leaves the target set unresolved.
The native CFG marks each taken CTR edge as true and the fallthrough as false.
When both reach one block, JSON and DOT show both edges.
An untyped register callback forwards the integer argument registers on the
taken edge. ELFv1 descriptor and ELFv2 global-entry checks apply there.
[Power ISA, Book I, Branch Processor](https://openpowerfoundation.org/specifications/isa/).

PPC64 arithmetic models the common CA, OV and SO bits. A full XER read
requires processor evidence for the ISA 3.0 CA32 and OV32 fields and is
currently rejected. [OpenPOWER ELFv2 ABI](https://openpower.foundation/specifications/64bitelfabi/).
[PowerPC ELFv1 ABI](https://refspecs.linuxfoundation.org/ELF/ppc64/PPC-elf64abi.html).
The scalar encodings and flag definitions follow Books I and II of versions
2.07b and 3.1c of the ISA. [OpenPOWER ISA](https://openpowerfoundation.org/specifications/isa/).

PowerPC `vaddubm` adds corresponding bytes in two VMX registers modulo
256. Each byte discards its carry. The sixteen lanes have the same addition
semantics in either byte order. The AltiVec ABI passes vectors in v2 through
v13 and returns a vector in v2. [AltiVec Programming Environments Manual](https://www.nxp.com/docs/en/reference-manual/ALTIVECPOWERISAPEM.pdf).

## Analysis coverage

| Stage | RISC-V | PowerPC |
| --- | --- | --- |
| Identify and load | ELF class, machine, flags and attributes | ELF class, machine, ABI flag and byte order |
| Decode and lift | Integer ALU and word operations, loads/stores, branches, jumps, multiply/divide, integer compressed forms, fences, atomics, cycle reads, scalar F/D operations and floating-point control/status registers | Integer ALU, signed and unsigned word/doubleword division, VMX modulo byte addition, per-lane population counts, CR compares and bit operations, LR/CTR transfers, carry and overflow forms, rotates/masks, ordinary, update-form and byte-reversed loads/stores, reservations and barriers |
| CFG and calling convention | Direct and indirect jumps/calls, return through ra, integer and double-float ABI argument registers | Direct and conditional branches, CTR loops, conditional direct and indirect calls, CTR jumps and guarded LR returns, integer register arguments |
| Source and LLVM | HighC, LLVM IR and LLVM-derived C for the qualified integer subset and scalar F/D operations, including RISC-V rounding and FCSR | Same stages in target byte order for the qualified PowerPC subset |
| Native rewrite | Unsupported | Unsupported |

When selected on its own, a straight-line RV32 or PPC32 leaf can establish a
64-bit sum from the first two argument pairs. Both result registers must hold
the corresponding sums and the low addition's carry must reach the high
addition. Word spills and reloads can carry the inputs through the leaf's
own frame. PPC32 places the high word in r3 on big endian and r4 on little
endian. The source projections can retain four word-sized parameters.

On RV32 and PPC32, the compiler runtime helpers `__ashldi3`, `__ashrdi3`
and `__lshrdi3` take a double word followed by a shift count. A zero-count
branch can return both incoming words untouched. Recovery checks the actual
count register, the direction of the branch and the values reaching that
return. At a shared exit, both words must come from the same predecessor.
For an unnamed leaf that first tests the word boundary and splits on the
zero count, recovery follows each return and checks both ABI words. For each
count from 0 through 63, every output bit must have the input or fill bit
of the same left, logical-right or arithmetic-right shift. The input words
and scratch registers are symbolic. The proof establishes the result width.
The emitted function keeps its original body, including the RV32I return
alignment check. A mismatched word, store or unproved path leaves the
standalone result at its ordinary word width.

PowerPC `slw` and `srw` use six bits of the count. On PPC32, LowIR zero-extends
the source word to 64 bits, shifts, and takes the low 32 bits. Counts from
32 through 63 therefore produce zero through the same expression in LLVM
and C. The record forms compare the result at the target register width.

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

On PPC64, `mullw` writes the full signed product of the low 32 bits of both
operands. `mullwo` sets OV when that product does not fit a signed word.
`mulldo` tests whether the doubleword product fits a signed doubleword. Both
update sticky SO. Their record forms compare the returned GPR with zero.
`mulhw` and `mulhwu` put the upper word of the signed or unsigned product in
the low word of the GPR. In 64-bit mode the upper GPR word and the recorded
LT, GT and EQ bits are unspecified. LowIR keeps those bits unknown. Rebuilt
C and LLVM execute the native instruction for each occurrence and capture
the CR0 comparison from the same multiply.
[Power ISA, Book I, Fixed-Point Arithmetic Instructions](https://openpowerfoundation.org/specifications/isa/).

RISC-V atomics retain width and acquire/release bits. HighC and LLVM-derived C
use target GNU inline assembly for ordering and reservation instructions. The
LowIR emulator evaluates aligned ordinary memory under a single-hart model.
A conditional store needs a known reservation state. Multi-hart interference,
device memory and guest exception delivery are outside that model.

On PPC32 and PPC64, `lhbrx`, `lwbrx`, `sthbrx` and `stwbrx` transfer a
halfword or word in the opposite byte order to ordinary memory accesses.
PPC64 also supports `ldbrx` and `stdbrx` for a doubleword. Loads zero-extend
to the register width. Stores take the low bits of the source register.
The effective address adds RB to RA, using zero for a zero RA field.
Power ISA 3.0 `cnttzw` counts trailing zeros in the low word, and `cnttzd`
counts across the full PPC64 register. A zero input returns 32 or 64,
respectively. Their record forms update CR0 and copy XER.SO to CR0.SO.
[Power ISA, Book I, Fixed-Point Byte-Reverse Load and Store Instructions](https://openpowerfoundation.org/specifications/isa/).

In ordinary PPC64 code, `paddi`, `plbz`, `plhz`, `plha`, `plwz`, `plwa`,
`pld`, `pstb`, `psth`, `pstw` and `pstd` use a signed 34-bit displacement.
Each of the two instruction words follows the ELF byte order. A PC-relative
form uses the address of its prefix and requires a zero RA field. Other
forms use zero or the selected GPR as their base. The eight-byte instruction
must fit within one 64-byte fetch block. The ELFv2 loader uses the same
displacement and boundary rules for a PC-relative linkage stub.
[Power ISA, Book I](https://openpowerfoundation.org/specifications/isa/).

The strict lifters report unsupported instructions at their original VA.
Other RISC-V system and CSR instructions, V and vendor extensions still
require lifting support. Additional PowerPC VMX operations, VLE, SPE, VSX,
other prefixed forms and system instructions also remain outside this profile.
Relative relocations write the link-time pointer in target byte order.
Full-width absolute relocations retain the symbol and addend. RISC-V and
ELFv2 jump slots retain their import binding. PPC32 PIC stubs require a
proven r30-relative jump slot. PowerPC floating arguments, variadic and aggregate calls require separate ABI coverage. Link objects with relocations
against allocated sections before analysis. Raw firmware mapping and native
guest execution have separate entry points and coverage.

A linked RV32 or RV64 function can construct an integer constant and a table
address with the same `lui` and `addi` sequence. At an indexed load, MedIR can
recover the table when the entire range of possible reads fits file-backed,
read-only storage and stays within known data object boundaries. Other integer
uses of the same sum retain its original bits. An unbounded index, a read
across adjacent objects, or a writable mapping still needs other address
evidence. Without section headers, the load needs a readable, non-executable
segment and must stay clear of the ELF and program headers.

For example, compiler-rt's `__ffssi2` compiled by Clang 23.1.6 at `-O2`
uses an unsigned five-bit index into a 32-byte table on RV32IMAC and RV64IMAC.
HighC and the full LLVM module carry the table into the rebuilt image.
Single-function LLVM-derived C declares the original image storage as an
external array. Its caller can link the table bytes at the corresponding
offset in that array at a new address.

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
A RISC-V assembly fixture uses the same sum as a load address and an integer
operand. It checks every table index after moving the image with and without
section headers, at both rebuild optimization levels.
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

The PowerPC product fixture executes a separately compiled caller against the
original and all three projections in both byte orders. It checks the full
return register and the stored CR fields, including an independent comparison
after the multiply. A PPC64 function with repeated operands must still execute
both recorded high-word multiplies.

```sh
python3 unittests/lift/elf-integer/ppc_products.py \
  --neverd build-release/bin/neverd --clang /path/to/clang --ld /path/to/ld.lld
```

The shift fixture builds the three compiler-rt helpers with Clang at `-O2`.
A separately compiled caller uses the original C prototypes and checks
independent high and low input words for every shift count from 0 to 63.
All six target profiles rebuild HighC, LLVM-derived C and module IR at
`-O0` and `-O2`, from both a named module and individual fully stripped
ELFs selected by entry address. RV32 also runs the stripped fixtures with
the base ISA's four-byte instruction alignment. By default the
fixture reads the builtins from the LLVM submodule. Use `--builtins` to
select another compiler-rt source directory.

```sh
python3 unittests/lift/elf-integer/wide_shifts.py \
  --neverd build-release/bin/neverd --clang /path/to/clang --ld /path/to/ld.lld
```

The bit-operation fixture executes all Zbb instructions and the indexed
byte-reversed transfers and trailing-zero counts. A separate caller compares each register result and
each affected or surrounding byte with a Python oracle. It checks the
original and all three rebuilt projections at two optimization levels on
RV32, RV64, PPC32 and PPC64, including both PowerPC byte orders. PPC32 runs in Unicorn with MSR.SF clear.
RV32, RV64 and the two PPC64 profiles use Linux user-mode QEMU.

```sh
python3 unittests/lift/elf-integer/bit_operations.py \
  --neverd build-release/bin/neverd --clang /path/to/clang \
  --ld /path/to/ld.lld --qemu-dir /usr/bin
```

For PPC64 ELFv1, pass `--profiles powerpc64 powerpc64le --ppc64-abi elfv1`
and select a GNU PowerPC64 linker with `--ld`. The entry descriptor and the
code it names must both lie outside every rebuilt mapping. Plain-pointer
C projections use the `-fno-strict-aliasing` flag specified in their prelude.

The conditional-transfer fixture checks original machine execution and
relocated HighC, LLVM C and LLVM IR rebuilds at both optimization levels.
It varies the `isel` choice independently of the branch condition and checks
the diagnostic when one of the selectable addresses comes from a parameter.
Use GNU PowerPC64 ld with `--ppc64-abi elfv1 --profiles powerpc64 powerpc64le`
to exercise descriptors in both byte orders.

```bash
python3 unittests/lift/elf-integer/conditional_branches.py \
  --neverd /path/to/neverd --clang /path/to/clang --ld /path/to/ld.lld
```

The counter and VMX fixture links each rebuilt callee to a separately compiled
native caller. ILP32, ILP32D, LP64 and LP64D runs read the guest counter under
QEMU. RV32 also repeats its high/low/high loop across a forced rollover. PowerPC callers exercise two-argument addition and a three-argument function
that leaves its first vector unused. Both check all 65,536 unsigned byte pairs
on PPC32 and PPC64 in both byte orders. Pass `--elfv1-ld /path/to/powerpc64-linux-gnu-ld`
to exercise ELFv1 function descriptors as well.

```sh
python3 unittests/lift/elf-integer/counter_vmx.py \
  --neverd build-release/bin/neverd --clang /path/to/clang --ld /path/to/ld.lld \
  --qemu-dir /usr/bin
```

The extended scalar fixture runs linked RV32, RV64 and both byte orders of
POWER10 PPC64 under Linux QEMU user mode. For Zicond, the RV32 and RV64
fixtures combine both instructions in a branchless select. A caller compiled
for the base ISA checks zero, nonzero and high-bit-only conditions. The PPC64
oracle checks return values and each byte of the stores. It runs again
after recompiling HighC, LLVM-derived C and LLVM IR at `-O0` and `-O2`, with
the original code and data addresses outside the rebuilt mappings.

```sh
python3 unittests/lift/elf-integer/extended_scalar.py \
  --neverd build-release/bin/neverd --clang /path/to/clang \
  --ld /path/to/ld.lld --qemu-dir /usr/bin
```

With two ELFv2 musl dynamic loaders, the same fixture also executes the
original POWER10 PLT call in both byte orders. It checks that the linked
branch enters a prefixed load of the provider’s jump slot, then calls the
provider repeatedly through the system loader in QEMU. Each rebuilt caller
links to the same independent provider. Run with LLD or GNU PowerPC64 ld.

```sh
python3 unittests/lift/elf-integer/extended_scalar.py \
  --neverd build-release/bin/neverd --clang /path/to/clang --ld /path/to/ld.lld \
  --profiles powerpc64 powerpc64le --qemu-dir /usr/bin \
  --ppc64-loaders /path/to/powerpc64/libc.so /path/to/powerpc64le/libc.so
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
