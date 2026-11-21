# MIPS analysis

The experimental MIPS analysis path targets linked ELF programs with classic
MIPS32/o32 and MIPS64/n32 or n64 instructions in either byte order. The profiles below
cover classic Release 2 integer, FR=0/FR=1 scalar floating-point and FR=1
paired-single instructions,
classic and microMIPS Release 6 integer and FR=1 floating-point instructions,
pre-R6 microMIPS scalar/paired-single floating-point and integer profiles,
MIPS16e32 and MIPS16e2, and Release 5/o32 and n64 MSA.
Classic MIPS IV/V n32/n64 also admit the implemented integer and floating-point
slice, with version-specific instruction checks described below.
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
- For fixed-address non-PIC executables, a global register-info record in
  `.reginfo` or `.MIPS.options` supplies the ordinary function-entry `$gp`.
  The corresponding program headers also work in sectionless files. n32 sign
  extends the ELF32 value into its 64-bit GPR. GP-relative accesses in the
  upper half of the n32 address space retain their ELF32 storage identity
  when generated code is relinked. Each later register write takes effect
  normally.
- In linked classic n32/n64 PIC and CPIC code, the ordinary entry can
  establish GP from its incoming `$t9` and a linker displacement. Source
  conversion follows saved copies of `$t9` across direct branches and loops.
  A branch-likely passes the pre-slot value on its skipped edge and the
  updated value on its taken edge. The classic `b` alias and
  `beql $zero,$zero` always execute their slot and have one successor.
  Every incoming path, including loop backedges, must establish the same
  value. Calls and independent entries end the proof. The completed write
  must match the image-wide register-info GP. This covers assembler `.cpsetup` and Clang's calculation
  in a temporary GPR. The caller's incoming GP and later register writes keep
  their own values. Scoped GP records cannot supply this proof. A full-width
  GOT import load retains its symbol through n32's sign extension. A static
  GNU GOT with a single GP and no TLS can supply the initial address of a
  writable callback object. The call reads the object's current pointer.
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
- `SWL`/`SWR` share a partial-store contract across classic, microMIPS and
  MIPS16e2 encodings. Pre-R6 classic MIPS64 and microMIPS64 also lift
  `SDL`/`SDR`. Each instruction writes its selected bytes in target byte
  order and preserves the surrounding bytes. The emulator validates the full write span
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
  for 64-bit integers. n32 and n64 use registers 4–11 and eight-byte argument slots.
  n32 selects ELF32 with ABI2 and a 64-bit ISA. Its pointers and C `long`
  occupy four bytes, while its general registers and saved link occupy eight.
  Big-endian integer pairs put their high word first, and narrow stack scalars
  are right justified. Single-carrier results use `$v0`; o32 64-bit results
  use `$v0/$v1` in target word order. These placement rules are tested
  separately from source-signature reconstruction. The fixed integer-call
  adapter still declines split o32 values, register-padding holes and narrow
  big-endian overflow slots; complete typed ABI reconstruction is not claimed.

  HighC preserves integer bit reads of declared floating parameters, including
  o32 floats following an integer in GPRs. Its emitted bit carrier retains
  signedness so logical right shifts receive the required unsigned view.
  Typed source calls marshal the emitted integer view before consulting the
  parameter's floating declaration. This keeps forwarded values as bits in
  ordinary calls and the shared AArch64 authenticated-call packet path.
  Classic, microMIPS and MIPS16 FR=0/1 leaves are checked in both byte orders
  against original machine code, with Clang and GCC at O0/O2. MIPS16 source
  attributes use compiler-specific spelling from `MIPSInstructionModes.def`;
  LLVM still retains both the selected mode and excluded alternative.
  Parameter placement distinguishes an o32 MIPS16 body from its classic
  entry stub: the body receives leading floating arguments in GPRs. The same
  mode enters call classification, source binding and descriptor validation;
  classic and microMIPS entry rules keep their FPR prefix. Both C routes retain
  the target's mode on declarations even when its body is omitted. A complete
  fixed MIPS16 signature supplies logical parameter types when all recovered
  GPR bytes match, including both words of an o32 double in target byte order.
  Missing inputs, stack and variadic entry adapters need separate proof.
  Classic entry stubs can inherit an exact body declaration only after their
  actual LowIR transfers and delay slot prove every parameter byte; a name
  alone never establishes that association. The original call graph shares
  the transport proof without requiring a prototype. MIPS16 floating results
  require the byte-proven dual GPR/FPR result adapters described below.
  General runtime call and return helpers remain unqualified.

LowIR execution retains the common known bits of the two permitted partial
MIPS64 `LWR` upper words. Scalar COPY, SUBBYTES, CONCAT and integer extensions
transport that uncertainty, so a later complementary load can produce a fully
known value. Public register reads refuse a view containing unknown bits;
stores, arithmetic and control also refuse any unknown bits they observe.
Narrow reads can still consume a defined low word. This does not select a CPU's
implementation-dependent behavior or give uninitialized registers a zero value.

Before inferring parameters, source recovery checks merge-load pairs in a
single block. Register arithmetic that constructs other call arguments may
appear between the loads. The instructions must be complementary at the addresses
required by the target byte order. Both reads retain their fault order, and
the second consumes the first result. The first load can discard its incoming
register bits only when every use of its result proves those bits unobservable.
A typed record call observes its explicit operands, live register views and
preserved register bytes. Its opcode and occurrence must match the original
instruction. An unbound call, a PHI, an exceptional edge or a branch prevents
the proof.
The physical occurrence proof also accepts four-byte MIPS16e2 merge loads;
two-byte slots and unproved instruction modes remain excluded.
Concrete LowIR transport and source parameter inference have separate validation
boundaries; this source proof does not initialize absent machine registers.

Classic, microMIPS and MIPS16 word `DIV`/`DIVU` capture both operands and define
HI/LO together. Signed division truncates toward zero; remainder follows the
dividend's sign. MIN32/-1 produces the truncated word quotient and zero
remainder. A zero divisor makes both LowIR results unknown, without inventing
an arithmetic exception or adopting a particular CPU's value. On MIPS64, both
word inputs must be sign-extended words and each result is sign-extended.
Pre-Release 6 classic MIPS64 and microMIPS64 also lift `DDIV` and `DDIVU`.
They consume two full 64-bit GPRs and write the 64-bit quotient to LO and
remainder to HI. In the signed case, MIN64/-1 produces MIN64 and zero.
Division by zero leaves both results unknown. The 32-bit microMIPS
encodings of `MFHI` and `MFLO` can read either result on n32 and n64.

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

Coverage remains incomplete for historical MIPS I–III profiles, floating-point
forms outside the qualified slices, trapping arithmetic and the MSA/DSP forms outside the
slices below. Privileged instructions and reservations are admitted by profile
as described under MIPS16e2 and classic Release 6. Unsupported instructions, missing slots and transfers in a
delay slot produce errors, not replacement NOPs. General aggregate source ABI
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
flags and the specified AC0 clobbers. The 74Kf reference exercises all three
CMPGDU predicates with zero, ordinary and aliased destinations, including the
DSP-disabled exception. BALIGN positions 0 and 2 produce an
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
and generated helpers, disables automatic DSP instruction selection.
Without an image, all MIPS32 source has the same
contract because no image profile rules out live DSPControl. This also covers
pure arithmetic with no state intrinsics, siblings, helpers and source shards.
Clang applies negative target attributes to each function, including when
the translation unit enables `-mdsp`. MIPS GCC can rebuild DSP-preserving C
with `-mno-dsp -mno-dspr2` on the translation unit, including n32 and n64
microMIPS functions. Generated C diagnoses GCC builds that enable automatic
DSP or DSPR2 instruction selection. Big-endian MIPS32 source that also
requires the `no-msa` target attribute still requires Clang.
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
EXTP[V] and EXTPDP[V] extract size + 1 bits ending at POS[5:0] from the low
word of each accumulator half. Failure sets EFI, preserves POS and yields an
unknown GPR value. A successful DP extraction subtracts size + 1 from the
complete POS field, modulo 64 on MIPS32 and modulo 128 on MIPS64 (including
n32). On MIPS64, immediate EXTP sign-extends its word result. EXTPV, EXTPDP
and EXTPDPV zero-extend theirs. Source code traps if it needs an undefined result.
MIPS64r2/n64 uses the word and packed DSP/DSPR2 operations from revision 3.02
of the MIPS64 DSP specification. Arithmetic consumes the low word of each
accumulator half and sign-extends each written half independently. The four
banked register transfers copy the entire GPR-width half. The pinned Unicorn
preserves the full width in all four banks, including non-word values in
AC1–AC3. These transfers are checked by independent expectations and
original/generated execution. Discarded MFHI/MFLO results still require
DSP enablement when they address a nonzero bank. Indexed
memory instructions use the full address registers, including the DSP-only
doubleword load `LDX`. Both C views preserve DSPControl under the n64 ABI.
An n64 source return requires the complete eight-byte incoming link, including
when it is restored from a leaf stack slot. A four-byte spill suffices on o32.
Standalone IR that writes the link and calls a direct target needs the image
to establish the callee mode on either ABI. Classic n64 also lifts all 101 GNU 2.47 packed .OB, .QH and .PW forms from the early DSP64 extension,
including octal-byte, quad-halfword and paired-word arithmetic, shifts, compares,
picks and packing. Revision 2.40 deprecated this extension. ELF advertises DSP
without identifying its historical revision. Concrete use of these encodings
therefore requires a processor implementing them. Twenty dot-product and selected-lane instructions update complete 128-bit
HI/LO accumulators. Q15/Q31 product saturation, carry/borrow, Q31/Q63 result
saturation and the selected bank's sticky overflow flag remain explicit IR
operations. Classic `DMADD`/`DMSUB` and `DMADDU`/`DMSUBU` add or subtract
the sum of two signed or unsigned 32×32-bit lane products in the selected
128-bit accumulator. Signed products retain their full 64-bit value and
sign-extend into the accumulator before addition or subtraction.
Carry and borrow propagate between LO and HI. DSPControl is preserved.
Classic `DEXTR.W/.L` and `DEXTRV.W/.L` extract
the low word or doubleword after shifting the complete 128-bit accumulator.
`DEXTR_R.W/.L` and `DEXTRV_R.W/.L` round the shifted value;
`DEXTR_RS.W/.L` and `DEXTRV_RS.W/.L` additionally saturate it to the signed
destination range. `DEXTR_S.H` and `DEXTRV_S.H` saturate a shifted halfword
without rounding. Overflow sets sticky DSPControl bit 23 even when the
destination is `$zero`. Immediate counts use five bits; variable halfword
counts use five and variable word/doubleword counts use six. The rounded
range check retains both accumulator halves, including a rounding carry
from LO into HI. Extraction preserves every accumulator bank.
`DEXTP[V]` and `DEXTPDP[V]` extract up to 64 bits ending at the seven-bit
POS, including fields crossing HI/LO and fields wholly within HI. A failed
extraction sets EFI, preserves POS and returns an unpredictable value.
Successful decrement forms update POS modulo 128. EFI and POS updates still
occur when the destination is `$zero`; discarded results also retain the
DSP-enabled check in the original executor. `DSHILO`/`DSHILOV` shift
the complete accumulator logically by a signed seven-bit count, including
the -64 endpoint. `DMTHLIP` moves LO to HI, writes the full GPR to LO and
advances POS by 64; a POS greater than 64 leaves POS unpredictable.
`DINSV` shares the INSV bitfield owner with 64-bit source/destination values
and seven-bit positions. Empty or out-of-range fields remain unpredictable.
The scalar-result contract preserves both 32- and 64-bit extraction values
through LowIR, LLVM and C. Retained unpredictable extraction results trap
in generated C to bound its source domain; this is not a guest exception.

APInt unit tests cover all counts, all 128 positions and four accumulator
banks in both byte orders. GNU-assembled source round trips separately execute
the original forms and both C views, including mixed-width extraction helpers.
The pinned Unicorn fixes legacy DEXTR's signed-LO shift, DEXTP's undefined
host shifts and the DSHILO -64 endpoint. Original-machine comparisons cover
negative LO, zero and full-width fields, fields wholly in HI, and the -64
shift through both C views. Its dedicated regression constructs expected
bits independently for every register count and position in all four banks
and both byte orders. The rounded-extraction and word-product additions use
APInt expectations and original instructions through the pinned reference.
The reference fixes include truncated signed products, rounded word overflow,
negative halfword saturation and the variable halfword count operand.
Their [qualification receipt](evidence/mips-dsp64-rounded-products-2026-11-17.json)
records the classic n64 slice; it does not establish execution on historical
physical CPUs or support for deprecated microMIPS64 encodings. In n64 DSPControl,
POS has seven bits and CCOND has eight. RDDSP zero-extends selected bits.
BPOSGE32 tests the complete POS against 32 before its delay slot executes.
MTHLIP adds 32 to an incoming POS of 0 through 32. Larger values leave only
POS unpredictable. These rules also apply to the admitted microMIPS forms.
See the register model and revision history in the
[MIPS64 DSP Module, revision 3.02](https://docs.alexrp.com/mips/mips64_dsp_v3_02.pdf)
and [microMIPS64 DSP Module, revision 3.02](https://s3-eu-west-1.amazonaws.com/downloads-mips/documents/MD00765-2B-microMIPS64DSP-AFP-03.02.pdf).

Classic AC0 partial writes preserve the pre-R6 unread-result restriction:
MULT/MULTU/DIV/DIVU and their doubleword forms establish pending
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

## microMIPS instruction and source boundaries

The loader accepts linked r2 and r6 microMIPS ELF for o32, n32 and n64 in either
byte order. It checks the ELF header against any available
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
Pre-R6 LUI, SEB, SEH and WSBH normalize to their classic word semantics,
including sign extension into 64-bit GPRs. Their literal physical encodings
are checked against GNU as and original execution in both byte orders.
microMIPS32r2 DSP/DSPR2 and microMIPS32r5 MSA use the same scalar/vector
semantics as their classic encodings. The physical decoder preserves the two
halfwords in target order, the instruction address, operand detail and size.
The MSA branch displacement counts halfwords. Optional instructions require
the corresponding ELF feature and local encoding evidence. Unlisted encodings
produce a lift error.

Pre-R6 microMIPS floating encodings share the classic COP1 semantic owner.
`MicroMIPSLegacyFloat.def` supplies the legacy-only arithmetic and paired-single
fields; shared S/D encodings remain in `MicroMIPSFloat.def`. The qualified slice
includes scalar arithmetic and conversions, COP1X multiply-add, all S/D/PS
predicates, conditional moves, PS packing/alignment, ordinary and indexed FP
memory, and FPR/control transfers. DMFC1/DMTC1 require 64-bit GPRs. Explicit FP
ABI evidence determines FR pairing. BC1F/T use halfword displacements and
capture FCC before a two- or four-byte slot changes it. Reusing a decoder across
R6 must distinguish legacy MOVF from R6 RINT even when the physical word is the
same. Reserved fields remain errors.

The tests compare 160 independently GNU-assembled classic/micro encoding pairs
and execute physical micro instructions in both byte orders and GPR widths,
including FR=0 on o32. The original and regenerated floating source leaves use
Clang for o32 and GCC for n64, with scalar results, FCSR and memory compared at
O0/O2. PS source tests use n64 integer carriers; they do not establish a vector
call ABI. The generic Unicorn CPU profiles are bounded execution references,
not complete physical processor models. Source-call qualification is described
separately below.

The [2026-11-15 physical microMIPS FP receipt](evidence/mips-micro-legacy-fp-2026-11-15.json)
records 160 independently assembled encoding forms, 406,272 arithmetic
comparisons, 448 memory cases and 512 branch/slot cases in both byte orders.
Its source matrix contains 1,128 builds and 52,808 rebuilt execution comparisons.
These checks qualify the listed instruction and source slices. The subsequent
compressed call matrix covers the ABI slice below; full microMIPS ISA coverage
remains separate work.

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
bounded integer and floating leaves, memory effects, o32 stack arguments and
branch/slot capture. The Release 2 floating call matrix additionally executes
microMIPS callers with microMIPS or classic callees: o32 FR=0/FR=1, n32 and n64,
in both byte orders. It covers fixed and variadic signatures, FP overflow onto
the stack, runtime indirect targets, and floating, narrow-integer, two-register
integer, pointer and void results. Both HighC and LLVMC are recompiled with GNU
MIPS GCC at O0/O2; original and rebuilt callers check result bits, FCSR, saved
FPRs, SP and the tagged return link. Direct interworking uses JALX; indirect
interworking uses the runtime target's mode tag.

The [2026-11-15 compressed call receipt](evidence/mips-compressed-call-abi-2026-11-15.json)
records 448 original ELF profiles and 8,960 original execution cases, followed
by 2,176 source builds and 43,520 rebuilt comparisons. Its reference pin fixes
microMIPS SWM word stores and 64-bit LWM sign extension, with an independent
regression that fails against the previous executor.

That microMIPS matrix does not qualify MIPS16 hard-float calls. The separate
MIPS16 matrix checks `float, uint32_t` body entries and typed direct calls
returning `uint32_t`, in FR=0/1 and both byte orders. HighC and LLVMC rebuild
each side against the original other side, and both functions together, with
Clang/GCC at O0/O2. The compiler starts in classic mode, so the generated
attributes must select MIPS16 and its GPR parameter transport.
The [2026-11-16 MIPS16 carrier receipt](evidence/mips16-floating-carriers-2026-11-16.json)
records 288 source builds and 36,864 rebuilt comparisons across this call matrix
and the leading/after-integer floating leaf matrices. Its 1,319 local owner tests
include four native-source tests that timed out in the parallel run and passed
unchanged on individual reruns. Eight classic-stub source refusals are counted
separately from successful reconstruction.
Classic linker entry stubs remain distinct physical functions and need a
verified prototype association; their names alone do not establish it.
[GCC's MIPS16 function-stub implementation](https://gnu.googlesource.com/gcc/+/refs/tags/releases/gcc-12.1.0/gcc/config/mips/mips.cc)
describes this FPR-to-GPR bridge. Indirect runtime stubs still require their
own result and control-flow qualification.

An o32 compressed caller may call a classic GNU runtime stub with the tagged
function pointer in v0. The source projection checks the instructions that
copy argument and result words, the indirect jump, its t9 delay-slot copy
and the saved link for a returning call. A call-site declaration supplies
the actual prototype. A variadic occurrence has no FPR argument prefix and
requires the promoted types of its unnamed arguments. The original LowIR
retains the direct call to the runtime stub. The
[indirect-call roundtrip](../unittests/lift/mips/mips16-indirect-call-roundtrip.py)
exercises both byte orders, FR=0/1 and classic/compressed targets.
It includes a variadic call with a named float and unsigned word, followed by
a promoted double in a2/a3 and an unsigned word on the stack. The callee
combines all four operands into the returned double's bits.

The [MIPS16 call roundtrip](../unittests/lift/mips/mips16-floating-call-roundtrip.py)
also covers split double parameters and proven classic entry stubs, in both
byte orders and FR=0/1. GCC originals are compared with GCC and Clang rebuilds
of either isolated side and of the combined caller/callee, through HighC and
LLVMC at O0/O2. The proof rejects altered target tags, unproven register
targets, stores in the delay slot, and missing or mismatched declarations.
Source definitions of compiler-generated stubs use a distinct identifier so
regenerated stubs cannot collide; original addresses and names remain in
metadata. GNU rebuilds use fixed-width rotate helpers with modulo counts and
single evaluation. Tests and paired performance receipts are retained in the
task's ignored build directory; the entry adapter does not establish a
universal speed ranking or qualify other firmware.

Clang 22 misses an entry stub when only a function attribute enables MIPS16
inside a classic compilation unit; its FR=1 double stubs also use the wrong
high-word register. Independent original-code executions distinguish these
compiler failures from source reconstruction. The corrected compiler in
[NeverSight LLVM PR #12](https://github.com/NeverSight/llvm-project/pull/12)
passed the float/double reconstruction matrix and three-host fork CI and has
been merged. The installed MIPS backend is also qualified by
[LLVM PR #13](https://github.com/NeverSight/llvm-project/pull/13); consuming a
new NeverD dependency snapshot requires its own build qualification.

MIPS16 o32 scalar `float` and `double` results now admit byte-proven classic
return helpers. The proof checks the decoded operations, physical delay slot,
unchanged incoming link and exact GPR-to-FPR words in both byte orders and
FR=0/1. Names do not confer a contract. HighC and LLVMC project the helper's
effects into a private source body before SSA; published LowIR and original
call targets remain intact. Machine lift does not inherit this source-only
qualification. Proven classic parameter adapters can also accompany a scalar
floating result.

Complete source units omit these result helpers with an explicit function
audit and retain the native runtime when linking the rebuilt program. A
standalone source request for a helper fails clearly: its incoming result
registers do not form an ordinary C function signature. This prevents an empty
helper definition from replacing the actual register transport. Selected source
units honor audited omissions; machine lift still requires every selected body.

The [scalar runtime receipt](evidence/mips16-scalar-runtime-2026-11-17.json)
records separate pointer-input and floating-input matrices against unmodified
GCC 12.4 runtime helpers. Each matrix has 16 profiles, 384 source rebuilds and
12,288 independent execution comparisons, including NaN payloads, asymmetric
double words and preserved machine state. The return-link proof follows
numerical result spills and external scalar input loads before any possible
frame escape, including predecessor blocks. Passing a frame-derived argument,
or reading after an external store or call without a stronger memory contract,
does not acquire that proof. Variadic/runtime conversion adapters and MIPS64
compressed floating results remain unqualified.

Type declarations now retain `float _Complex` and `double _Complex` through
C parsing/rendering, DWARF import and source prototype conversion. They keep
their complex passing class rather than becoming a same-sized integer or
ordinary two-field record. Fixed o32, n32 and n64 calls with scalar parameters
can return these two-component complex values. The source ABI uses f0/f2 and preserves
the MIPS16 v0/v1 and a0/a1 word aliases in target byte order. A component that
already occupies its full ABI register is not zero-extended into the same
width. HighC and LLVMC construct and extract components as raw floating values,
including NaN payloads and signed zero, and evaluate each call once.

The return-link proof also handles GCC's double arguments assembled by two
word stores before `ldc1`. It follows every requested byte in target byte
order on every reaching predecessor; missing bytes, frame-derived values and
overwrites of the saved link still reject source reconstruction. A later
floating result spill can depend on the argument currently being proved.
Only completed positive provenance proofs are cached, so a recursive query's
temporary failure cannot invalidate an otherwise proven call.

The [complex result receipt](evidence/mips16-complex-runtime-2026-11-17.json)
records separate caller/callee objects and
complete source units with GCC and Clang at O0/O2, for both byte orders,
FR=0/1 and classic/MIPS16 callers. The original uses the unmodified GNU
runtime; rebuilt freestanding programs also link the tested guest `memcpy`
required by compiler-generated aggregate copies. No emulator-side memory
hook replaces that code. This qualifies complex results with scalar parameters,
not complex parameters, variadic complex calls or general
indirect runtime adapters. Extended complex types remain opaque when DWARF
describes them and are refused as source declarations.

The n32/n64 complex-result checks have their own independent compiler
fixtures in `unittests/lift/mips/n32-n64-complex-roundtrip.py`. They bind
classic MIPS64r2 hard-float declarations with scalar parameters and compare
both C routes with separately compiled GCC/Clang objects, including direct
and indirect calls with an explicit call-site ABI. A callback parameter's type
alone does not establish the contract of an arbitrary indirect instruction.
n32 retains four-byte pointers and eight-byte argument
positions; the result's real and imaginary components occupy f0 and f2 in
both byte orders. This does not qualify compressed n32/n64 complex runtime
adapters, complex parameters, variadic complex calls or general record ABIs.

The return-link proof permits saved FP/SP values only in stack storage that
remains private at every call. The boundary includes each callee's entire
outgoing argument area, including unused bytes of a partial argument slot.
Pointer spills need a bounded argument extent; an unknown or open variadic
call's minimum ABI area does not establish that bound. External stores,
passed or reloaded frame addresses, an unknown call SP, and a second call
that exposes an earlier spill remain unproved. Logical OR/XOR
with architectural zero retains its full-width copy identity before this
proof; word ADDU still has its distinct truncation and extension semantics.

Two additional complete-unit matrices rebuild the HighC call closure and LLVMC
image, linking only the resulting translation unit and the original runtime.
Each covers 16 profiles, 128 builds and 4,096 executions with the same byte
orders, FR modes, compilers and observations. The unit and isolated-function
receipts identify their different tested commits explicitly.

Numeric microMIPS code identities are admitted for defined r2 targets with
explicit ELF mode evidence in o32, n32 and n64. MIPS16 numerical identities
remain limited to r2/o32. Both source backends share this boundary; unknown-mode
imports, including whole or partial GOT reads, remain refused. Direct control
identities, serialized callables and code-byte/data addresses retain distinct
roles. The LLVM r7 SDK uses the fork's `Fi8` data layout, so stronger function
code alignment cannot clear the callable ISA tag during LLVM optimization.
Recompiling numeric observations requires NeverSight Clang with the qualified
[MIPS function-pointer layout fix](https://github.com/NeverSight/llvm-project/commit/df115daf54a117501223f3a6a88bfcd9bf4fa5a8):
ordinary Clang 22 can fold the low bit to zero despite a mode attribute.
The runtime matrix relocates rebuilt callees, then checks symbol-relative
integer values, masks, offsets, equality and actual indirect calls.
The n32/n64 matrix additionally observes the complete 64-bit result of direct
and indirect calls to separate classic and microMIPS targets. N32 upper-half
addresses retain their canonical register representation; n64 cases execute
above 4 GiB. External microMIPS references do not require the compiler to emit
a microMIPS body. Reconstructed bodies still require compiler support for
that encoding.

When HighC recovers a 32-bit integer declaration for an n32/n64 register
parameter, a full 64-bit GPR read restores its ABI sign extension even for
`uint32_t`. Narrow reads keep their logical width, so explicit zero extension
remains distinct. The n32 integer view of a pointer parameter likewise keeps
the canonical sign extension. These rules apply to known incoming argument
registers; they do not assign values to stack padding or unknown live-ins.

Linked ELF instruction fields can retain function identity through
`R_MIPS_HI16/LO16/HIGHER/HIGHEST` records. The loader checks the symbol,
instruction mode, bytes and addend; ELF32 implicit HI/LO pairs follow relocation
table order, including a LO instruction scheduled before HI in different
registers. Shared source projection applies each field's carry, shift and
truncation to the rebuilt symbol address while keeping physical LowIR unchanged.
Address arithmetic and bit projection retain that symbol's identity.
This currently covers classic LUI, ADDIU, DADDIU and ORI fields naming defined
function symbols. It does not cover compressed instruction relocation encodings
or infer an address from an equal numeric literal. Without relocation or other
authenticated use evidence, the original integer bits remain scalar. Conflicting,
mismatched or unpaired claimed fields refuse source reconstruction.

The microMIPS operation and slot rules were checked against
[microMIPS32 Volume II, revision 5.04](https://s3-eu-west-1.amazonaws.com/downloads-mips/documents/MD00582-2B-microMIPS32-AFP-05.04.pdf)
and the pinned [GNU simulator definitions](https://gnu.googlesource.com/binutils-gdb/+/d01e823438c7dc264d6885fbbfeace4d8955dcb7/sim/mips/micromips.igen).
On n64, tagged callable addresses, computed jumps, link registers and memory
addresses retain all 64 bits. The r2 scalar subset adds doubleword arithmetic,
shifts, rotates, bit fields, byte shuffles, HI/LO division, LD/SD, LWU,
LDL/LDR and SDL/SDR.
The micro doubleword merge transfers use a signed 12-bit displacement.
For micro functions, HighC and LLVMC select GCC's `micromips` attribute or
Clang's `micromips,nomips16` pair. A call-site prototype also retains the
original direct target's known instruction mode on its external declaration;
omitting that body does not turn a compressed callee into a classic one.
GCC requires `-minterlink-compressed` for
calls between compressed functions and classic helpers in the same output.
The MIPS source prelude supplies a single-evaluation, size-checked raw bit copy
for GCC C, whose frontend lacks Clang's `__builtin_bit_cast`. This preserves
pointer/integer and floating bit patterns without aliasing through another type.
Classic functions in mixed-mode outputs use the same compatibility definition.
LLVMC emits it only when the selected bodies or record definitions use the
bit-copy builtin, so unrelated floating functions cannot enlarge an integer
function's cached source text.
Both emitters retain native `__int128` for n32: its pointers are 32 bits, but
the GNU-compatible compiler type follows its 64-bit register architecture.
Targets lacking that type continue to use the existing `_BitInt(128)` mapping.
Compilation stops if Clang reports no microMIPS attribute for the selected
target. GCC's MIPS attributes are
documented in the [GCC manual](https://gcc.gnu.org/onlinedocs/gcc/MIPS-Attributes.html).
Both r2 widths also lift LWL/LWR and SWL/SWR with a signed 12-bit
displacement. The source fixtures compare the returned word and surrounding
bytes for aligned and unaligned pairs. Word DSP and DSPR2 instructions use
the same revision 3.02 accumulator rules as classic n64. GNU as encodes the
113 DSP and 46 DSPR2 forms in micro mode. Indexed doubleword LDX has a qualified classic encoding.

microMIPS32r6 compact transfers use halfword displacements, the physical
length of the instruction and tagged return links. A second branch may
immediately follow a compact branch. JIC preserves the link register and
JIALC writes it. Both scalar micro revisions lift NOT16, AND16, OR16,
XOR16, the ANDI16 mask table and the four compact add-immediate forms.
MOVEP copies both architectural registers. LWM16/SWM16 transfer saved
registers followed by RA in address order. A later memory fault preserves
earlier transfers. JRADDIUSP/JRCADDIUSP return to the tagged RA and add the
encoded word offset to SP. The pre-R6 profile also reads HI/LO through MFHI16
and MFLO16. The decoder and lifter also cover the R6 compact three-register
word arithmetic, PC-relative operations, scalar and paired word reservations,
selected RDHWR, CP0 word/high transfers, DI/EI, CACHE, PREF, EHB and PAUSE.
The physical decoder corrects the register fields for LSA/ALIGN and signed
nine-bit offsets for CACHE/PREF. The encoding source is the
[microMIPS32 Instruction Set Reference Manual, revision 6.05](https://docs.alexrp.com/mips/micromips32_insns_v6_05.pdf).
The compact register lists and pre-R6 merge fields were also checked against
[Ghidra’s microMIPS SLEIGH definitions](https://github.com/NationalSecurityAgency/ghidra/blob/42db038f3c88c6bd7e9bedbb919138111f8b77fd/Ghidra/Processors/MIPS/data/languages/mipsmicro.sinc).

The n64 Release 6 profile adds the doubleword multiply, high multiply, divide,
remainder, count, shuffle and align encodings, DAHI/DATI/DAUI, DMFC0/DMTC0,
LDPC and scalar/paired doubleword reservations. Shared integer, shift and
load/store forms preserve 64-bit operands. Tests compare the compact words
with GNU-assembled classic encodings at a PC above 4 GiB in both byte orders.
The diagrams for DLSA and DSRA32 disagree with the opcode map. One candidate
for each overlaps DINSM. NeverD rejects those ambiguous words in the R6 profile.
See the [microMIPS64 Instruction Set Reference Manual, revision 6.05](https://docs.alexrp.com/mips/micromips64_insns_v6_05.pdf).

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
r2/o32 code identities use the toolchain contract described above. Source tests
use Clang assembly
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
Classic Release 6 reservations and CP0 transfers are described below.

`NdOpEmulator::setMIPSReservationModel` explicitly selects a flat,
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
preserve target byte order within each element. Each LowIR memory operation
accesses a complete element. Both C views use byte-aligned scalar aliases
for ordinary RAM in an MSA image so the target compiler can generate legal
loads and stores at odd addresses. LDI, MOVE.V, scalar element
moves and the immediate mask operations share the vector register bank.
BZ/BNZ test their predicate before the delay slot. The element forms test
for any zero or all nonzero lanes, while .V tests the whole register.

The o32 MSA profile requires classic or microMIPS32 Release 5 ELF in either byte
order, with `.MIPS.abiflags` explicitly declaring MSA, 128-bit CPR1 registers
and FP64 ABI tag 6. A reused decoder consumes the current revision as well as
feature evidence; architecture-only initialization remains Release 2 without
MSA. The n64 profile is described below. Other revisions and DSP/MSA
combinations are refused.

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
Release 6 scalar floating writes define the low FPR view and leave the
remaining W bits unpredictable. A single-word result also leaves the FPR
high word unpredictable. Reads of defined low bytes retain their values.

LowIR accepts all 32 W registers. Source qualification is narrower and runs
before SSA/dead-code elimination. Each vector input must be initialized on every incoming leaf path. Calls and writes to the saved
F20/F22/.../F30 low views require a separate ABI proof. Without an Image, all
W20..W31 writes are refused because other o32 FPABIs preserve additional views.
The n64 profile preserves F24 through F31. Returns must retain the
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
delivery and scalar FP call ABI recovery remain outside this profile.
Classic and microMIPS64r5 use n64 hard double (FP ABI 1, CPR1 size 128).
Both support the full-width memory base, COPY_S.D, COPY_U.W, INSERT.D, FILL.D
and DLSA. COPY_U.D is reserved. The concrete evaluator uses I6400 for n64
and P5600 for o32. Scalar Release 6 FPR transfers, conditional selects, bitwise
moves and CLASS use the shared low FPR views. The operation and alias rules follow the
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

A complete debug record layout does not by itself establish its MIPS return
carriers. Until a source entry contract binds that return, HighC keeps the
recovered result representation instead of changing between integer and
floating register banks based only on the debug type.

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

Use `roundtrip.py --encoding micro` for the seven microMIPS assembly leaves in
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

### Classic DSPR2, MSA and compressed system validation

The [DSPR2/MSA execution and performance receipt](evidence/mips-dspr2-msa-2026-11-11.json)
records both byte orders. LLVM assembled 113 classic DSP forms, all 46 DSPR2
forms and 526 MIPS32 MSA mnemonic/width combinations. NeverD lifted both
copies of each stream with matching LowIR fingerprints. The 74Kf and P5600
execution suites compared 1,552 HighC/LLVMC builds with the original ELF
across 18,080 inputs, including unaligned vector loads/stores at odd addresses.
The MIPS16e2 fixture independently assembled the board CP0 and RDHWR transfers.
Its generated MIPS16 code passed 16,640 executions against the ISA expectations.

The receipt includes paired CPU-time samples against the previous NeverD
baseline and Ghidra SLEIGH, with separate Capstone physical decode timings.
The comparison uses classic MIPS32r2/o32 DSP, MIPS32r5/o32 MSA, and compressed
MIPS16e2 r2/o32. DSP64, compressed DSP/MSA and the Release 6 DSP compact
branch BPOSGE32C have separate ISA and ABI requirements.

## Classic Release 6 instructions

Linked MIPS32r6/o32 and MIPS64r6/n64 ELF select a revision-specific integer
decoder. It covers three-register multiply, multiply-high, divide and modulo,
conditional selects, count-leading operations, bit/byte alignment, scaled and
upper-immediate arithmetic, PC-relative address generation and loads, and
compact conditional, unconditional and indirect transfers. Word and doubleword
results retain their specified GPR width. A zero divisor produces an explicit
unpredictable result. Compact links save PC+4, use the physical immediate
scale and do not consume a delay slot. A control transfer in a conditional
compact branch's forbidden slot faults on fallthrough. Lifting a conditional compact
branch requires the following instruction's bytes to check that constraint.
Surviving legacy delayed transfers retain their slot. NAL writes PC+8 before
executing its delay slot. Removed HI/LO and branch-likely encodings
cannot use the Release 2 lifter through this profile.

Release 6 also lifts signed nine-bit-offset `LL/SC` and `LLD/SCD`, and
offset-free `LLWP/SCWP` and `LLDP/SCDP`. Doubleword elements require n64.
The first GPR in a pair carries the least significant element in either byte
order. The emulator checks the full naturally aligned memory span, including
write permission on a failed conditional store. Its flat-memory reservation
model takes the target coherence granule. The two-argument overload with
`PairedTransfers=true` also binds Config5.XNP=0. An unresolved reservation or
an inconsistent same-address pairing stops evaluation. A failed comparison
against a different mapped address returns zero and leaves memory untouched.

`MFC0/MTC0`, `MFHC0/MTHC0`, n64 `DMFC0/DMTC0`, `RDHWR`, `DI/EI`,
`EHB`, `PAUSE`, `CACHE` and `PREF` retain their machine-state dependencies
in LowIR. A board supplies implemented CP0 views, including MVH support for
high-word transfers. Selected performance registers use the encoded RDHWR
selector. A cache request includes the ISA revision and full virtual address.
Source generation currently declines live reservation and board-state effects.
General privileged execution and optional Release 6 ASEs require
additional coverage. The [testing guide](testing.md)
covers the linked GNU fixtures. The encoding and execution rules follow the
[MIPS64 Instruction Set Reference Manual, revision 6.06](https://docs.alexrp.com/mips/mips64_insns_v6_06.pdf).

The [MIPS64 DSP, microMIPS ASE and Release 6 qualification](evidence/mips64-micro-r6-2026-11-11.json)
records the linked big- and little-endian corpora, per-form comparisons and
independent execution checks. Both C views were recompiled at `-O0` and
`-O2`, including the Archer C7 V5 `ubus call` handler.

The [microMIPS64 and compact R6 measurements](evidence/mips-micro64-compact-r6-2026-11-12.json)
record 779 passing tests and 636 executions of physical n64 DSP/DSPR2
instructions in GNU sim. The source tests include 144 builds of the profile
kernels and compare the Archer C7 V5 handler on 12 original and 48 rebuilt
cases. On an Apple M4 Max, the four o32 compressed instruction corpora with
matching instruction boundaries reached 5.15–5.42 million instructions per CPU
second in NeverD and 1.08–1.15 million in SLEIGH. Against the previous NeverD
build on the full microMIPS corpus, the median paired big-endian CPU time
increased by 4.23% for decode and 2.13% for lift. Individual paired timings
varied in both directions. The receipt includes little-endian results, input
hashes and the excluded SLEIGH delay-slot case.

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


### Historical MIPS I–III scheduling

Linked classic MIPS I/II o32 and MIPS III o32/n32/n64 ELF profiles are admitted
in both byte orders. The shared physical-word catalog rejects instructions
introduced in later ISAs, release-only aliases and conflicting ABI evidence.
MIPS I/II floating profiles require paired 32-bit FPRs. MIPS III o32 ELF32
uses 64-bit GPRs with four-byte pointers and argument words, four argument
registers, and a 16-byte caller home area. GNU and Clang producers can declare
a 32-bit or 64-bit GPR size in the ABI record. Both use the historical ISA
from the ELF flags. Hard-double o32 uses paired FPRs when CPR1 is 32-bit.
Concrete o32 doubleword GPR operations require the caller to bind
`HistoricalO32DoublewordEnabled` for the selected processor mode.

Each admitted I–III instruction carries ordered begin/retire operations in
LowIR and MedIR. A branch and its executed delay slot retire separately; a
branch-likely annulled slot does not age the schedule. Physical register reads
survive lowering even when their computed result is discarded. Call-site
argument inference does not introduce physical dependencies.

Execution requires a caller-bound `MIPSSystemContext::Historical` snapshot.
The same state can continue across basic blocks, calls and distinct emulator
instances; a function entry does not imply an empty pipeline. The schedule
checks MIPS I load delays, I–III coprocessor move and condition dependencies,
FR=0 register pairs, and the two-instruction HI/LO read-to-write restriction.
LWL/LWR may forward the preceding memory-load destination into their merge
operand, while the base register remains an ordinary read. The first MTHI or
MTLO after an unread product makes the companion accumulator unavailable.
Failed attempts do not retire or consume a delay period.

Unbound execution, including lenient constant recovery, refuses these ordered
operations. HighC and LLVM source reconstruction also refuse to erase the
scheduling contract. Floating arithmetic additionally requires an executor
that explicitly declares the historical ISA; the modern pre-R6 FPU does not
claim R3000/R4000 floating-point exception, NaN or subnormal behavior.

A historical scheduling snapshot can additionally bind a processor's CP0 state.
The R30xx and R4000 have distinct exception rules. A MIPS II CW4011 snapshot
selects its R3000- or R4000-compatible exception mode. Historical CP0 moves use
selector zero. The bound state supplies Status, Cause, EPC and an observed bad
address. Cause writes change only the software interrupt bits. Accepted Status
writes track interrupt visibility and coprocessor usability separately.
Register updates and hazard aging occur on successful physical retirement.

SYSCALL and BREAK enter the common exception handler, including inside delay
slots. The branch occurrence and its outcome accompany an executed slot.
R30xx shifts KU/IE into the mode stack. R4000 retains the original EPC and BD
when EXL is already set. The CW4011 also records the branch outcome in BT.
RFE restores the KU/IE stack. ERET selects EPC or the observed ErrorEPC and
clears the load-linked reservation. Signed ADD/SUB and immediate forms
deliver overflow before writeback. Aligned scalar integer loads and stores
report address errors before accessing memory. A disabled coprocessor
sets the CE field. Interrupts sample the observed IP and IM bits at issue
after the corresponding write hazard. The platform can update its six
hardware interrupt lines between physical instructions. Reset, TLB state
and board-specific Status fields require additional platform evidence. The CW4011 conventions follow the
[LSI L64364 ATMizer II manual, chapter 4](https://www.ic72.com/pdf_file/l/123798.pdf).

The timing rules are based on the
[R3000 family manual](https://cgi.cse.unsw.edu.au/~cs3231/doc/R3000.pdf)
and [R4000 manual](https://www.cs.columbia.edu/~youngjin/download/MIPSR4000.pdf).
Independent instruction replay qualifies valid integer values, not the timing
behavior of a historical physical processor.

### Historical MIPS IV and V

Linked classic n32/n64 ELF files can select MIPS IV or MIPS V in either byte
order. `e_flags` and `.MIPS.abiflags` must agree about the historical architecture
level; this level is separate from the MIPS32/MIPS64 release number. Historical
profiles reject modern ASE declarations, compressed mode and NaN2008 evidence.
The shared availability catalog gates physical words before decoder fallbacks
and at direct lift entry, including after decoder reuse. It does not imply that
every architecturally available instruction has a semantic implementation.
Trapping arithmetic, privileged operations and reservations retain the limits
listed above. MIPS IV–V o32 remains unqualified; I–III scheduling has
the explicit execution and reconstruction boundaries described above.

IV admits the implemented integer and scalar COP1/COP1X slice; V additionally
admits paired-single and LUXC1/SUXC1. ROTR, EXT, MFHC1 and other release-only
forms cannot enter through a Capstone alias. The historical SLL-to-zero words
later used for EHB and PAUSE keep their original shift semantics. IV condition
branches and conditional moves read the conditions from FCSR; V also admits
FCCR/FEXR/FENR. Floating execution uses the existing pre-R6 computational
contract after historical instruction admission, including legacy NaNs,
rounding, sticky flags and enabled exceptions. It does not substitute Release 6
floating behavior or establish a historical physical-CPU timing model.
The paired-single decoder supplies missing pack, unpack, shuffle and alignment
operand records while retaining the physical instruction for lifting.

`profile-roundtrip.py --historical-isa 4|5 --historical-abi n32|n64` assembles
original historical ELF profiles, then executes their HighC and LLVMC outputs
at O0/O2. Clang 22 lacks a MIPS V code generator: the V source check uses its
MIPS IV scalar code generator and the semantic helpers' original PS words.
GNU as assembles that unedited output for V, so both original and rebuilt ELF
headers and ABI records retain V. The receipt records source, code-generation
and rebuilt ISA separately.
The original-instruction reference is the pinned compatible MIPS64r2 FPU;
physical MIPS IV/V hardware is not qualified. The availability rules follow
[MIPS IV ISA revision 3.2](https://www.infania.net/misc1/sgi_techpubs/techpubs/007-2597-001.pdf)
and the historical notes in
[MIPS64 Volume II revision 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf).

The [2026-11-15 historical qualification receipt](evidence/mips-historical-2026-11-15.json)
records 940 original ELF profiles, 3,760 source builds and 163,440 executions
across both ABIs, byte orders, C views and optimization levels. The LowIR
arithmetic matrix adds 264,576 original-instruction comparisons. These results
qualify the stated slice, not full historical ISA coverage.

### Classic Release 2 scalar FPU

The scalar FP owner admits classic pre-R6 COP1/COP1X with explicit register-mode
evidence: o32 FP64 (GNU FP ABI 6) or n32/n64 hard double (ABI 1) establishes
FR=1. o32 hard double (ABI 1) with 32-bit CPR1 evidence establishes FR=0.
Conflicting CPR1 widths, EF_MIPS_FP64 with FR=0, and FR=0 under Release 6 are
rejected. FPXX and integer-only images do not establish an FPR mode. Both GPR
widths and byte orders share the instruction owner. Historical MIPS I–III
profiles remain outside this profile. Pre-R6 microMIPS uses this semantic owner
with the separate physical encoding and source qualification described above.

FR=0 LowIR retains 32 independent physical FPR words. Doubles join the even
register's low word with its odd partner's high word in either byte order;
a single write preserves the partner. Double operations and memory accesses
reject odd register operands, and long fixed-point, paired-single and
LUXC1/SUXC1 encodings are refused. Source projection combines physical words
into even-register ABI carriers before SSA. Unknown incoming scratch words
remain explicit until the byte-use proof establishes that they are unobserved.
Neither this projection nor parameter inference initializes machine registers.

FR=1 paired-single supports ADD/SUB/MUL, ABS/NEG, all 16 paired comparisons,
and COP1X MADD/MSUB/NMADD/NMSUB. Both lane exceptions are combined before
writeback. Comparisons require an even condition selector; MOVF/T.PS selects
the two lanes independently. MOV/MOVZ/MOVN, CVT.PS.S, CVT.S.PL/PU,
PLL/PLU/PUL/PUU and ALNV.PS preserve their specified bit patterns. ALNV
requires a low-three-bit offset of zero or four for a defined result; other
offsets produce explicit unknown bits. Decoder adapters retain the original
bytes and fill Capstone's missing PS forms using the shared catalogs.
Native PS source round trips currently use n64 integer argument/result carriers.
Native MIPS32 PS execution and PS vector calling conventions are not qualified.

`MIPSLegacyFloat.def` extends the common scalar operation catalog with legacy
ABS/NEG, the 16 S/D predicates and COP1X MADD/MSUB/NMADD/NMSUB. Together with
the shared arithmetic and conversion forms there are 109 operations. Unlike
R6 ABS/NEG, the legacy forms can raise Invalid. Comparisons write their selected
FCC0–FCC7, preserve the other condition bits and retain FCSR exception effects.
The bound scalar executor must match the ISA revision; the Unicorn adapter uses
MIPS64R2_GENERIC for these operations, including o32. This binding supplies an
FPU, while LowIR owns GPR width and FR=0 pairing; 74Kf does not implement PS. A pending enabled exception
commits FCSR and suppresses destination writeback. This is an explicit reference
CPU policy, not an assertion that all implementations produce identical NaNs
or reciprocal estimates.

Register/control transfers, MOV.S/D, MOVF/T in GPRs and FPRs, and MOVZ/N.S/D
have explicit rules. An untaken FP move preserves the full destination;
a taken word write leaves its high word unpredictable. BC1F/T and their likely
forms snapshot the selected FCC before the slot, including a slot that writes
FCCR. A false likely branch annuls that write.

LWC1/LDC1/SWC1/SDC1 and LWXC1/LDXC1/SWXC1/SDXC1 check natural alignment before
memory or register effects. LUXC1/SUXC1 instead clear the low three effective
address bits, as specified for FR=1. The existing source-domain checks for
alignment and initialized memory continue to apply.

HighC and LLVMC preserve the arithmetic and control operations with native
target assembly and reuse the scalar ABI and byte-provenance checks described
below. No host floating-point evaluation substitutes for the target's FCSR.
The pinned Unicorn includes [the scalar legacy ABS/NEG exception fix](https://github.com/NeverSight/unicorn/pull/28)
and [the paired-single sign/exception fix](https://github.com/NeverSight/unicorn/pull/29).
Its [MSVC target configuration fix](https://github.com/NeverSight/unicorn/pull/34)
also restores the natural-alignment checks already enabled by Unix builds.
The [native Windows receipt](evidence/mips-msvc-reference-alignment-2026-11-15.json)
records the failing previous reference, three passing corrected runs and the
R6 unaligned-memory control.
The independent sign tests distinguish pre-R6 arithmetic NaN behavior from R6
bit operations, including enabled Invalid exceptions and preserved destinations.
The rules follow [MIPS64 Volume II, revision 2.50](https://0x04.net/~mwk/doc/mips/MD00087-2B-MIPS64BIS-AFP-02.50.pdf).
The [qualification receipt](evidence/mips-classic-fp-2026-11-14.json) records
the bit/exception comparisons, both source projections and the o32/n32/n64
fixed, variadic and indirect call roundtrips in both byte orders.
The [FR=0 and paired-single receipt](evidence/mips-fr0-paired-2026-11-15.json)
adds physical register-pair observations, PS lane/exception checks and FR=0
source ABI roundtrips. It records the historical ISA and legacy DSP64 gaps
separately from the qualified profiles.

### Release 6 scalar FPU

Classic and microMIPS scalar FP use a common COP1 semantic owner in both GPR
widths and byte orders. The 98 arithmetic, conversion and comparison forms
include fused MADDF/MSUBF and all 22 implemented mask predicates in each FP
width. Classification, selectors, register transfers and FP memory accesses
also have explicit lift rules. Classic BC1EQZ/BC1NEZ retain their delay slots.
The microMIPS BC1EQZC/BC1NEZC forms have halfword-scaled compact targets.

The ordered arithmetic intrinsics carry numeric FPR bit patterns and preserve
FCSR dependencies. A caller-bound Release 6 scalar FPU executor supplies
implementation-specific results. The optional Unicorn adapter selects
MIPS32R6_GENERIC or I6400, restores the supplied FCSR, and executes the original
operation. An enabled exception commits Cause state and suppresses the FPR
destination. FIR/FCSR and the FCCR/FEXR/FENR views are explicit control effects.
An unavailable implementation or unpredictable control write stops evaluation.

HighC and LLVMC retain native COP1 operations through volatile assembly,
including ambient FCSR and enabled exceptions. Their source qualification
requires initialized FPR bytes on every incoming path and an explicit FR=1 ELF
profile. Scalar float/double parameters and results follow o32 FP64 or
n32/n64 hard float. The o32 fixed FP prefix uses f12/f14 and consumes aligned
integer word positions. N32 and n64 share argument positions between a0–a7 and
f12–f19. Floating overflow values start at the lowest addressed bytes of their
stack slots in either byte order.

Complete declarations bind integer, pointer and floating operands before SSA.
This includes scalar-only signatures and `void(void)` in o32, n32 and n64.
For local callees, including private functions, an exact-entry declaration from
the loaded image's authenticated debug reader supplies the same contract as a
fixed C prototype.
Names-only information, unknown return types, variadic declarations and custom
parameter locations do not establish this fixed call contract.
Integer/pointer/void signatures do not require a floating-point profile. An
unspecified, soft-float or FPXX profile still does not authorize floating
arguments, floating results or FPR accesses. Call descriptors are checked in
either case.
For a variadic call, supply the prototype and the promoted types of the
unnamed arguments at its original call occurrence. O32 passes even named
floating arguments of a variadic call in the integer argument slots. N32 and
n64 retain FPRs for named FP arguments and use integer slots for unnamed
arguments. An indirect call keeps the runtime target operand.

A floating result uses f0. Integer results use v0, or v0/v1 in target word
order for an o32 64-bit result or an n32/n64 128-bit result. Narrow integers
retain their declared signedness. Both signed and unsigned 32-bit results are
sign-extended in n32/n64 registers. A pointer uses its ABI width, and a void
call defines no result carrier. Direct and indirect tail transfers preserve
the complete integer result register; n32's four-byte pointer width does not
narrow its eight-byte result carrier. Direct tail calls also retain the full
machine address width, so an explicit source prototype binds the same call
occurrence as an ordinary MIPS64 call. The ABI preserves the low eight bytes of even
F20–F30 on o32 and n32, or F24–F31 on n64. Unqualified FP profiles, unknown
call types, aggregate results and general saved-FPR restoration require
additional evidence. The register assignments follow the
[SGI n32/n64 calling convention](https://techpubs.jurassic.nl/library/manuals/2000/007-2816-005/sgi_html/ch02.html).

Natural C record transport for o32, n32 and n64 is classified in
`neverd/ir/MIPS/SourceRecordABI.h`. Each piece records its offset in the object,
its register or stack slot, and its position in the carrier. On o32 every
record result uses caller-provided storage and the callee returns its address
in v0. Large n32/n64 results use caller-provided storage and leave v0
unspecified. A typed call passes the original result address as LLVM `sret`.
The saved-link check accounts for every byte that the result writes into a
private frame, including the last byte of a short final word. An o32 double or long long
alongside a record uses an aligned pair of physical words. Both C backends
recombine the words in target order for the single typed argument. An o32
record can also split a field across argument registers and stack words. Its
LLVM call includes the unused alignment word when the flattened record needs
eight-byte alignment after an odd number of slots. An n32 record word retains
all 64 data bits when the source pointer width is 32. LLVMC handles the
nonstandard integer funnel shifts that LLVM creates when packing byte fields.
A by-value parameter keeps every physical operand live through C emission.
An indirect call may use its caller’s frame for a hidden result when its
address derives from the authenticated incoming stack register.
Both C emitters can build
a typed entry around a recovered body
when the machine inputs match the declared layout. The return can occupy
one register, a pair of GPRs or FPRs, or the caller's storage. A short final
word uses only the object's remaining bytes, including on big-endian targets.
Direct calls and explicitly typed indirect calls retain the register bank and
byte offset of each independently placed field. A result written in the
return delay slot is captured before SSA loses its physical register name. Reconstructing a source type requires an
explicit local declaration or matching debug information and a complete
by-value field layout. Unions and incomplete field graphs retain their display
types.

A nonleaf's saved link is accepted only when exact reaching stores preserve
the original RA and mode, its slot lies outside the callee's complete outgoing
argument area, and no argument or other store exposes the private frame.
An untyped PIC call can observe argument-register writes that reach it,
including its delay slot and loop backedges. A later call with a declared
by-value record observes the pieces selected by its own signature. For example,
a single unsigned field that fills an ABI slot occupies one argument GPR,
including a four-byte slot on o32. A four-byte integer field in an eight-byte
n32 or n64 register uses its high half on big-endian machines and its low half
on little-endian machines when passed or returned as a record. A caller may
sign-extend that field after the call. The saved-link analysis follows the
field's sign byte and leaves the unused half unproven, including after an
unaligned word load with an implementation-dependent upper word.
Saving the incoming frame pointer or another frame address does not expose
the frame when every byte of that spill remains private at every call.
A merge load from caller-owned input can supply a record argument when both
the memory bytes and the retained register bytes exclude the private frame.
For a merge store into a private stack slot the analysis follows each written
byte in target order. A write covering even one saved-link byte invalidates
that byte's previous value. A merge store to an output pointer requires
evidence that the stored bytes cannot expose the private frame.
For record callers that copy an argument into a private stack buffer in a
counted loop, the proof follows the loop's actual byte addresses and bound.
A call with an unknown frame effect, an uninitialized private byte or an
overlapping write to the saved link prevents this reconstruction.
The shared call-stack proof computes that intersection using complete outgoing
argument slots. An unknown call SP, an external store or a frame-derived call
argument cannot obtain this exception.
Byte provenance ignores unread upper FPR bytes; stack arguments require their
own reaching-store proof. A validated alignment guard preserves computed stack
addresses and reaching stores; malformed guards and opaque effects invalidate
that evidence. This also qualifies the aligned pre-R6 floating stores used for
outgoing overflow and variadic arguments.
In n32, an eight-byte saved link can survive a declared call beside a four-byte
pointer spill, including word-sized or doubleword-sized stack adjustments.
Unknown call-stack state and writes overlapping that link slot still fail
qualification.
Function-scoped instruction mode is preserved, including physical microMIPS FP
assembly. LLVMC copies floating views of integer storage as object bytes;
ordinary C assignment would change these bit patterns numerically.

The independent GNU/LLVM encoding fixtures cover both byte orders. Original
execution covers classic MIPS32/64 and physical microMIPS32/64. I6400 supplies
the classic 64-bit reference. The pinned Unicorn fork adds a
[generic microMIPS64 R6 model](https://github.com/NeverSight/unicorn/pull/23)
using its existing QEMU micro decoder; it is not a claim about I6400 hardware.
The 112 FP forms execute their original micro bytes in both GPR widths and byte
orders, with finite values, NaNs and rounding changes. Reference tests also
cover double transfers and compact FP branches. Its [native doubleword decoder](https://github.com/NeverSight/unicorn/pull/24) also executes physical integer
encodings directly: 21 forms, 18 wide/signed operand pairs and both byte orders
are compared against NeverD, including every GPR and next PC. The test
executes above 4 GiB with Status.KX and an identity-mapped virtual address
space. A doubleword load/store at 0x100040000 must copy eight bytes while
leaving the page with the same low address bits at 0x40000 unchanged.
Fixed call round trips use explicit classic o32 and n64 FR=1 profiles in HighC
and LLVMC at O0 and O2: single/double precision, an integer parameter after an
FP entry parameter, nine-argument overflow, and a floating call argument after
an integer (including o32 register pairs). Word, pointer and void callees
make the upper bits of n64 results and FCSR side effects observable. Each
rebuilt caller is compared with the original for result bits, FCSR, SP, RA
and ABI-preserved FPRs. The [execution receipt](evidence/mips-micro64-fp-calls-2026-11-13.json)
records 224 rebuilt callers and 4,480 comparisons, including signed and
unsigned narrow arguments observed at the full width of the register.
Single-precision result inference follows the arithmetic operation rather than
the FPR container. A final source byte-use proof may canonicalize unread upper
bits; stores, calls, preserved registers and observed return bytes block that
proof. A full replacement in the same block also proves an earlier return
register definition cannot supply the implicit result; explicit reads of that
earlier value remain observable. LowIR and analysis-only MedIR retain the
original unpredictable state.

Discovery keeps the detail-off decode path: a delayed-unit snapshot never reads
Capstone's retained but unpopulated operand allocation. MIPS lifting uses the
physical bytes, so discovery does not need to enable operand detail merely to
avoid a stale operand count. Repeated SDK requests and poisoned detail counts
exercise this boundary separately from instruction semantics.

The baseline for decoder measurements is
bcef8d07e1, with matched Release compiler and dependency inputs except for
the intentional Capstone revision change. The paired driver defaults to the four
classic Release 2 profiles from its `.def` catalog. Optional profiles are selected
explicitly. MSA64, legacy DSP64, scalar FPU validation and nine-pair compressed
decode/lift results are recorded in
[the MSA64 and scalar FP qualification receipt](evidence/mips-wide-fp-2026-11-12.json).
