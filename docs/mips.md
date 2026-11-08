# MIPS analysis

The experimental MIPS analysis path targets linked ELF programs with classic
MIPS32 Release 2/o32 or MIPS64 Release 2/n64 instructions, in either byte order,
and the bounded microMIPS32 Release 2/o32 subset described below.
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

This is a strict subset. n32, historical MIPS I–V profiles, Release 6, MIPS16,
microMIPS64, floating point, DSP/MSA, privileged instructions, LL/SC reservations,
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
coverage and does not admit MIPS16, other ISA revisions or DSP/MSA encodings.

Physical instructions occupy two or four bytes, including four-byte heads at
addresses congruent to two modulo four. The execution-unit record keeps the
actual head and slot widths. JALR16/JALRS16 require four/two-byte slots, JRC has
no slot, and a slot cannot itself transfer control. Link values retain the
caller's mode tag, while direct JALX destinations and serialized indirect
targets have distinct validation rules. Defined function GOT cells use callable
tags; data symbol addresses keep their original bits.

Both source routes retain microMIPS/nomicromips function attributes. A source
return must prove the captured incoming RA, including its mode bit, from the
original instruction boundaries and SSA. Tail calls and unproved stack restores
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
