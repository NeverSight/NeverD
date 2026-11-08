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
- o32 uses `$a0`–`$a3`, a 16-byte caller argument area and aligned register pairs
  for 64-bit integers. n64 uses registers 4–11 and eight-byte argument slots.
  Big-endian integer pairs put their high word first, and narrow stack scalars
  are right justified. Single-carrier results use `$v0`; o32 64-bit results
  use `$v0/$v1` in target word order. These placement rules are tested
  separately from source-signature reconstruction. The fixed integer-call
  adapter still declines split o32 values, register-padding holes and narrow
  big-endian overflow slots; complete typed ABI reconstruction is not claimed.

This is a strict subset. n32, historical MIPS I–V profiles, Release 6, MIPS16,
microMIPS, floating point, DSP/MSA, privileged instructions, LL/SC reservations,
unaligned merge accesses and trapping arithmetic/division are not admitted by
the initial lifter. Unsupported instructions, missing slots and transfers in a
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
data layouts for all four width/byte-order combinations.

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

The initial qualification used macOS arm64 Release: 1,212 focused/shared
CTest cases passed, including 133 MIPS cases. All 128 source roundtrip groups
(16 kernels, four profiles, two backends) passed 312 input cases. The shared
parameter-width proof also covers x86-64, ARM and AArch64, preserving real
truncations and excluding unrelated values. Linux/Windows host builds and GUI
operation were not exercised. Local receipts are
`build-mips/ctest-mips-qualified.log`, `build-mips/roundtrip-final/results.json`
and `build-mips/qualification.json`.

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
