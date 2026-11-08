# MIPS analysis

The experimental MIPS analysis path targets linked ELF programs with classic
MIPS32 Release 2/o32 or MIPS64 Release 2/n64 instructions, in either byte order.
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
- Ordinary disassembly consumes four bytes. A delayed transfer and its slot
  form an eight-byte semantic execution unit. The branch reads its condition
  and indirect target before the slot, writes its link before the slot, and
  transfers after it. Branch-likely instructions annul the slot on the false
  path. The IR retains separate head/slot addresses and byte ownership; entry
  directly into a slot decodes that instruction independently.
- Integer arithmetic, shifts/rotates, comparisons, conditional moves, HI/LO
  multiplication and aligned integer loads/stores have explicit lifter rules.
  MIPS64 word results are sign extended where the ISA requires it. Writes to
  `$zero` discard the result without discarding required memory effects.
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
microMIPS, floating point, DSP/MSA, privileged instructions, LL/SC reservations,
unaligned merge stores and trapping arithmetic/division are not admitted by
the current lifter. Unsupported instructions, missing slots and transfers in a
delay slot produce errors, not replacement NOPs. Floating/aggregate source ABI
classification, arbitrary PIC/gp recovery, raw-input target selection and
native code rewriting are not claimed. Unicorn is a test reference, not a
NeverD MIPS guest execution backend.

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
