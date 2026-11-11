# MIPS analysis

The experimental MIPS analysis path targets linked ELF programs with classic
MIPS32 Release 2/o32 or MIPS64 Release 2/n64 instructions, in either byte order,
the bounded microMIPS32, MIPS16e32 and MIPS16e2 Release 2/o32 subsets, and the MIPS32
Release 5/o32 MSA integer, permutation, floating-point and control forms described below.
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
- `SWL`/`SWR` share a partial-store contract across classic and MIPS16e2
  encodings. Only selected bytes are written; no neighboring read or whole-word
  read/modify/write is introduced. The emulator validates the full write span
  and observed source bytes before committing. Source image stores require a
  bounded writable backing and preserve guest alignment separately from its
  relocated host address. Source helpers use explicit classic MIPS mode when
  called from compressed code.
  A partial store also ends stack-memory argument recovery. If a known callee
  requires a slot whose value can no longer be proved, source retains an
  explicit unknown-value trap instead of omitting it or inventing zero. This
  also preserves required unknown slots when the bounded setup proof cannot
  finish. Unproved padding is conservatively refused in either case; ordinary
  pair padding from a complete setup walk keeps the existing ABI path.
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
The physical occurrence proof also accepts four-byte MIPS16e2 merge loads;
two-byte slots and unproved instruction modes remain excluded.
Concrete LowIR transport and source parameter inference have separate validation
boundaries; this source proof does not initialize absent machine registers.

Classic `DIV`/`DIVU` and MIPS16 word division capture both operands and define
HI/LO together. Signed division truncates toward zero; remainder follows the
dividend's sign. MIN32/-1 produces the truncated word quotient and zero
remainder. A zero divisor makes both LowIR results unknown, without inventing
an arithmetic exception or adopting a particular CPU's value. On MIPS64, both
word inputs must be sign-extended words and each result is sign-extended.

Generated source has an explicitly narrower domain: **every retained division
that executes requires a nonzero divisor**. Its runtime domain check is not a
MIPS exception. A fully dead division can disappear; checking the divisor after
a live division does not extend the source domain to zero, even when that path
never reads HI/LO. C and LLVM use unsigned word magnitudes and wrap signs, so
MIN32/-1 causes neither signed overflow nor a 64-bit runtime division helper on
o32. Word division's pure value effect is shared by DCE and leaf stack-return
proofs; fresh unknown results remain ineligible for common-subexpression reuse.
Inferred MIPS integer parameters retain their full o32/n64 register carrier.
Reading only a low byte or halfword does not prove that incoming upper bits
have the extension a narrow C parameter would require. Declared source types
remain a separate contract.

This is a strict subset. n32, historical MIPS I–V profiles, Release 6,
microMIPS64, scalar floating point, MSA/DSP forms beyond the slices below, privileged
instructions and LL/SC reservations outside the qualified MIPS16e2 LowIR slice below,
doubleword merge stores (`SDL`/`SDR`), doubleword division and trapping arithmetic are not admitted by
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
`PRECEQ.W.PHL/PHR`, `PRECEQU.PH.QBL/QBR/QBLA/QBRA`, and
`PRECEU.PH.QBL/QBR/QBLA/QBRA` preserve the specified byte/halfword positions;
PRECEQU appends seven fractional zero bits. `REPL.QB/PH` and `REPLV.QB/PH`
replicate immediate or register values, with signed ten-bit REPL.PH immediates.
`BITREV` reverses the low sixteen bits and zero-fills the upper half;
`RADDU.W.QB` sums the four unsigned RS bytes (maximum 1020). All these forms
leave DSPControl unchanged; reserved source/immediate bits are checked.
Variable counts are reduced modulo the lane width. Rounded arithmetic shifts
round in widened signed precision, including the identity at count zero.
Pack operations select the upper half of each source lane. These forms leave
DSPControl untouched and need no incoming DSPControl value, including when
their destination is `$zero`. Decoder reuse consumes the current
image's feature evidence; Capstone recognizing an encoding without that
evidence does not authorize its lift. DSPR2 is admitted when the ABI flags contain bit 0x2, alone or with the DSP bit.
The 46 revision-two instructions cover packed halving add/subtract, wrapping and
saturating halfword arithmetic, fractional multiplication, byte comparisons with
both GPR and DSPControl results, shifts, packing/alignment and cross-lane dot
products in all four accumulators. Discarding a GPR destination preserves sticky
flags and the specified AC0 clobbers. BALIGN positions 0 and 2 produce an
unpredictable destination. The classic DSP profile also includes indexed
loads, INSV, accumulator shifts, banked multiply/accumulate, CMP.PH, MTHLIP
and BPOSGE32. For CMP.PH only the two low condition bits are specified.
For MTHLIP an incoming POS greater than 32 makes only POS unpredictable.
The classic MIPS32/o32 profile covers both byte orders. [MIPS DSP instruction manual](https://docs.alexrp.com/mips/mips32_dsp_v3_01.pdf)

`ADDQ_S.W`/`SUBQ_S.W` saturate signed words and set sticky overflow bit 20.
`ADDSC` replaces carry bit 13 with unsigned carry-out; `ADDWC` adds that input
carry, wraps its word result and sets signed overflow without replacing carry.
These effects survive a `$zero` destination. `MODSUB` selects the 16-bit last
index from RT[23:8] when RS is zero, otherwise subtracts RT[7:0].
`CMPU.EQ/LT/LE.QB` replace the four condition bits; `PICK.QB/PH` select each
complete lane from RS or RT using condition bits 24 upward. Other state stays
unchanged, and pure MODSUB needs no incoming DSPControl.

`ABSQ_S.PH/W` clamp the minimum signed input to the signed maximum and set
sticky bit 20. `SHLL.QB/PH`, `SHLL_S.PH/W` and their variable forms set sticky
bit 22 when significant bits are lost; saturated signed results clamp to their
lane's signed limits. Counts are masked to the lane width, while reserved
immediate bits are rejected. All these state effects survive `$zero` outputs.
The revision 1.00 manual's SHLL_S positive-overflow pseudocode conflicts with
its signed-saturation description. The admitted result is `0x7fff`/`0x7fffffff`,
checked against independent original-target execution; the contradictory
all-ones pseudocode is not used as the oracle.
`CMPGU.EQ/LT/LE.QB` return four predicates in GPR bits 3..0 without observing
DSPControl. `CMP.EQ/LT/LE.PH` replace the two low condition bits. LowIR
tracks the two unpredictable upper condition bits separately. Native source
observes those bits on the issuing processor when they are used.

`MULEQ_S.W.PHL/PHR` expand a signed Q15 product to Q31, saturating
negative-one times negative-one to `0x7fffffff` and setting sticky OUFLAG21.
Like the other GPR-targeting multiplies, they invalidate HI/LO even for `$zero`.
`PRECRQ_RS.PH.W` widens before adding `0x8000`, saturates positive overflow,
and packs the high halfwords. `PRECRQU_S.QB.PH` does not round: negative Q15
values become zero, values above `0x7f80` become 255, and the remainder contributes
bits 14..7. Both pack operations set sticky OUFLAG22 on saturation, including
when the destination is `$zero`; `0x7f80` and `0x7f81` produce equal bytes but
different flag updates. HI/LO are preserved by these pack operations.

`MULEU_S.PH.QBL/QBR` saturate unsigned byte-by-halfword products to 65535.
`MULQ_RS.PH` rounds signed Q15 products toward positive infinity at halfway
values and saturates the pair of -1 operands to 32767. Both families set sticky
DSPControl bit 21 and leave HI/LO unpredictable, including with a `$zero`
destination. Explicit accumulator writes can redefine either value;
source refuses a retained observation of an unpredictable result.

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

AC0 aliases classic HI/LO; AC1–AC3 have separate named high/low slots owned by
`MIPSRegisters.def`. Banked MFHI/MFLO/MTHI/MTLO and DPAU/DPSU.H.QBL/QBR,
DPAQ_S/DPSQ_S.W.PH and MULSAQ_S.W.PH are admitted. Byte products accumulate
without truncating the dot product; fractional halfword products saturate only
MIN×MIN, set sticky OU16+AC, and accumulate modulo 64 bits. Accumulator inputs
must be defined on every source path and survive the ABI call-clobber proof.
DPAQ_SA/DPSQ_SA.L.W saturate the fractional word product and the final
signed 64-bit sum or difference. MAQ_S/MAQ_SA.W.PHL/PHR select the high
or low halfword of each input. The SA variant also clamps the accumulator
to a signed word. Each saturation sets the bank's sticky OUTFLAG bit.

EXTR[V].W, EXTR[V]_R.W, EXTR[V]_RS.W and EXTR[V]_S.H implement immediate
and register counts. Word forms check the arithmetic shift and its rounded
value for OUTFLAG[23], including when the returned word is unrounded.
EXTP[V] and EXTPDP[V] extract size + 1 bits ending at POS. Failure sets EFI,
preserves POS and yields an unknown GPR value. A successful DP extraction
advances POS modulo 64. Source code traps if it needs an undefined result.
DSP64 and compressed DSP still require qualification.

Classic AC0 partial writes preserve the pre-R6 unread-result restriction:
MULT/MULTU/DIV/DIVU (and supported doubleword multiply) establish pending
history; MFHI/MFLO consume it even when their destination is `$zero`. A partial
write before consumption makes the other half unpredictable. A pure transport
intrinsic carries unknown bits in LowIR; source propagation must discharge it
or discard its dead output before ABI recovery. Unknown live history refuses.
This differs from the removed MIPS I–III two-instruction read-to-write hazard.
The rule does not couple AC1–AC3 halves.

The operation rules follow the
[MIPS32 DSP ASE manual, revision 1.00](https://0x04.net/~mwk/doc/mips/MD00374-2B-MIPS32DSP-AFP-01.00.pdf).
The pinned Unicorn model is checked with Status.MX enabled. Its packed
arithmetic, shift and admitted GPR-multiply translators preserve DSPControl
effects for zero destinations and still check the enabled state. The source runner rejects any
original/generated disagreement with the independent expectations. Historical
receipts below retain the zero-destination defect of the older reference.

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

Short and extended `SAVE`/`RESTORE` have a distinct encoding kind. Their ordered
word accesses reuse the ordinary memory owner. SAVE writes incoming argument
homes first, followed by ra, extended saved registers, s1, s0 and static argument
registers in descending stack addresses. RESTORE loads only the static/saved
registers from the old SP plus frame size. SP changes after every access
succeeds; a fault retains previous individual effects and the old SP. Frames
with no accesses impose no alignment check. Short frame zero means 128 bytes;
extended zero means zero bytes. Reserved aregs 15 is refused. A short frame can
occupy a two-byte delay slot; the transfer still captures its target/link first.
These operations are not alignment padding during function discovery.
In a leaf, saving other registers does not by itself invalidate the saved
return link; overlapping and unresolved writes still invalidate its proof.

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

MIPS16e2 r2/o32 requires both MIPS16 (0x400) and MIPS16e2 (0x4000) in an
explicit ABI-flags record. Reused decoders refresh this evidence. The admitted
four-byte EXTEND forms are GP-based ADDIU, ANDI/ORI/XORI/LUI, GP-relative
LB/LBU/LH/LHU/LW/SB/SH/SW, LWL/LWR/SWL/SWR, EXT/INS (including zero-source INS),
and MOVZ/MOVN/MOVTZ/MOVTN (including zero sources). Field limits and reserved
bits are checked before normalization into the existing classic owners.
T moves use register 24; their physical text spelling does not replace the
canonical condition operand. EHB and all 32 SYNC stypes are admitted as
ordering intrinsics shared with classic MIPS32/64. The current compressed slot
path requires exactly two physical bytes and refuses four-byte e2 slots; this
is a support boundary, not a claim that every such slot is architecturally
invalid.

The encoding/operation authority is
[MIPS16e2 Volume IV-a, MD01172 revision 1.00](https://jnastarot.io/arch_index/pdf/MD01172-2B-MIPS16e2-AFP-01.00.pdf).
The GNU fixture uses `-mmips16e2 -mmt` to publish both features in ELF
`.MIPS.abiflags`. Local `.set` directives only enable the instructions for assembly.
The pinned Unicorn predates e2, so it is **not** an original-e2 oracle. Literal
GNU bytes, independent LowIR expectations and execution of generated old-MIPS16
source are separate checks. Hardware or independent original-e2 execution
remains unavailable. These compressed system operations use the MIPS32/o32 profile. The following system/atomic slice is
qualified at LowIR, not as native source or original-e2 hardware execution. No path treats a misdecoded operand or unknown instruction as
valid semantics. The immediate, memory, call and slot rules follow
[MIPS16e Volume IV-a, revision 2.63](https://jnastarot.io/arch_index/pdf/MD00076-2B-MIPS1632-AFP-02.63.pdf)
and were cross-checked against the pinned Ghidra definitions linked above.

MIPS16e2 `LL`/`SC`, `RDHWR` (indices 0–3 and 29–31), `DI`/`EI` (with or
without a result), `MFC0`/`MTC0` with a five-bit register and three-bit selector, and `PAUSE` retain explicit effectful intrinsics. Reserved fields and
feature evidence are checked before effects. SC captures the original address
and stored value before overwriting its GPR with 0/1. An unused result cannot
remove the memory/reservation or system effect. SC ends stack-memory argument
recovery, so an earlier outgoing stack value cannot survive a conditional write.
Classic LL/SC and general privileged instruction admission are unchanged.

`NdOpEmulator::setMIPSReservationModel` explicitly selects a MIPS32 flat,
synchronizable ordinary-RAM model with identity virtual/physical addresses,
Config5.LLB=1 and a supplied power-of-two coherence granule of at least four
bytes. It assumes no unreported interference or cache eviction. Incoming
reservation knowledge is unknown until LL or an explicitly reported clear.
Aligned mapped reads establish or replace the word reservation. SC checks full
mapping and write permission even on a known failure, matches the LL address,
and clears the reservation after completion. Same-granule committed stores
invalidate it even after an ABA change; unknown calls, skipped unknown effects
and reset forget it. `clearMIPSReservation` reports an observed ERET, coherent
invalidation or spurious failure without changing memory. Rejected operations
leave architectural outputs/reservation state unchanged; rejection is not
modeled guest exception delivery. Both strict and lenient execution require the
same admission. With this profile enabled, ordinary LowOp inputs must be fully
known even in lenient mode; partial-unknown transports are conservatively refused.
MIPS scalar intrinsics keep their own narrower observed-byte rules. Skipping a
budget-limited ordinary write forgets reservation knowledge; an operation that
stops without writing preserves it. Virtual aliases, MMIO, arbitrary SMP and unreported events are
outside this declared model.

`stepMIPSSystem` binds a caller-authenticated `MIPSSystemContext` for one
operation. RDHWR requires CP0 usability or the selected HWREna bit and consumes
a matching hardware-register observation on success. It never guesses Count
progression or the existence of UserLocal. DI/EI require known Status and CP0
permission, return the old word, change only IE and mark an execution hazard.
While that obligation is pending, only a bound EHB can clear it; SYNC cannot.
Unbound system execution refuses. Context authentication and interrupt/event
sequencing belong to the caller; this API does not simulate interrupt delivery,
CP0 pipeline timing or privilege transitions.

`MIPSCP0.def` admits selector-zero reads of Status (12), EPC (14), and
ErrorEPC (30), and writes to EPC/ErrorEPC. These are explicit software-visible
32-bit word views: all bits, including the low ISA-mode marker, are preserved.
They do not change the current decode mode or implement exception return.
Status reads share the DI/EI state owner. CP0 usability is required independently
of HWREna; an unknown read or write operand refuses. A successful write establishes
that view and an execution-hazard obligation, discharged by bound EHB. Board-specific CP0 reads and writes, including masked Status writes and Count or Config views, require a bound `MIPSCP0Platform`. Its callback performs the transfer on the issuing context and can decline unsupported views. RDHWR 30/31 consume a matching implementation-specific observation.
The explicit MT ABI feature (0x40) additionally enables the built-in read views of MVPControl
(0,1), VPEControl (1,1) and VPEConf0 (1,2). The intrinsic key is `(sel << 5) | reg`,
so existing selector-zero identities remain stable. DMT/EMT update only TE;
DVPE/EVPE update only EVP when VPEConf0.MVP is set. Each returns the prior
control word, including retained effects when the result is discarded, and
requires CP0 usability, declared MT implementation and known bound state.
Bound EHB permits a subsequent control-register observation. Disabling peer
issue takes an instruction hazard barrier such as JR.HB or ERET. A caller
observing that boundary invokes `completeInstructionHazardBarrier()`.
Until then the affected TC or VPE gate returns unknown. The two gates have
independent scopes. Which runnable peer a scheduler selects belongs to the
platform. The [MT manual rev1.12, p42](https://docs.alexrp.com/mips/mips32_mt.pdf)
resolves the inverted MVP condition in the older e2 pseudocode. The pinned
Unicorn MT helpers are stubs and are not a semantic oracle for these forms.

CACHE/PREF preserve the five-bit operation/hint, signed nine-bit offset and
32-bit effective address as effectful LowIR intrinsics. Both may write memory
(PREF PrepareForStore included), so they end outgoing stack-memory setup proofs.
Dead-code elimination retains both. A bound `MIPSCachePlatform` translates
the virtual address as a data read and receives the selected cache, exact
operation or hint, original instruction address and physical address. CACHE
requires CP0 usability. PREF suppresses address exceptions and uncached
accesses. Missing translation evidence stops the step. A synchronous board
can use single-line maintenance for index/hit invalidation, dirty writeback,
fill and fetch-and-lock. Eviction uses the old physical tag and line contents.
Line geometry, tag layout, replacement policy, coherence and bus errors
depend on the board. Native C projection of these privileged effects is unavailable.
The EPC/ErrorEPC software word view is also described in the vendor
[CPU reference manual, sections 50.13.25 and 50.13.55](https://ww1.microchip.com/downloads/aemDocuments/documents/WSG/ProductDocuments/ReferenceManuals/PIC32_Family_Reference_Manual_CPU.pdf).

`PAUSE` retains an effectful intrinsic and is forbidden in a delay slot. In the
explicit reservation model it completes only when LLBit is known clear; a held
or unknown reservation refuses without changing it. An observed external clear
may then permit execution. No scheduling, interrupt wakeup, or EPC update on the
blocked/descheduling path is inferred. Classic PAUSE is also refused outside
this admitted profile, rather than falling through the zero-destination SLL rule.

HighC, LLVM and LLVMC explicitly refuse native lowering of this stateful slice.
An isolated LL/SC asm helper would permit compiler spills or calls inside the
reservation interval; ordinary loads/stores or compare-exchange are not a
replacement. A future native-source contract must own the whole reservation or
privileged execution region. Encoding and model tests are separate from native
hardware equivalence and source execution qualification.

EHB/SYNC remain effectful through SSA and dead-code elimination. They order
execution or memory but do not overwrite saved data, so a saved-return proof
can cross them. Native source uses non-inlined classic-mode helpers containing
the original barrier and exact stype, including under compressed compiler
defaults. The LowIR emulator observes only its sequential ordinary-memory
contract when no system context is bound; a bound EHB additionally clears the
explicit execution-hazard obligation described above. No multicore,
device/cache, privileged hazard, or original-e2 execution equivalence is
claimed. In particular the qualified native-source domain has no incoming
CP0/privileged execution hazard: helper calls and their instruction fetches
precede the EHB and cannot establish equivalence for arbitrary hazard states.
Source inspection checks that the barriers and helper noinline attributes
survive optimization without a global `-fno-inline` option.
Selected-function output includes its helper definitions. Input symbols with
the same spelling cannot be reused as an internal barrier implementation.

## Classic MIPS32 MSA vectors and control registers

The r5/o32 integer profile includes horizontal add/subtract, widening dot
products and their accumulates, mixed-signedness saturated subtraction, and
Q-format multiply/add/subtract with optional rounding. DIV/MOD observe the
issuing CPU's result for a zero divisor lane. Concrete portable evaluation
covers defined inputs, and original-target source uses the MSA instruction.
LD/ST use signed element-scaled offsets, allow unaligned addresses, and
preserve target byte order within each element. LDI, MOVE.V, scalar element
moves and the immediate mask operations share the vector register bank.
BZ/BNZ test their predicate before the delay slot. The element forms test
for any zero or all nonzero lanes, while .V tests the whole register.

The initial MSA slice requires classic MIPS32 Release 5/o32 ELF in either byte
order, with `.MIPS.abiflags` explicitly declaring MSA, 128-bit CPR1 registers
and FP64 ABI tag 6. A reused decoder consumes the current revision as well as
feature evidence; architecture-only initialization remains Release 2 without
MSA. Other revisions, MSA64, microMIPS/MSA and DSP/MSA combinations are refused.

`FILL.W`, `INSERT.W`, `COPY_S.W`, all four B/H/W/D formats of `ADDV` and
`SUBV`, `CEQ`, `CLT_S`/`CLT_U`, `CLE_S`/`CLE_U`, `MIN_S`/`MIN_U`,
`MAX_S`/`MAX_U`, `SLL`/`SRL`/`SRA`, `BCLR`/`BSET`/`BNEG`, `MULV`/`MADDV`/`MSUBV`, `MAX_A`/`MIN_A`,
`ADD_A`/`ADDS_A`, `ADDS_S`/`ADDS_U`, `SUBS_S`/`SUBS_U`,
`AVE_S`/`AVE_U`, `AVER_S`/`AVER_U`, and `AND.V`/`OR.V`/`NOR.V`/`XOR.V` use exact encoding descriptors in
`MIPSMSA.def`. Additional forms are `ANDI/ORI/NORI/XORI.B`,
`BSEL/BMNZ/BMZ.V`, and B/H/W/D formats of `PCNT`, `NLZC`, `NLOC`,
`SLLI/SRLI/SRAI`, `BCLRI/BSETI/BNEGI`, `ASUB_S/ASUB_U`, `SAT_S/SAT_U`,
`ADDVI/SUBVI`, `CEQI`, `CLTI_S/U`, `CLEI_S/U`, `MAXI_S/U` and `MINI_S/U`.
SRAR/SRLR/SRARI/SRLRI and BINSL/BINSR/BINSLI/BINSRI support B/H/W/D too.
Rounded shifts add the highest discarded bit after shifting. A zero count
preserves the lane without an invalid shift. Insertion copies count+1 source
bits into the destination. A full-width immediate insertion can initialize an
unknown destination. Register-count insertions require the prior destination
in strict emulation, including when every runtime count selects a full lane.
The lifter captures aliased inputs before the vector write. Rounded shifts
only depend on the source and count.
The I5 arithmetic operands are unsigned; CEQI and the signed comparisons/extrema
sign-extend the five-bit immediate. Comparison masks cover the entire element,
including 64-bit lanes.
Absolute difference preserves the full unsigned magnitude, including signed
min/max pairs. Saturation clamps to the signed/unsigned range of m+1 bits while
retaining the lane width; m=0 and full-width saturation are defined. BIT immediates have a prefix-coded
width; the reserved `1111xxx` field is rejected. Counts define the zero/full
lane cases without poison. BSEL uses old WD as its mask; BMNZ/BMZ use WT and
merge selected WS bits with old WD. Every input is captured before writing an
aliased destination. The 32 W registers are 128-bit containers. Lane zero occupies
the low bits in both byte orders; addition and subtraction wrap independently
within each 8/16/32/64-bit lane. Integer comparisons set every bit of a true
lane to one and every bit of a false lane to zero; signed and unsigned ordering
have separate rules. Min/max selects the complete winning lane using the same
signed/unsigned ordering, including 64-bit extrema. Shifts and bit edits use
the low log2(lane width) bits of each corresponding count/index lane. Multiply
retains the low lane-width product independently for every lane. MADDV/MSUBV
add/subtract it from the captured old destination lane. Absolute min/max uses
unsigned magnitudes, so the minimum signed value has the greatest magnitude;
equal magnitudes select WT, including opposite signs. Logical
operations consume all 128 bits. Absolute addition uses unsigned magnitudes;
ADD_A wraps, while ADDS_A clamps their sum to the signed maximum, including
MIN+MIN. Signed and unsigned saturating add/subtract use their respective lane
ranges. AVE rounds the widened sum down; AVER rounds it up, including negative
signed ties. The implementation avoids overflowing a host signed integer or
requiring a 128-bit carrier for each 64-bit-lane sum.
Inputs are captured before an overlapping destination is written. F registers
share the low 64 bits of their corresponding W register.
Scalar floating instructions remain unsupported: their future implementation
must model the unpredictable remaining W bits, not preserve or zero them by
assuming a generic subregister write.

LowIR accepts all 32 W registers. Source qualification is narrower and runs
before SSA/dead-code elimination. Each vector input must be initialized on every incoming leaf path. Calls and writes to the saved
F20/F22/.../F30 low views require a separate ABI proof. Without an Image, all
W20..W31 writes are refused because other o32 FPABIs preserve additional views.
MSA64 source is refused at this boundary too; no extra vector parameter is
invented. Returns must retain the
captured entry RA, including when `COPY_S.W` writes RA in a delay slot. General
vector ABI and saved-FPR restoration remain future work. Both C paths use Clang's bit-precise integer support for any surviving
128-bit scalar temporaries on MIPS32.

Big-endian MSA scalar source carries a `-msa` LLVM feature and a Clang
`target("no-msa")` C constraint, including helper functions. Clang 22.1.6
miscompiles a vectorized unsigned 64-bit comparison against 32: its MSA
constant-splat bitcast emits `LDI.D 32; SHF.W 0xb1`, changing the bound to
`32 << 32`. The scalar C and optimized LLVM comparison remain correct; the
constraint prevents that instruction selection while retaining scalar
optimization. The compiler's
[constant-splat selector](https://github.com/llvm/llvm-project/blob/llvmorg-22.1.6/llvm/lib/Target/Mips/MipsSEISelDAGToDAG.cpp)
also documents big-endian bitcast limitations. This is a source-compiler
boundary, not a change to MSA instruction semantics. Little-endian and known
non-MSA images keep their existing profile.

`MIPSSourceCompiler.h` composes this constraint with DSPControl preservation.
Image-less big-endian MIPS32 source needs both; one complete target attribute
is emitted because separate declaration and pragma attributes do not compose
in Clang. LLVM functions, declarations, C source shards and helpers retain the
same constraints. The original-versus-generated source matrix includes the
`CLEI_U.D 31` case that exposed the compiler defect.

Integer permutation forms cover ILVEV/ILVOD, ILVL/ILVR, PCKEV/PCKOD, VSHF,
SLD/SLDI, SPLAT/SPLATI and INSVE in all four widths, and SHF.B/H/W. SLD
slides byte columns within each row. VSHF uses the captured old destination
as its selector and zeros lanes selected by either of bits 7 and 6.

The MSA floating-point forms in `MIPSMSAFloat.def` include arithmetic,
fused operations, estimates, conversions, classification and ordered or
unordered comparisons. FCLASS.W/D expand to integer LowIR and distinguish
all ten IEEE classes. Other forms carry their original sources, fused
destination when needed, and an ordered MSACSR effect through LLVM and both
C paths. Original-target native helpers execute the encoded FP instruction
in the caller's FP environment. A P5600 executor can bind to LowIR for
concrete evaluation. In LowIR, enabled FP exceptions commit MSACSR while preserving
the old vector destination. Native source comparisons observe the trap,
MSACSR and prior memory effects. Physical exception frames and handler EPCs
belong to the compiled program. Approximation and NaN results depend on the
target implementation.

CFCMSA/CTCMSA cover all 32 selectors. The concrete context carries MSAIR,
the writable MSACSR mask, and optional privileged partition control registers
with the number of free physical registers. Native floating-point helpers
require WRP clear because their scratch register numbers differ from the
original. The vector execution profile requires MSA enabled with CU1/FR
configured. General partitioned vector execution, disabled-unit exception
delivery, scalar FPU, MSA64 and compressed MSA remain outside this profile. The operation and alias rules follow the
[MIPS32 MSA manual, revision 1.12](https://docs.alexrp.com/mips/mips32_simd.pdf), sections ADDV, SUBV, CEQ, CLT/CLE, MIN/MAX,
SLL/SRL/SRA, BCLR/BSET/BNEG, MULV/MADDV/MSUBV, MAX_A/MIN_A, ADD_A/ADDS_A, ADDS/SUBS, AVE/AVER,
AND/OR/NOR/XOR, ADDVI/SUBVI, CEQI, CLTI/CLEI, MAXI/MINI,
COPY_S, FILL and INSERT. Owned execution
fixtures validate scalar results, output bytes and the ABI-preserved low FPR
views against an enabled P5600 reference. The pinned Unicorn includes fixes
for signed host-limit construction and absolute `INT64_MIN` magnitudes, and
preserves QEMU's wrapping-arithmetic build contract. Every reference lane,
saved register and regenerated result must match the independent expectation.
No unimplemented W-register API is used as an oracle.

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

The [2026-11-10 qualification](evidence/mips16e2-dsp-msa-2026-11-10.json)
records EHB/SYNC, 13 additional DSP forms and 43 additional MSA forms. It also
records the shared ABI repair found during dev integration: pair-aligned stack
arguments retain their actual reaching stores, wide lanes follow target byte
order, and missing predecessor lane evidence leaves required arguments unknown.
The final integration has separate build, dependency and executable hashes from
the performance comparison. Both C backends still produce byte-identical Archer
C7 `ubus` call-handler and discovered-function output against the baseline.

MIPS16 delay-slot queries now use the shared jump normalizer instead of fully
normalizing each scalar instruction. The performance qualification separates an
identical-dependency normalizer control from the complete feature increment,
using seven warmed, alternating pairs on the same Release baseline. It covers
short and mixed MIPS16 streams, 12 decode/lift profiles and complete firmware
requests, retaining CPU, wall, RSS and output fingerprints separately. Newly
supported forms have their own throughput measurements. Local results and a
rejected barrier-dispatch experiment remain in the ignored
`build-mips/mips16e2-next/qualification.json`; they do not establish universal
improvement or a cross-tool speed ranking.

The [system/atomic and packed-integer increment](https://github.com/NeverSight/NeverD/pull/2293)
qualifies LL/SC, RDHWR and DI/EI at their explicit LowIR state boundaries,
three packed DSP multiplies, and ASUB/SAT in all four MSA lane widths. Release
verification after the shared ABI/LLVM dev integration passed 2,002 CTest
cases with 82 host, compiler or fixture skips; all 539 MIPS cases ran. All six
source matrices passed 3,604 groups, plus 28 stateful e2 source refusals and
standalone/wide-value controls. The four Archer C7 `ubus` outputs remained
byte-identical. Original e2 hardware and native stateful source execution are
still outside the qualified domain described above.

The feature comparison retains seven warmed alternating pairs across fourteen
profiles, long/mixed compressed streams and complete firmware requests. Newly
admitted forms have separate throughput measurements. The later shared-dev
integration has its own build/products, source matrices, firmware checks and
42 matching primitive fingerprints; its timing is not inferred from the earlier
feature products. Exact receipts and compressed source evidence remain in
`build-mips/mips-system-atomic/`, including `qualification.json` and
`final-integration.json`. Test executables and one-use comparison/dependency
builds are removed after their last readers; the current CLI/SDK, incremental
cache and qualified reference library are retained.

The [CP0/PAUSE, DSP packing and MSA I5 increment](https://github.com/NeverSight/NeverD/pull/2301)
adds the bounded CP0 word views and PAUSE model above, four DSP forms and
eleven MSA immediate families in all four lane widths. Independent original
DSP/MSA instruction checks passed 7,464 observations before the source
comparisons. The full integrated Release selection passed 1,578 cases with
36 explicit host/compiler/fixture skips. The subsequent compiler-helper fix
passed all 126 MIPS pipeline cases. The later shared C integration selection
passed 1,243 cases with 73 explicit skips. The qualification and build identities
are recorded in the [qualification receipt](evidence/mips-cp0-immediate-2026-11-11.json).

The six source matrices qualify 4,724 groups. The 1,252 classic, microMIPS,
MIPS16 and MIPS16e2 groups execute on the final products; the 648 DSP and 2,824
MSA groups retain their completed execution evidence only after regenerating
all 1,736 distinct C outputs and matching complete source, input ELF,
toolchain and reference-library hashes. This is explicit reuse of identical
source evidence, not a claim that those executions were repeated. There are
also 52 stateful e2 source refusals, 24 standalone DSP execution groups,
12 external compiler feature probes, four live MSA wide-carrier groups and
four wide-storage/declaration checks. The compiler probes include DSPControl
read/write helpers under adverse `-mdsp -mmsa` defaults. All four Archer C7
`ubus` outputs retain their previous hashes.

Performance evidence separates the same-base feature comparison, long/mixed
compressed streams, newly admitted instruction throughput and the later shared
dev/source-compiler changes. Seven warmed alternating pairs retain CPU,
wall time, memory and output fingerprints; the final firmware comparison uses
20 complete requests per sample with matching snapshot layouts. These runs
do not establish universal improvement or a cross-tool ranking. Raw reports,
compiler-failure traces, recovery indexes and cleanup receipts remain in
`build-mips/mips-cp0-immediate/`; completed raw reports are compressed and
reproducible test/snapshot products are retired after their final readers.
The [CP0/MT, DSP accumulator and MSA qualification](evidence/mips-system-accumulators-2026-11-11.json)
covers the selected CP0 views, the four MT controls, CACHE/PREF ordering,
banked accumulator transfers, seven DSP dot-product families and 32 MSA rounded
shift and bit-insertion forms. The MT tests check prior control values, master
VPE permissions and execution hazards. The DSP and MSA tests compare the original
instructions and the recompiled C with independent expected values in both byte
orders. That qualification covered the cache instruction operands and execution
hazards. The later cache and MT state contracts are described above.

The MIPS16 comparison includes the original mixed instruction stream. One
multiply and two HI/LO reads now emit three additional LowIR operations to
track an unread accumulator result. A second comparison uses identical LowIR
on both versions. The firmware workload checks the Archer C7 V5 `ubus` call
handler at `0x4012d0` and the discovered functions through both C backends.
The selected LLVMC handler changed block and return layout. Running the original
handler with five modeled imports and both recompiled C views at `-O0` and
`-O2` matched all twelve input cases. The other three firmware outputs kept
their prior hashes. Local samples, build identities and cleanup receipts are in
`build-mips/mips-system-accumulators/`.

The [cache, MT, DSP and MSA qualification](evidence/mips-cache-mt-dsp-msa-2026-11-11.json)
records 18 DSP forms, 51 MSA permutations, 102 MSA floating-point forms and
both control transfers across all 32 selectors. The big- and little-endian
74Kf/P5600 runs compared 208 assembly functions with 1,664 recompiled outputs
from HighC and LLVMC at `-O0` and `-O2`. All 45,520 execution comparisons
matched, including discarded results, DSPControl, MSACSR and enabled FP traps.
MIPS16e2 cache and MT cases use the bound platform model described above.
The receipt includes the input bytes and LowIR fingerprints for the
decode/lift benchmarks. Local paired timings and the single-function Archer
C7 comparison with Ghidra are in
`build-mips/mips-cache-mt-dsp-msa/qualification.json`.

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
