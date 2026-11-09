# MIPS analysis

The experimental MIPS analysis path targets linked ELF programs with classic
MIPS32 Release 2/o32 or MIPS64 Release 2/n64 instructions, in either byte order,
the bounded microMIPS32 and MIPS16e32 Release 2/o32 subsets, and the MIPS32
Release 5/o32 integer MSA slice described below.
The selected ELF profile determines register width, byte order, stack layout
and the LLVM target triple. MIPS64 and AArch64 are different instruction sets.

## Implementation boundaries

- ELF identification and loading share class, ISA, ABI and encoding checks.
  Sectionless dynamic tables, MIPS global GOT suffixes and standard PLT veneers
  retain symbol identities. Data pointers and DWARF frame records use target
  byte order. A packed MIPS64 relocation is refused until all of its component
  operations are implemented. Runtime relocation tables are read from
  `PT_DYNAMIC`, including when a section table is present. Unimplemented
  relocations (including REL32/32/64 runtime fixups) and imported data/TLS GOT
  bindings fail explicitly. Admitted scalar COPY objects remain external
  dependencies in generated C; their file bytes are not runtime initializers.
  Known libc objects retain their external header declarations. HighC rejects
  partial/overlapping access or indexed byte backings for other COPY objects
  until those backings have an external-object contract.
- Classic disassembly consumes four bytes. A delayed transfer and its slot
  form an eight-byte semantic execution unit. The branch reads its condition
  and indirect target before the slot, writes its link before the slot, and
  transfers after it. Branch-likely instructions annul the slot on the false
  path. The IR retains separate head/slot addresses and byte ownership; entry
  directly into a slot decodes that instruction independently.
- Integer arithmetic, shifts/rotates, comparisons, conditional moves, HI/LO
  multiplication and aligned integer loads/stores have explicit lifter rules.
  MIPS64 word results are sign extended where the ISA requires it. Writes to
  `$zero` discard the result without discarding required memory effects.
- SPECIAL3 `EXT`/`INS`, `DEXT`/`DEXTM`/`DEXTU`, `DINS`/`DINSM`/`DINSU`, and
  `SEB`/`SEH`/`WSBH`/`DSBH`/`DSHD` use the encoding descriptors in
  `MIPSBitfield.def`. Reserved fields and invalid bounds are refused before
  emitting effects. Word forms sign extend their result, including a full-width
  `EXT`; a 32-bit field extracted by `DEXT` remains zero extended.
- MIPS64 Release 2 word-input restrictions are explicit checks for word
  addition/subtraction, multiplication, count-leading operations, right
  shifts/rotates and the word SPECIAL3 forms. Variable shift counts and
  `SLL`/`SLLV` have no such restriction. A failed check refuses an architecturally
  UNPREDICTABLE input before observable writes, including a `$zero` destination;
  this strict analysis policy does not claim a guest architectural exception.
- `LWL`/`LWR` and MIPS64 `LDL`/`LDR` retain the exact byte span of each
  instruction, including a discarded destination. They do not combine a pair
  into a wider speculative read. Source lowering retains guest alignment bits
  when direct image data is rebound. Indexed merge reads into rebuilt image
  storage still require an explicit address contract.
- SPECIAL2 `MUL` computes the low word and explicitly invalidates both HI and
  LO. Dead unpredictable results can be removed; an observed result cannot
  become an old value or zero. MIPS64 word-input restrictions are checked.
  A partial MIPS64 `LWR` separates its exact low word from its implementation
  dependent upper word. A subsequent `LWL` can discard those upper bits;
  source emission refuses an observed upper word unless it proves a result
  shared by the ISA's permitted implementations.

- o32 uses `$a0`–`$a3`, a 16-byte caller argument area and aligned register pairs
  for 64-bit integers. n64 uses registers 4–11 and eight-byte argument slots.
  Big-endian integer pairs put their high word first, and narrow stack scalars
  are right justified. Single-carrier results use `$v0`; o32 64-bit results
  use `$v0/$v1` in target word order. These placement rules are tested
  separately from source-signature reconstruction. The fixed integer-call
  adapter still declines split o32 values, register-padding holes and narrow
  big-endian overflow slots; complete typed ABI reconstruction is not claimed.

LowIR execution retains the common known bits of the two permitted partial
MIPS64 `LWR` upper words. Scalar COPY, SUBBYTES, CONCAT and integer extensions
transport that uncertainty, so a later complementary load can produce a fully
known value. Public register reads refuse a view containing unknown bits;
stores, arithmetic and control also refuse any unknown bits they observe.
Narrow reads can still consume a defined low word. This does not select a CPU's
implementation-dependent behavior or give uninitialized registers a zero value.

Source recovery proves complete adjacent merge pairs in a single ordinary leaf
block before inferring parameters. It checks the original instruction
occurrences, the directed address relation for the target byte order, and every
use of the first result. Only the first core's unobserved incoming destination
may become zero; the second still consumes the first result. Both reads and
their fault order remain. Calls, PHIs, exceptional edges, other control flow,
observed partial results and unproved address relationships retain the original
dependency. Multi-block and nonadjacent pairs remain outside this proof.
Concrete LowIR transport and source parameter inference have separate validation
boundaries; this source proof does not initialize absent machine registers.

This is a strict subset. n32, historical MIPS I–V profiles, Release 6,
microMIPS64, floating point, MSA/DSP forms beyond the slices below, privileged
instructions, LL/SC reservations,
unaligned merge stores and trapping arithmetic/division are not admitted by
the current lifter. Unsupported instructions, missing slots and transfers in a
delay slot produce errors, not replacement NOPs. Floating/aggregate source ABI
classification, arbitrary PIC/gp recovery, raw-input target selection and
native code rewriting are not claimed. Unicorn is a test reference, not a
NeverD MIPS guest execution backend.

The SPECIAL3 and word-input rules follow the operation/restriction sections of
[MIPS64 Volume II, revision 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf).
The [Ghidra MIPS definitions at e45e5a62](https://github.com/NationalSecurityAgency/ghidra/tree/e45e5a62fd66c2e48514931f6e356aff33a79fcb/Ghidra/Processors/MIPS/data/languages)
were a cross-check; the LowIR implementation and independent expectations are
owned here. Decoder recognition alone does not qualify instruction semantics.

## Classic MIPS32 DSP state and packed arithmetic

The initial DSP slice admits classic MIPS32r2/o32 ELF in either byte order when
`.MIPS.abiflags` declares the DSP ASE. It supports `RDDSP`, `WRDSP`, `ADDU.QB`,
`ADDU_S.QB`, `SUBU.QB`, `SUBU_S.QB`, `ADDQ.PH`, `ADDQ_S.PH`,
`SUBQ.PH` and `SUBQ_S.PH`. Pure integer forms include `SHRL.QB`/`SHRLV.QB`,
`SHRA.PH`/`SHRAV.PH`, `SHRA_R.PH`/`SHRAV_R.PH`,
`SHRA_R.W`/`SHRAV_R.W`, `PRECRQ.QB.PH` and `PRECRQ.PH.W`.
Variable counts are reduced modulo the lane width. Rounded arithmetic shifts
round in widened signed precision, including the identity at count zero.
Pack operations select the upper half of each source lane. These forms leave
DSPControl untouched and need no incoming DSPControl value, including when
their destination is `$zero`. Decoder reuse consumes the current
image's feature evidence; Capstone recognizing an encoding without that
evidence does not authorize its lift. DSPR2, DSP64, microMIPS DSP, accumulators and other DSP instructions remain
unsupported.

The ten-bit control selector selects six field groups; its upper four bits are
ignored. Partial writes preserve unselected fields, and packed byte additions
and subtractions set the sticky overflow flag even when the destination is
`$zero`. Saturated addition clamps each overflowing lane to 255; saturated
subtraction clamps each underflowing lane to zero. Signed halfword operations
use two independent Q15 lanes: ordinary results wrap to 16 bits, and saturated
results clamp to [-32768, 32767]. Any lane overflow sets DSPControl bit 20
without clearing an existing flag, including when the destination is `$zero`.
Full writes
can initialize an absent concrete DSPControl value; reads and partial updates
never invent its initial state. Selector and encoding tables live in `.def`
files and are shared by the owning semantics.

DSPControl reads and writes remain effectful machine-state intrinsics through
SSA and dead-code elimination. HighC, LLVM and LLVMC project them to explicit
target state transfers. Their results remain visible to callers after return;
no extra source parameter or zero-initialized C local substitutes for machine
state. This source contract requires an enabled DSP execution environment and
an original-target compiler. Disabled-DSP exception delivery is not modeled.
Every generated function in a DSP image, including ordinary scalar functions
and generated helpers, disables automatic DSP instruction selection with
negative target features. Without an image, all MIPS32 source has the same
contract because no image profile rules out live DSPControl. This also covers
pure arithmetic with no state intrinsics, siblings, helpers and source shards.
Generated C requires Clang's scoped target attributes; both `-O0` and `-O2`
are exercised with external `-mdsp` enabled.
Explicit state transfers use local assembler directives. This prevents a
compiler from replacing ordinary wide arithmetic with `ADDSC`/`ADDWC` and
silently changing DSPControl. Source returns also require proof of the captured
incoming link; a DSP result written to `$ra` cannot become an ordinary C return.
Standalone MIPS32 LowIR with an explicit link write also requires this proof,
including a zero-selector `RDDSP ra,0` reduced to a constant copy. Cross-call
link spills remain outside the current source proof.

The operation rules follow the
[MIPS32 DSP ASE manual, revision 1.00](https://0x04.net/~mwk/doc/mips/MD00374-2B-MIPS32DSP-AFP-01.00.pdf).
The pinned Unicorn model is checked with Status.MX enabled. Its zero-destination
packed arithmetic incorrectly omits the overflow update; independent manual
expectations cover that case, and source roundtrip receipts record the oracle
disagreement explicitly.

## microMIPS32 instruction and source boundaries

The loader accepts linked r2/o32 ELF with consistent microMIPS header and
`.MIPS.abiflags` evidence. `st_other` supplies each defined function's encoding;
an even `st_value` can name a microMIPS function. Entry tags, symbol ranges and
incoming control-flow modes must agree. Mixed images retain local mode evidence
through discovery, CFG construction and SDK instruction-flow requests; an
unknown destination is not silently treated as classic code.

The scalar decoder maps explicit microMIPS encodings into the existing integer
semantic forms. The current set includes MOVE16, ADDU16/SUBU16, SLL16/SRL16,
LI16, compact byte/halfword/word memory forms, SP/GP-relative word accesses,
the 32-bit immediate/register forms listed in `MicroMIPSInteger.def`,
BEQZ16/BNEZ16/B16, BEQ/BNE, J/JAL/JALS/JALX and the supported JR/JALR forms.
Unlisted encodings are errors. This is not complete compressed-instruction
coverage and does not admit other ISA revisions or compressed DSP/MSA encodings.

Physical instructions occupy two or four bytes, including four-byte heads at
addresses congruent to two modulo four. The execution-unit record keeps the
actual head and slot widths. JALR16/JALRS16 require four/two-byte slots, JRC has
no slot, and a slot cannot itself transfer control. Link values retain the
caller's mode tag, while direct JALX destinations and serialized indirect
targets have distinct validation rules. Defined function GOT cells use callable
tags; data symbol addresses keep their original bits.

Both source routes retain explicit microMIPS/nomicromips and nomips16 function
attributes. A source return must prove the captured incoming RA, including its
mode bit, from the original instruction boundaries and SSA. Tail calls and unproved stack restores
across calls remain outside this source proof. Source tests currently target
bounded integer leaves, memory effects, o32 stack arguments and branch/slot
capture.

Numeric microMIPS code-pointer reconstruction is explicitly refused. The tested
Clang 22 compiler folds a microMIPS function pointer's low bit to zero in some
integer expressions and static initializers; a function mode attribute does not
establish that contract. HighC and LLVM share this qualification boundary when
materializing a code identity. Unknown-mode import values, including whole or
partial GOT reads, are also unqualified. Direct control identities, serialized
callables and code-byte/data addresses are separate roles. These limits require
a subsequent toolchain/source-contract increment, not merely another opcode.

The microMIPS operation and slot rules were checked against
[microMIPS32 Volume II, revision 5.04](https://s3-eu-west-1.amazonaws.com/downloads-mips/documents/MD00582-2B-microMIPS32-AFP-05.04.pdf)
and the pinned [GNU simulator definitions](https://gnu.googlesource.com/binutils-gdb/+/d01e823438c7dc264d6885fbbfeace4d8955dcb7/sim/mips/micromips.igen).
The lifter's admitted ISA remains r2; this reference does not admit later forms.

## MIPS16e32 integer and control subset

Linked MIPS32r2/o32 ELF can declare MIPS16 in the header and, when present,
`.MIPS.abiflags`. Function symbols use `STO_MIPS_MIPS16`; its overlapping
microMIPS bit does not change the ISA family. Conflicting flags, symbols,
entry tags or mode ranges are errors. MIPS16 and microMIPS cannot coexist in
one admitted image. Classic-to-MIPS16 JALX has an explicit destination mode;
register transfers carry a separate MIPS16 target-bit contract even when the
source instruction is classic MIPS. Serialized callables retain bit zero;
ordinary data pointers do not acquire a code tag.

The scalar set is `LI`, `ADDIU`, `ADJSP`, `CMPI`, `SLTI`/`SLTIU`,
`ADDU`/`SUBU`, `AND`/`OR`/`XOR`/`NOT`, `CMP`, `SLT`/`SLTU`, `NEG`,
`SLL`/`SRL`/`SRA`, `SLLV`/`SRLV`/`SRAV`, `MULT`/`MULTU`,
`MFHI`/`MFLO`, and both compact/full-GPR `MOVE` directions.
`CMP` writes the full XOR result to T, while the comparisons write a boolean.
Short shifts encode counts 1..8 (zero means eight); extended shifts encode
0..31 in prefix bits 10:6 and reject nonzero reserved prefix/suffix fields.
Variable shifts mask the count to five bits. Multiply produces both HI/LO
halves through the classic semantics; moves from HI/LO preserve their known-bit
state and never initialize an absent value.
Before source optimization, the shared MIPS qualification requires HI/LO reads
to come from definitions on every incoming path. Entry values and call-clobbered
values have no ordinary C ABI carrier and are refused, including without an
image. This check also applies to classic MIPS32/64; an unknown accumulator
return must not silently turn into a void function.
Ordinary `LB`/`LBU`/`LH`/`LHU`/`LW` and `SB`/`SH`/`SW`,
SP-relative word loads/stores, and the RA stack store reuse the classic memory
semantics. Short memory offsets are unsigned and scaled by access width;
extended offsets are signed, unscaled 16-bit values. `EXTEND` and its suffix
form one four-byte instruction, including at addresses two modulo four.
`MIPS16Integer.def` owns the scalar and memory encoding descriptions.

PC/SP-relative `ADDIU` and PC-relative `LW` are supported. PC-relative operations
clear the base PC's low two bits before adding the offset; they never align the
sum. In a delay slot the base is the transfer head's first halfword, while the
LowIR operation keeps the slot's own address. Memory checks, sign extension and
base/destination overlap remain owned by the shared MIPS memory lifter.

`B`, `BEQZ`/`BNEZ`, `BTEQZ`/`BTNEZ`, `JRC` and `JALRC` have no delay slot.
An unconditional B has one successor. `JR` and `JALR` require one two-byte,
non-extended, non-control slot. `JAL`/`JALX` have a four-byte head and the same
two-byte slot; their link is the tagged head address plus six. JALR/JALRC links
advance by four/two, respectively. Indirect targets are captured before link
and slot writes. JALX selects classic MIPS at its destination. Local mode
evidence must agree across the whole head/slot unit, including during function
discovery. SDK listings continue to show physical instructions separately.

HighC and LLVMC retain `mips16,nomicromips` attributes. Source returns require
the captured-entry-RA proof shared with microMIPS. A call may preserve the entry
link in a callee-saved GPR and restore it before return; a caller-saved register,
an overwritten save or an unproved stack restore across a call is refused.
Direct calls also require image evidence for the destination mode. Numeric
compressed-code pointers remain unqualified. Source tests use Clang assembly
output plus GNU MIPS as/ld, with adverse `-mmicromips` compiler defaults, and
verify ELF flags, symbol tags, memory, saved registers and actual callee entry.
The GNU assembler's [`.insn` directive](https://sourceware.org/binutils/docs-2.29/as/MIPS-insn.html)
marks literal instruction fixtures so their symbols retain the code mode.

SAVE/RESTORE, division, MIPS16e2 and MIPS16 on MIPS64 remain
unsupported. No path treats a misdecoded operand or unknown instruction as
valid semantics. The immediate, memory, call and slot rules follow
[MIPS16e Volume IV-a, revision 2.63](https://jnastarot.io/arch_index/pdf/MD00076-2B-MIPS1632-AFP-02.63.pdf)
and were cross-checked against the pinned Ghidra definitions linked above.

## Classic MIPS32 MSA integer vectors

The initial MSA slice requires classic MIPS32 Release 5/o32 ELF in either byte
order, with `.MIPS.abiflags` explicitly declaring MSA, 128-bit CPR1 registers
and FP64 ABI tag 6. A reused decoder consumes the current revision as well as
feature evidence; architecture-only initialization remains Release 2 without
MSA. Other revisions, MSA64, microMIPS/MSA and DSP/MSA combinations are refused.

`FILL.W`, `INSERT.W`, `COPY_S.W`, all four B/H/W/D formats of `ADDV` and
`SUBV`, `CEQ`, `CLT_S`/`CLT_U`, `CLE_S`/`CLE_U`, `MIN_S`/`MIN_U`,
`MAX_S`/`MAX_U`, `SLL`/`SRL`/`SRA`, `BCLR`/`BSET`/`BNEG`, `MULV`, and
`AND.V`/`OR.V`/`NOR.V`/`XOR.V` use exact encoding descriptors in
`MIPSMSA.def`. The 32 W registers are 128-bit containers. Lane zero occupies
the low bits in both byte orders; addition and subtraction wrap independently
within each 8/16/32/64-bit lane. Integer comparisons set every bit of a true
lane to one and every bit of a false lane to zero; signed and unsigned ordering
have separate rules. Min/max selects the complete winning lane using the same
signed/unsigned ordering, including 64-bit extrema. Shifts and bit edits use
the low log2(lane width) bits of each corresponding count/index lane. Multiply
retains the low lane-width product independently for every lane. Logical
operations consume all 128 bits.
Inputs are captured before an overlapping destination is written. F registers
share the low 64 bits of their corresponding W register.
Scalar floating instructions remain unsupported: their future implementation
must model the unpredictable remaining W bits, not preserve or zero them by
assuming a generic subregister write.

LowIR accepts all 32 W registers. Source qualification is narrower and runs
before SSA/dead-code elimination: it accepts one ordinary leaf block with fully
initialized vector dependencies, no calls, and no writes to the saved
F20/F22/.../F30 low views. Without an Image, all
W20..W31 writes are refused because other o32 FPABIs preserve additional views.
MSA64 source is refused at this boundary too; no extra vector parameter is
invented. Returns must retain the
captured entry RA, including when `COPY_S.W` writes RA in a delay slot. General
vector ABI, saved-FPR restoration and multi-block source proofs remain future
work. Both C paths use Clang's bit-precise integer support for any surviving
128-bit scalar temporaries on MIPS32.

The execution contract requires MSA enabled with CU1/FR configured. It does not
model disabled-MSA exceptions, MSA floating point/control state or arbitrary
MSA guest execution. The operation and alias rules follow the MIPS32 MSA manual
(MD00866, revision 1.12), sections ADDV, SUBV, CEQ, CLT/CLE, MIN/MAX,
SLL/SRL/SRA, BCLR/BSET/BNEG, MULV, AND/OR/NOR/XOR,
COPY_S, FILL and INSERT. Owned execution
fixtures validate scalar results, output bytes and the ABI-preserved low FPR
views against an enabled P5600 reference; no unimplemented W-register API is
used as an oracle.

## Ordinary memory source qualification

LowIR preserves ordinary load/store checks and fault order, including dead load
results and loads in delay slots. HighC/LLVM/LLVMC equivalence for ordinary RAM
accesses is qualified only when every original access is valid, within accessible
storage, and aligned as required by the target. This does not cover MMIO or
architectural memory-fault delivery. Ordinary C dereferences and existing
MedIR/compiler dead-code elimination can remove an unused load: an unmapped
`LBU v0,0(a0); LI v0,1; JRC ra` faults in the original and LowIR, while generated
C may return 1. The same limitation applies to classic MIPS. This is an open
source-effect contract limitation, not a passing fault-equivalence case; the
explicit merge-load intrinsics have their separately tested effect contract.

## Tests

The owned fixtures in `unittests/lift/mips` do not contain vendor firmware.
The ELF tests construct bytes independently of the loader. Integer semantics
have mathematical expectations and a pinned Unicorn differential reference.
Control tests cover pre-slot operand capture, visible links, annulment and
independently entered slots. Pipeline tests check C/LLVM generation and target
data layouts for all four width/byte-order combinations. Merge-load tests cover
every offset, bounded memory, discarded results, base/destination aliasing and
faults. MUL tests distinguish unread HI/LO clobbers, separately restored
accumulators and invalid MIPS64 word inputs.
Partial-value tests also cover both merge orders with arbitrary old upper
words, common known-bit slices, sign extension of unknown sign bits, narrow
versus wide stores, malformed transports, partial register writes, temporary
reuse, explicit seeding, reset and preserved/volatile call state. MFHI/MFLO can
transport an unpredictable result; querying or computing with that result
still refuses it until the observed bits are overwritten.
Complete-pair source fixtures also leave the destination uninitialized, exercise
both orders at every offset with fixed and seeded random old machine values,
and compare independently assembled expected bytes. Page-edge faults check
whether the preceding merge read occurred. Branch-likely load fixtures check
that an annulled invalid-memory slot returns normally and an executed slot faults.
Bitfield tests enumerate every legal position/size and every raw five-bit field
combination using independent expectations. Shuffle tests check byte/halfword
order, sign extension, aliases and `$zero`; pinned Unicorn checks defined
boundary inputs. Source fixtures cover all 13 forms, full-width fields,
annulled invalid-word slots and retained checks for discarded results.

```sh
python3 scripts/build.py build-mips --target NeverDMIPSTests NeverDMIPSIntegerTests NeverDMIPSFlowTests NeverDMIPSSDKTests NeverDMIPSPipelineTests neverd --parallel 4
ctest --test-dir build-mips --build-config Release -L '^NeverDMIPS.*Tests$' --output-on-failure --parallel 4
```

The additional `unittests/lift/mips/roundtrip.py` runner compiles generated
HighC and LLVMC for their original targets and compares return values and
memory effects against the assembly originals in pinned Unicorn. LLVMC binds
original-image storage; either source route explicitly binds omitted original
callees. See the
[testing guide](testing.md#mips-elf-analysis-and-source-roundtrips) for setup.
A registered test is not a claim that it has passed on every host.

Use `roundtrip.py --encoding micro` for the five microMIPS assembly leaves in
both byte orders and both C backends. The runner selects M14Kc, follows each ELF
symbol's mode bit and recompiles each result for its original target. The
BEQZ16 fixture specifies literal displacement bytes: the local-label spelling
in the tested Clang 22 assembler emitted a zero displacement. Both branch
outcomes are checked by original-target execution. Memory tests also compare
literal 16/32-bit encodings with independent byte expectations and Unicorn,
including sign extension, aliases, exact stores and failed/discarded loads.

`neverd-mips-bench micromips|micromipsel capstone|decode|lift CODE REPEATS`
uses matching microMIPS options for physical Capstone/Decoder comparisons.
Decode plus lift is a separate workload. The paired regression runner continues
to compare identical classic inputs and TP-Link requests against the retained
baseline; support for new encodings is not a same-output speedup claim.

The merge-load/MUL qualification used macOS arm64 Release, integrated with
dev `e320fec5b`: 1,294 focused/shared CTest cases passed, including 169 MIPS
cases. All 200 source roundtrip groups (25 kernels, four profiles, two backends)
passed 992 normal inputs and 24 expected read-fault cases. The latter check
discarded destinations and a JALR slot fault before callee entry. HighC image
merge fixtures execute with the original image unmapped. Disposable roundtrip
objects and executables are deleted after success; sources and hashes remain.

The shared checks cover byte projection, liveness, return classification and
call contracts on x86-64, i386, ARM and AArch64. CTest used four concurrent
tests with `NEVERD_THREADS=1`; an earlier AAPCS64 source-compilation timeout
passed both in isolation and in the final run. Linux/Windows host builds and
GUI operation were not exercised. Local receipts are
`build-mips/ctest-mips-merge-final.log`,
`build-mips/roundtrip-merge-final/results.json` and
`build-mips/qualification-merge.json`.

After integrating dev `1d27c2d2b`, the Release CLI and MIPS targets rebuilt
successfully, all 169 MIPS tests passed again, and the selected firmware views
were regenerated. This follow-up covers the intervening SDK changes; the
broader and source-roundtrip results above remain tied to their recorded build.

The partial-value follow-up on dev `a72fcd510` passed 330 MIPS/shared cases.
After adding the fully unknown-destination regression, all 64 MIPS integer
cases and 691 jump-table cases passed. Six jump-table cases were skipped:
two require a compiler-generated table shape this compiler did not produce,
one requires an x86-64 Linux host, and three require unavailable QEMU user-mode
executors. The selected firmware views still emit successfully. Receipts are
`build-mips/ctest-partial-values-shared.log`,
`build-mips/ctest-partial-values-jumptable.log` and
`build-mips/partial-values/qualification.json`. The 200-group source result
above belongs to the preceding merge-load increment, not a rerun of this one.
The final integration with dev `9e7932d73` and Capstone `20a8dd5f` rebuilt the
CLI/MIPS targets, passed all 189 MIPS cases and regenerated the selected
firmware views. The broader jump-table qualification remains tied to its
recorded earlier dependency snapshot.

The complete-pair source follow-up on dev `537264cf` passed 240 roundtrip
groups (30 kernels, four profiles, both C routes): 3,832 normal inputs and 96
expected read faults. These include uninitialized destinations, both page-edge
fault orders and annulled/executed invalid-memory likely slots. The selected
Archer C7 views were regenerated successfully. The source receipt is
`build-mips/roundtrip-source-pairs-final/results.json`; the wider qualification
is recorded in `build-mips/source-merge-pairs/qualification.json`.
All 221 MIPS cases passed across the broad run and the final 69-case
pipeline/proof suite, including actual base/destination aliasing and reversed
address relations. The broader 818-case selection initially passed 808 cases;
nine timed-out generated-source matrices completed on isolated reruns (eight
through CTest, one direct run in 33 seconds). One x64/AArch64 native-hint
assertion also fails with unchanged dev production code and the MIPS proof
absent from the binary. It is recorded as an existing failure, not a passing
check. These shared results retain their original dependency snapshot.
Integrations with dev `aac568578` and then `4c57243f`, using Capstone
`8b81f225`, each passed all 221 MIPS cases and repeated all 240 source groups
with identical generated source hashes. The final build also regenerated all
selected firmware views with unchanged source hashes and verified call LLVM.
Its original source receipt was
`build-mips/roundtrip-source-pairs-shared-dev/results.json`; the integration
commands and binary hashes are in
`build-mips/source-merge-pairs/shared-dev-integration.json`. Earlier detailed
reports, including that source receipt, are retained in verified local archives.

The SPECIAL3/word-contract follow-up, integrated with dev `97f5f3797` and
Capstone `6787c2f3`, passed all 248 MIPS cases, including exhaustive raw field
admission and both endian forms of the MIPS64 word checks. All 344 source
groups passed 4,600 normal inputs, 96 expected read faults and 24 explicit
strict-word refusals. Source hashes and selected firmware C/LLVM hashes were
unchanged by the dev integration; the `ubus` call LLVM module verified again.
The current receipts are `build-mips/roundtrip-bitfields-dev/results.json` and
`build-mips/bitfield-coverage/qualification.json`. Successful temporary
objects/executables were removed; current source, hashes and compact receipts
remain. This qualification used macOS arm64 Release, not Linux/Windows hosts
or the still-unsupported profiles/extensions listed above.

The microMIPS follow-up passed all 294 MIPS tests. The broader shared-boundary,
pointer projection and metadata run selected 1,298 cases: 1,252 passed and 46
existing fixture/toolchain-dependent cases were skipped, with no failures.
All 344 classic source groups and 20 microMIPS source groups passed on macOS
arm64 Release. The micro groups cover 44 inputs across both byte orders and
both C backends. Import-memory regressions cover ordinary, partial, merge and
dynamically indexed reads; adjacent ordinary data and external parameter
storage remain admitted. Compact receipts are in
`build-mips/compressed-ase/checkpoint.json`; the roundtrip producers remove
their generated sources, objects and ELFs after their readers finish.

The initial DSP32 addition passed all 313 MIPS tests. Its shared regression
selected 1,317 tests: 1,271 passed and the same 46 fixture/toolchain-dependent
cases were skipped. All 344 classic and 20 micro source groups still passed.
The nine DSP leaves passed 72 source groups at `-O0`/`-O2` and 1,080 scalar and
post-return DSPControl observations. Receipts distinguish 24 original-model
zero-destination flag disagreements from generated-code results, which all
matched independent expectations. Standalone RA writes, module/shard target
attributes and generated helper scope have dedicated regressions. Compiler
probes separately execute guarded wide arithmetic and non-inlined helpers.
Raw results and managed cleanup receipts remain under
`build-mips/compressed-ase/` and `build-mips/roundtrip-dsp-final/`; this does not
qualify the DSP/MSA families listed as unsupported above.

## TP-Link firmware workflow

Start with the actual extracted ELF profile instead of assuming a device is
MIPS64. The official Archer C7 V5 US firmware selected for this work contains
387 ELF children, all ELF32 big endian MIPS. Of these, 385 declare MIPS32r2/o32;
two historical MIPS I objects remain outside the initial profile.

Firmware unpacking precedes NeverD instruction analysis. This image exposes an
XZ-compressed SquashFS filesystem directly, so tpbreak is unnecessary. For
supported encrypted formats, [tpbreak](https://afflicted.sh/blog/posts/tpbreak.html)
can help obtain the contents; it does not supply MIPS instruction semantics or
prove that modified firmware can be signed.

See [the Archer C7 analysis receipt](firmware/archer-c7-v5.md) for the official
source, hashes, extraction boundary and selected analysis target. Firmware
containers/mapping are tracked separately by [#1727](https://github.com/NeverSight/NeverD/issues/1727);
the ISA work belongs to [#97](https://github.com/NeverSight/NeverD/issues/97).
