//===- BinaryImageModel.h - The unified loader output -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines \ref BinaryImage, the format-agnostic structure every loader
/// produces: the segments, sections, symbols, imports, exports, relocations,
/// and exception metadata parsed out of a binary, plus the lookups the rest of
/// the pipeline reaches them through.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_BINARYIMAGEMODEL_H
#define NEVERD_LOADER_BINARYIMAGEMODEL_H

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/evm/EVMImageMetadata.h"
#include "neverd/loader/BinaryImageDynamic.h"
#include "neverd/loader/BinaryImageRelocation.h"
#include "neverd/loader/BinaryImageSection.h"
#include "neverd/loader/ExceptionTable.h"
#include "neverd/object/SectionNames.h"
#include "neverd/sbf/SBFMetadata.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd {

/// One concrete pointer slot bound by Mach-O dyld metadata.  This is kept
/// separate from ImportPtrSlots: that map is indirect-symbol-table provenance
/// used by the patcher, while this record comes from the classic/chained bind
/// programs and also preserves the runtime relocation's signed addend.
struct ImportBindSlot {
  std::string Name;
  int64_t Addend = 0;
};

/// Exact encoded relocation field that materializes a complete address
/// operand. EncodedValue is the canonical unsigned value written to Width
/// bytes; TargetVA is the mapped address it denotes. Keeping both prevents a
/// section-difference expression (whose field is A-B but whose eventual target
/// is A) from being mistaken for a complete address occurrence.
enum class RelocatedAddressFieldKind : uint8_t {
  Generic,
  /// i386 ELF R_386_GOTOFF instruction operand.  This is distinct from an
  /// absolute R_386_32 field even when both encode the same mapped data VA.
  I386ELFGOTOFF,
};

struct RelocatedAddressField {
  uint64_t EncodedValue = 0;
  va_t TargetVA = InvalidVA;
  uint8_t Width = 0;
  /// Address inside the section/segment that semantically owns TargetVA.
  /// This differs from TargetVA for one-past relocations, which may
  /// numerically coincide with the next section's first byte.  InvalidVA
  /// means no occurrence-local owner was retained.
  va_t TargetOwnerVA = InvalidVA;
  /// The encoded field is a signed PC-relative displacement whose complete
  /// target depends on the containing instruction's end address.  Loaders do
  /// not know that boundary; CFG lifting recomputes TargetVA occurrence-
  /// locally before the architecture lifter consumes the field.
  bool PCRelativeFromInstructionEnd = false;
  /// Relocation semantics that cannot be reconstructed from the folded value
  /// or PC-relative bit alone.  Most address operands are Generic; consumers
  /// of the i386 GOT-base-zero model must require I386ELFGOTOFF explicitly so
  /// an absolute field cannot borrow that proof.
  RelocatedAddressFieldKind Kind = RelocatedAddressFieldKind::Generic;
};

/// Loader-authenticated instruction whose architectural output materializes a
/// complete address even though the relocation patches a non-contiguous
/// instruction bitfield rather than a standalone pointer-sized operand.
/// AArch64 ADRP+ADD PAGEOFF and ARM MOVW/MOVT pairs are the canonical forms.
/// Consumers must still bind this record to the exact decoded LowOp output;
/// the target value alone is never address provenance.
struct RelocatedInstructionAddressMaterialization {
  va_t TargetVA = InvalidVA;
  va_t TargetOwnerVA = InvalidVA;
};

/// One R_ARM_REL32 literal after the ELF loader has applied it to its mapped
/// section.  RelocationEntry::Address is a raw section-relative r_offset for
/// ET_REL inputs, so CFG consumers must use this applied SlotVA-keyed record
/// rather than trying to match a mapped literal address against the global
/// relocation list.  EncodedValue is the exact 32-bit displacement written at
/// SlotVA; TargetVA/TargetOwnerVA retain the relocated symbol identity.
struct AppliedARMRelativeLiteral {
  uint32_t EncodedValue = 0;
  va_t TargetVA = InvalidVA;
  va_t TargetOwnerVA = InvalidVA;

  bool operator==(const AppliedARMRelativeLiteral &Other) const = default;
};

/// One i386 R_386_GOTPC field after the ELF loader has applied it at its
/// mapped instruction address.  The relocation establishes the architectural
/// model used by NeverD's relocatable image: adding EncodedValue to the exact
/// get-PC seed ExpectedPCValue produces the model-zero GOT base.  Consumers
/// must still prove that the matching instruction input reaches that exact PC
/// seed; the numeric zero alone is never provenance.
struct AppliedI386GOTPCField {
  uint32_t EncodedValue = 0;
  uint32_t ExpectedPCValue = 0;

  bool operator==(const AppliedI386GOTPCField &Other) const = default;
};

// ===--------------------------------------------------------------------===//
// BinaryImage — the unified output of all loaders
// ===--------------------------------------------------------------------===//

struct BinaryImage {
  // The type is written fully-qualified so the member's name (`Arch`) does not
  // shadow the enum type `neverd::Arch` inside the struct scope — an
  // -Wchanges-meaning hard error under GCC.  All consumers still read `.Arch`.
  neverd::Arch Arch = neverd::Arch::Unknown;
  InstructionMode Mode = InstructionMode::Default;
  BinaryFormat Format = BinaryFormat::Unknown;
  Bitness Bits = Bitness::Unknown;
  bool IsRelocatable = false;
  va_t Base = 0;
  va_t Entry = 0;
  std::vector<Segment> Segments;
  std::vector<Section> Sections;
  std::vector<Import> Imports;
  std::vector<Export> Exports;
  std::vector<Symbol> Symbols;
  std::vector<RelocationEntry> Relocations;
  std::vector<BaseRelocation> BaseRelocations;
  /// Present only for Solana SBF ELF inputs.  The dedicated loader owns ELF
  /// parsing; the frontend consumes this typed record and never reparses Raw.
  std::optional<sbf::Metadata> SBF;
  /// Present only for EVM inputs.  An EVM input has no header, so what kind of
  /// container it is and which compiler emitted it are conclusions the loader
  /// reaches by reading the bytes.  Dropping them at this boundary would leave
  /// the frontend to re-derive them — under a different hardfork, and therefore
  /// differently — or to report an unknown build for a contract that named
  /// itself.
  std::optional<evm::ImageMetadata> EVM;
  /// Virtual addresses that a relocation resolves to inside a read-only data
  /// segment (filled by the loader as it applies relocations).  A code constant
  /// equal to one of these is a genuine pointer into rodata — e.g. a low-VA
  /// i386 GOTOFF `.rodata` base the numeric-VA heuristic would read as an
  /// integer literal — so the emitter can hoist it.  Only relocation forms that
  /// bake an absolute data VA into the code are recorded (i386 GOTOFF/abs32);
  /// PC/page-relative forms (x86-64 rip, AArch64 ADRP) already resolve through
  /// the existing >kMinGlobalDataAddr path and are intentionally left out.
  std::set<uint64_t> RelocDataAddrs;
  /// Exact relocation-field VA -> resolved data-address operand for relocation
  /// forms that materialize the address in an instruction.  Unlike
  /// RelocDataAddrs this is occurrence provenance: CFG lifting consults the
  /// field inside the current instruction before tagging the matching constant
  /// as DataAddress, so an unrelated integer that merely equals the same target
  /// VA never inherits pointer meaning.  i386 R_386_GOTOFF is the canonical
  /// producer; its relocated displacement is the complete target under the
  /// loader's GOT-base-zero model.
  std::map<va_t, RelocatedAddressField> DataAddressRelocOperands;
  /// Exact instruction VA -> complete address written by that instruction.
  /// Kept separate from DataAddressRelocOperands because PAGEOFF/MovW fields
  /// are encoded bitfields, not byte-address operands a generic consumer may
  /// read or use as a relocation slot.
  std::map<va_t, RelocatedInstructionAddressMaterialization>
      InstructionAddressMaterializations;
  /// Exact ADRP/PAGEBASE instruction VA -> symbol target/owner whose page the
  /// instruction materializes.  A PAGEOFF output is complete only when its
  /// reaching base is a matching record from this map.
  std::map<va_t, RelocatedInstructionAddressMaterialization>
      InstructionPageAddressFragments;
  /// Applied ARM-state R_ARM_REL32 literal fields keyed by mapped slot VA.
  /// A record authenticates the literal field only; CFG lifting must still
  /// prove that an exact LOAD of this slot reaches the architectural PC ADD.
  std::map<va_t, AppliedARMRelativeLiteral> ARMRelativeLiteralFields;
  /// Applied i386 R_386_GOTPC fields keyed by mapped relocation-field VA.
  /// RelocationEntry::Address is section-relative for ET_REL, so CFG/Med
  /// provenance must consume this mapped occurrence instead of rescanning the
  /// raw relocation inventory.
  std::map<va_t, AppliedI386GOTPCField> I386GOTPCFields;
  /// Virtual addresses that a relocation resolves to inside a WRITABLE data
  /// segment (.data/.bss), filled by the loader as it applies relocations.  The
  /// writable counterpart of RelocDataAddrs: it proves a constant is a genuine
  /// pointer to a mutable global (a taken address `&G`, emitted via
  /// GOTOFF/abs32) rather than an ordinary data value that merely collides with
  /// the segment's VA range.  A relocatable .o places a large `static` array at
  /// a low VA, so a wide mutable run spans plain data immediates (an LCG
  /// increment) that must not be mistaken for a stored pointer base; the
  /// emitter gates writable store-value symbolization on membership here.
  std::set<uint64_t> WritableRelocDataAddrs;
  /// Maps a GOTOFF-folded rodata table base (`symbol + addend`) to the VA of
  /// the rodata segment that *contains the referenced symbol*.  clang's
  /// switch-to- lookup-table indexed by the unbiased case value folds the base
  /// to `table - min_case*stride`, a negative addend that lands BEFORE the
  /// table's segment (often inside `.text`).  The bare folded VA therefore
  /// resolves to the wrong segment (or is rejected as code); this map lets the
  /// emitter pin such a base to the symbol's own rodata global with a (possibly
  /// negative) byte offset, so `base + runtime_index` lands back inside the
  /// table.  Filled by the loader for i386 R_386_GOTOFF whose symbol is in
  /// clean rodata.
  std::map<uint64_t, uint64_t> RodataAnchorSeg;
  /// Virtual addresses of relocation slots that hold an absolute pointer into
  /// an executable segment (filled by the loader for R_X86_64_64 /
  /// R_AARCH64_ABS64 / R_386_32 / R_ARM_ABS32 whose target is code).  A run of
  /// these starting at a jump-table base marks the entries as absolute (not
  /// PIC-relative) and its length is the exact entry count — this is how a
  /// computed-goto / threaded dispatch table (`.data.rel.ro` code-pointer
  /// array) is recovered, since it has no comparison guard to bound it and
  /// 4-byte entries are otherwise ambiguous with PIC self-relative offsets.
  std::set<uint64_t> CodePtrRelocSlots;
  /// Virtual addresses of relocation slots that hold an absolute pointer into
  /// mapped non-code data (R_386_32 / R_ARM_ABS32 / R_X86_64_64 /
  /// R_AARCH64_ABS64, or their Mach-O dyld rebase equivalents).  These are the
  /// entries of an absolute data-pointer table — e.g. the `.data.rel.ro` array
  /// of `const char *` a 32-bit-target switch-returning-string lowers to.  The
  /// emitter rebuilds such a table with `ptrtoint(@recompiled_data)` fields so
  /// the pointers survive relinking (the data analogue of CodePtrRelocSlots),
  /// instead of leaving the stale original VAs that would read unmapped memory.
  std::set<uint64_t> DataPtrRelocSlots;
  /// Data-pointer slot -> address inside the relocation symbol's owning
  /// section/segment.  Slot bytes alone cannot disambiguate a one-past target
  /// from the next section's start, and VA zero is a valid ET_REL/COFF object
  /// address rather than an implicit null when this map has an entry.
  std::map<va_t, va_t> DataPtrRelocTargetOwners;
  /// Virtual addresses of 32-bit PC-relative relocation slots in read-only
  /// data whose targets are also non-code data.  Clang uses a contiguous run of
  /// these for a PIC switch that returns one of several data pointers (commonly
  /// string literals): the emitted code loads a signed entry and adds the table
  /// base.  Keeping the exact slot provenance lets later address lowering
  /// distinguish the source offset table from the data segment reached by the
  /// resulting pointer.
  std::set<uint64_t> RelDataPtrRelocSlots;
  /// Virtual addresses of relocation slots in a read-only data segment that
  /// hold a PC-relative reference to an executable target (R_X86_64_PC32 /
  /// R_386_PC32 / R_AARCH64_PREL32 / R_ARM_PREL31 / R_ARM_REL32 whose target is
  /// code).  These are the entries of a PIC `switch` jump table; a contiguous
  /// run starting at the table base gives the exact entry count, which is how a
  /// `switch(x % N)` table — bounded by the modulus, with no `cmp` range guard
  /// — is recovered.  Distinct from CodePtrRelocSlots so the entries keep their
  /// PC-relative (not absolute) interpretation.
  std::set<uint64_t> RelCodeRelocSlots;
  /// Virtual addresses of jump-table *bases* — the read-only data address a
  /// code relocation anchors (the `lea table(%rip)` / `adrp+add` a PIC switch
  /// materializes its table pointer from), filled by the loader for a
  /// relocation whose slot is in executable code and whose target is in
  /// read-only data.  When two unguarded PIC tables are laid out back-to-back
  /// in rodata their RelCodeReloc entry runs merge into one continuous run, so
  /// a run-length count from the first table's base over-reads into the second;
  /// the next anchor strictly above a table's base is that table's exact end
  /// and bounds the run to its real entry count.
  std::set<uint64_t> RelCodeTableAnchors;
  /// Virtual addresses of executable targets whose address is *taken as a
  /// value* — a function pointer materialized into a register by a relocation
  /// or a same-segment PC-relative idiom (`lea rip`, `adrp+add`, `movw/movt`,
  /// an ARM literal pool) and then stored, dispatched, or indirectly called.  A
  /// code constant equal to one of these is a `ptrtoint @func` reference that
  /// must survive relinking, not an integer literal.  Recording the exact
  /// target set keeps the symbolization relocation/lift-driven instead of a
  /// numeric guess, so a genuine integer that merely equals a low function VA
  /// (e.g. 0) is left untouched.  `mutable` because the same-segment x86 `lea
  /// rip` form carries no relocation and is recovered during lifting, when the
  /// image is const.
  mutable std::set<uint64_t> CodeRefTargets;
  /// Code-address counterpart of DataAddressRelocOperands.  The value-keyed
  /// CodeRefTargets remains the cross-stage relocation target inventory; this
  /// map identifies the instruction occurrence whose relocated operand
  /// actually materialized that target.
  std::map<va_t, RelocatedAddressField> CodeAddressRelocOperands;
  /// Function entries accepted only after FuncDetector's bounded semantic
  /// decode. Keep these separate from untyped exports and address-taken code
  /// references: both of those can legally identify data. Mutable because
  /// discovery runs through a const pipeline view and patch-time source
  /// authentication consumes the published result later in the same run.
  mutable std::set<uint64_t> VerifiedFunctionEntries;
  /// Maps the virtual address of a non-lazy/lazy pointer slot (Mach-O __got /
  /// __la_symbol_ptr, the indirect-symbol-table backed import pointer tables)
  /// to the imported symbol it binds to.  Most external calls go through a
  /// __stubs trampoline (recorded as an Import with the stub address), but a
  /// routine with a non-AAPCS ABI — notably Darwin's `____chkstk_darwin` stack
  /// probe — is called GOT-indirect (`ldr xN,[got]; blr xN`) with no call-site
  /// relocation, so its symbol is otherwise unrecoverable.  The rewriter
  /// consults this to identify (and elide) such a probe call, whose implicit
  /// size argument it cannot model and whose stack allocation it already lowers
  /// via a real alloca.
  std::map<va_t, std::string> ImportPtrSlots;
  /// Concrete Mach-O pointer bindings decoded from LC_DYLD_INFO bind bytecode
  /// or LC_DYLD_CHAINED_FIXUPS chains.  Kept separate from ImportPtrSlots so
  /// patch provenance remains an exact view of the indirect symbol table.
  std::map<va_t, ImportBindSlot> DyldBindSlots;
  /// Exact executable import veneer address -> Imports index.  Import::IATAddr
  /// intentionally keeps its format-native, API-visible meaning (a PE/ELF data
  /// slot, historically a Mach-O stub); recording another executable spelling
  /// of the import must never repurpose that field.
  std::map<va_t, size_t> ImportStubIndices;
  /// Half-open executable intervals occupied by linker/dynamic-loader import
  /// machinery whose individual entries may not be recoverable (ELF PLT
  /// families, Mach-O symbol stubs and stub helper).
  std::vector<std::pair<va_t, va_t>> ImportStubRanges;
  /// Loader/CRT invoked function entries whose calling convention is not an
  /// ordinary user-function ABI.  Patch mode preserves these original bodies.
  std::set<va_t> RuntimeFunctionAddrs;
  DynamicInfo DynInfo;
  /// Sorted (start, end) intervals describing code regions that already
  /// belong to a function.  Used by post-pdata scanners (padding scan,
  /// data-func-ptr scan) to skip candidates that fall inside a known body.
  /// Includes BOTH primary RUNTIME_FUNCTION entries and chained-info
  /// continuation chunks.
  std::vector<std::pair<va_t, va_t>> KnownCodeRanges;
  /// Checked, normalized table-based unwind and language exception metadata.
  /// Empty for formats/targets without a supported exception directory.
  ExceptionInfo ExceptionMetadata;
  std::vector<uint8_t> Raw;

  bool is64Bit() const { return neverd::is64Bit(Bits); }
  bool is32Bit() const { return neverd::is32Bit(Bits); }
  bool is256Bit() const { return neverd::is256Bit(Bits); }
  uint32_t getPointerSize() const {
    return getBitnessValue(Bits) / kBitsPerByte;
  }

  std::set<va_t> getSymbolAddresses() const {
    std::set<va_t> Addrs;
    for (const auto &Sym : Symbols)
      Addrs.insert(Sym.Addr);
    return Addrs;
  }

  const uint8_t *readVA(va_t Addr, size_t Len) const {
    for (const auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      size_t Off = static_cast<size_t>(Addr - Seg.VA);
      if (!rangeInBounds(Off, Len, Seg.Data.size()))
        return nullptr;
      return Seg.Data.data() + Off;
    }
    return nullptr;
  }

  /// True if \p Addr falls within any mapped segment of this image.
  bool containsVA(va_t Addr) const {
    for (const auto &Seg : Segments)
      if (Seg.contains(Addr))
        return true;
    return false;
  }

  /// True when mapped/readable section metadata owns any byte of \p Seg.
  /// This is segment-local: a partial/packed image may describe sections for
  /// one mapping while leaving another mapping intentionally section-less.
  bool segmentHasReadableSectionMetadata(const Segment &Seg) const {
    if (Seg.Size > InvalidVA - Seg.VA)
      return false;
    const uint64_t SegmentEnd = Seg.VA + Seg.Size;
    for (const Section &Candidate : Sections) {
      if (!Candidate.isReadable() || Candidate.Size == 0 ||
          Candidate.Size > InvalidVA - Candidate.VA)
        continue;
      const uint64_t CandidateEnd = Candidate.VA + Candidate.Size;
      if (Candidate.VA < SegmentEnd && Seg.VA < CandidateEnd)
        return true;
    }
    return false;
  }

  /// True when \p Addr is backed by format-native object data, rather than
  /// merely falling inside a coarse readable load segment. Exact section
  /// metadata excludes executable bytes plus ELF/Mach-O headers and alignment
  /// gaps; section-less images conservatively fall back to non-executable
  /// file-backed segments.
  bool hasObjectDataProvenance(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    if (!Seg || Seg->Data.empty() || Addr < Seg->VA ||
        Addr - Seg->VA >= Seg->Data.size() || !Seg->isReadable())
      return false;

    if (Sections.empty())
      return !Seg->isExecutable();
    if (const Section *Sec = getSectionFor(Addr))
      return Sec->isReadable() &&
             (isMachO() ? !isCodeAddress(Addr) : !Sec->isExecutable());

    // Some synthetic/stripped images describe a whole data segment but omit a
    // section row for it. Fall back only if no section overlaps this
    // segment at all; otherwise an uncovered byte is a header/alignment gap.
    return !segmentHasReadableSectionMetadata(*Seg) && !Seg->isExecutable();
  }

  /// Loader/layout-only upper bound for constants whose numeric value may
  /// change when the image is rebuilt.  This deliberately ignores use-site
  /// heuristics: callers may conservatively retain arithmetic or CFG edges,
  /// but must never freeze a relation between two independently emitted
  /// symbols.  Keeping this fact on BinaryImage gives every format and every
  /// semantic stage one owner for mapped, anchored, relocation-proven, and
  /// one-past-end address candidates.
  bool isPotentiallyRelocatableAddress(va_t Addr) const {
    const bool LoaderProven = RelocDataAddrs.count(Addr) != 0 ||
                              WritableRelocDataAddrs.count(Addr) != 0 ||
                              RodataAnchorSeg.count(Addr) != 0 ||
                              CodeRefTargets.count(Addr) != 0;
    if (LoaderProven)
      return true;

    // Zero remains the null integer unless exact loader provenance above says
    // otherwise. Relocatable objects may synthesize a section at VA zero, but
    // treating every pointer-width zero as its address would suppress nearly
    // all ordinary zero arithmetic in that object.
    if (Addr == 0)
      return false;

    if (const Segment *Seg = getSegmentFor(Addr))
      return Seg->isReadable();

    // Both read-only runs (file-backed extent) and writable/bss runs (virtual
    // extent) may materialize a legal one-past-end pointer in the emitter.
    for (const Segment &Seg : Segments) {
      if (!Seg.isReadable())
        continue;
      const uint64_t Extents[] = {Seg.Data.size(), Seg.Size};
      for (uint64_t Extent : Extents) {
        const bool HasFileBackedOwner =
            Addr > Seg.VA && hasObjectDataProvenance(Addr - 1);
        const bool HasVirtualDataOwner =
            Extent == Seg.Size && !Seg.isExecutable();
        if (Extent != 0 && Extent <= InvalidVA - Seg.VA &&
            Addr == Seg.VA + Extent && Addr > Seg.VA &&
            (HasFileBackedOwner || HasVirtualDataOwner))
          return true;
      }
    }
    return false;
  }

  /// Write bytes at a virtual address (modifies in-memory segment data).
  /// Returns true on success.
  bool writeVA(va_t Addr, const uint8_t *Src, size_t Len) {
    for (auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      size_t Off = static_cast<size_t>(Addr - Seg.VA);
      if (!rangeInBounds(Off, Len, Seg.Data.size()))
        return false;
      std::memcpy(Seg.Data.data() + Off, Src, Len);
      return true;
    }
    return false;
  }

  /// Patch a pointer-sized value at a virtual address.
  bool patchPtr(va_t Addr, uint64_t Value) {
    uint32_t PtrSz = getPointerSize();
    for (auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      size_t Off = static_cast<size_t>(Addr - Seg.VA);
      if (!rangeInBounds(Off, PtrSz, Seg.Data.size()))
        return false;
      writePtr(Seg.Data.data() + Off, Value, is64Bit());
      return true;
    }
    return false;
  }

  const Segment *getSegmentFor(va_t Addr) const {
    for (const auto &Seg : Segments)
      if (Seg.contains(Addr))
        return &Seg;
    return nullptr;
  }

  /// Return the mapped format-native section that owns \p Addr.
  ///
  /// Relocatable ELF metadata sections do not have runtime addresses.  Their
  /// synthetic VA remains zero, so a large `.strtab`, `.symtab`, or relocation
  /// section can numerically overlap every low-VA SHF_ALLOC section.  Such
  /// metadata must never win an address lookup: Sections with Readable set are
  /// the loader model's mapped/SHF_ALLOC sections, and the address must also be
  /// backed by a mapped segment.
  const Section *getSectionFor(va_t Addr) const {
    if (!getSegmentFor(Addr))
      return nullptr;
    for (const auto &Sec : Sections)
      if (Sec.isReadable() && Sec.contains(Addr))
        return &Sec;
    return nullptr;
  }

  /// Verify an occurrence-local relocated target against the section/segment
  /// owner retained by the loader.  Code addresses must lie strictly inside
  /// their owner; data addresses may name the one-past value used by C/C++
  /// address arithmetic.  Numeric adjacency is not ownership: a target equal
  /// to the next section's base remains one-past the recorded owner.
  bool relocatedTargetBelongsToOwner(va_t TargetVA, va_t OwnerVA) const {
    if (OwnerVA == InvalidVA)
      return false;
    for (const Section &Sec : Sections) {
      if (!Sec.isReadable() || Sec.VA != OwnerVA ||
          Sec.Size > InvalidVA - Sec.VA)
        continue;
      const va_t End = Sec.VA + Sec.Size;
      const bool OwnerIsCode = hasExecutableCodeOwnerAt(OwnerVA);
      return TargetVA >= Sec.VA &&
             (OwnerIsCode ? TargetVA < End && hasExecutableCodeOwnerAt(TargetVA)
                          : TargetVA <= End);
    }
    for (const Segment &Seg : Segments) {
      if (!Seg.isReadable() || Seg.VA != OwnerVA ||
          Seg.Size > InvalidVA - Seg.VA)
        continue;
      const va_t End = Seg.VA + Seg.Size;
      const bool OwnerIsCode = hasExecutableCodeOwnerAt(OwnerVA);
      return TargetVA >= Seg.VA &&
             (OwnerIsCode ? TargetVA < End && hasExecutableCodeOwnerAt(TargetVA)
                          : TargetVA <= End);
    }
    return false;
  }

  /// Verify the symbol owner retained for an i386 ELF R_386_GOTOFF field.
  /// Most GOTOFF addends resolve inside (or one-past) their symbol's ordinary
  /// section owner and use relocatedTargetBelongsToOwner directly.  Clang may
  /// instead fold a negative selector bias into the relocation, placing S+A
  /// before the symbol's rodata load segment.  The ELF loader records that
  /// exceptional relation in RodataAnchorSeg; accept it only when the exact
  /// folded VA names the same read-only data segment as the exact section owner.
  bool relocatedI386GOTOFFTargetBelongsToOwner(va_t TargetVA,
                                               va_t OwnerVA) const {
    if (Arch != Arch::X86 || !isELF() || getPointerSize() != 4 ||
        OwnerVA == InvalidVA)
      return false;
    if (relocatedTargetBelongsToOwner(TargetVA, OwnerVA))
      return true;

    const auto Anchor = RodataAnchorSeg.find(TargetVA);
    if (Anchor == RodataAnchorSeg.end())
      return false;
    const Segment *OwnerSeg = getSegmentFor(OwnerVA);
    if (!OwnerSeg || Anchor->second != OwnerSeg->VA ||
        !OwnerSeg->isReadable() || OwnerSeg->isWritable() ||
        TargetVA > std::numeric_limits<uint32_t>::max() ||
        OwnerSeg->VA > std::numeric_limits<uint32_t>::max() ||
        hasExecutableCodeOwnerAt(OwnerVA) ||
        !hasObjectDataProvenance(OwnerVA))
      return false;

    // GOTOFF arithmetic is performed in the i386 guest's 32-bit address
    // space.  A negative selector bias may therefore wrap below a rodata
    // segment at VA zero to (for example) 0xfffffff0.  Authenticate only the
    // exact loader-recorded anchor and a small, non-zero modular backward
    // distance; ordinary absolute relocations never populate this relation.
    const uint32_t BackDistance = static_cast<uint32_t>(OwnerSeg->VA) -
                                  static_cast<uint32_t>(TargetVA);
    if (BackDistance == 0 ||
        BackDistance > limits::kMaxRodataAnchorBackDistance)
      return false;

    if (const Section *OwnerSec = getSectionFor(OwnerVA))
      return OwnerSec->VA == OwnerVA && OwnerSec->isReadable() &&
             !OwnerSec->isWritable() && !OwnerSec->isExecutable();
    return OwnerSeg->VA == OwnerVA;
  }

  /// Half-open end of the mapped section/object-storage owner containing
  /// \p Addr. An uncovered gap in a segment that otherwise has section
  /// metadata has no owner; a genuinely section-less segment owns its
  /// materialized bytes as one compatibility range.
  std::optional<va_t> mappedObjectOwnerEnd(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    if (!Seg || Addr < Seg->VA || Seg->Data.empty() ||
        Seg->Data.size() > InvalidVA - Seg->VA)
      return std::nullopt;
    const va_t MaterializedEnd = Seg->VA + Seg->Data.size();
    if (const Section *Sec = getSectionFor(Addr)) {
      if (Sec->Size == 0 || Sec->Size > InvalidVA - Sec->VA)
        return std::nullopt;
      return std::min<va_t>(Sec->VA + Sec->Size, MaterializedEnd);
    }
    if (segmentHasReadableSectionMetadata(*Seg))
      return std::nullopt;
    return MaterializedEnd;
  }

  /// Classify one mapped address using the finest format-native provenance.
  /// An exact format-native section takes precedence over coarse segment
  /// permissions. Mach-O uses instruction attributes; ELF/COFF use the
  /// section's executable flag. This matters when linkers place non-code data
  /// in the same RX mapping as `.text`. A header or alignment gap is not code
  /// when section metadata exists; section-less images retain the historical
  /// segment fallback.
  bool isCodeAddress(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    if (!Seg)
      return false;
    if (const Section *Sec = getSectionFor(Addr)) {
      if (isMachO())
        return (Sec->Type & (llvm::MachO::S_ATTR_PURE_INSTRUCTIONS |
                             llvm::MachO::S_ATTR_SOME_INSTRUCTIONS)) != 0;
      return Sec->isExecutable();
    }
    if (segmentHasReadableSectionMetadata(*Seg))
      return false;
    return Seg->isExecutable();
  }

  /// True when one complete half-open byte range has a single code owner.
  bool isCodeRange(va_t Addr, uint64_t Size) const {
    if (Size == 0 || Size - 1 > InvalidVA - Addr)
      return false;
    const va_t Last = Addr + Size - 1;
    const Segment *FirstSeg = getSegmentFor(Addr);
    if (!FirstSeg || getSegmentFor(Last) != FirstSeg || !isCodeAddress(Addr) ||
        !isCodeAddress(Last))
      return false;
    const Section *FirstSec = getSectionFor(Addr);
    const Section *LastSec = getSectionFor(Last);
    return (!FirstSec && !LastSec) ||
           (FirstSec && LastSec && FirstSec == LastSec);
  }

  bool isDataAddress(va_t Addr) const {
    const Segment *Seg = getSegmentFor(Addr);
    return Seg && Seg->isReadable() && !isCodeAddress(Addr);
  }

  const Section *getSectionByName(llvm::StringRef Name) const {
    for (const auto &Sec : Sections)
      if (Sec.Name == Name)
        return &Sec;
    return nullptr;
  }

  /// Read an array of pointer-sized values from the loaded image at \p Addr.
  /// Returns up to \p MaxCount entries; stops at segment boundaries.
  std::vector<va_t> readPtrArray(va_t Addr, size_t MaxCount) const {
    std::vector<va_t> Result;
    uint32_t PtrSz = getPointerSize();
    if (PtrSz == 0 || MaxCount == 0)
      return Result;
    const uint8_t *P = readVA(Addr, PtrSz);
    if (!P)
      return Result;
    for (size_t I = 0; I < MaxCount; ++I) {
      if (I > (InvalidVA - Addr) / PtrSz)
        break;
      P = readVA(Addr + I * PtrSz, PtrSz);
      if (!P)
        break;
      Result.push_back(static_cast<va_t>(readPtr(P, is64Bit())));
    }
    return Result;
  }

  std::vector<va_t> getExecutableRanges() const {
    std::vector<va_t> Ranges;
    for (const auto &Seg : Segments) {
      if (!Seg.isExecutable() || Seg.Size == 0 || Seg.Size > InvalidVA - Seg.VA)
        continue;
      Ranges.push_back(Seg.VA);
      Ranges.push_back(Seg.VA + Seg.Size);
    }
    return Ranges;
  }

  // --- Format queries ---

  bool isELF() const { return Format == BinaryFormat::ELF; }
  bool isCOFF() const { return Format == BinaryFormat::COFF; }
  bool isMachO() const { return Format == BinaryFormat::MachO; }
  bool isEVM() const { return Format == BinaryFormat::EVM; }

  /// Get the main code section (".text" / "__text").
  ///
  /// Falls back to a flag-based pick when the canonical name is absent — e.g. a
  /// binary already processed by a packer/protector that renames its code
  /// section (VMProtect ".vmp0", UPX "UPX1", Themida, randomised names). The
  /// executable section containing the entry point wins; otherwise the largest
  /// executable section. Returns nullptr only when there is no executable
  /// section at all.
  const Section *getTextSection() const {
    if (const Section *Named = getSectionByName(getTextSectionName()))
      return Named;

    const Section *Best = nullptr;
    for (const auto &Sec : Sections) {
      if (!Sec.isExecutable())
        continue;
      if (Entry != 0 && Sec.contains(Entry))
        return &Sec;
      if (!Best || Sec.Size > Best->Size)
        Best = &Sec;
    }
    return Best;
  }

  /// Get the executable segment containing the text section.
  const Segment *getTextSegment() const {
    if (const Section *Text = getTextSection())
      return getSegmentFor(Text->VA);
    for (const auto &Seg : Segments)
      if (Seg.isExecutable())
        return &Seg;
    return nullptr;
  }

  /// Convert a virtual address to a file offset.
  /// Returns ~0ULL on failure.
  uint64_t vaToFileOffset(va_t Addr) const {
    for (const auto &Seg : Segments) {
      if (!Seg.contains(Addr))
        continue;
      uint64_t Delta = Addr - Seg.VA;
      if (Delta >= Seg.FileSz || Delta > InvalidVA - Seg.FileOff)
        return InvalidVA;
      return Seg.FileOff + Delta;
    }
    return InvalidVA;
  }

  /// Convert a file offset to a virtual address.
  /// Returns InvalidVA on failure.
  va_t fileOffsetToVA(uint64_t Offset) const {
    for (const auto &Seg : Segments) {
      if (Offset < Seg.FileOff)
        continue;
      uint64_t Delta = Offset - Seg.FileOff;
      if (Delta < Seg.FileSz && Delta <= InvalidVA - Seg.VA)
        return Seg.VA + Delta;
    }
    return InvalidVA;
  }

  /// For PE/COFF: convert RVA to VA using ImageBase.
  va_t rvaToVA(uint64_t RVA) const { return Base + RVA; }
  /// For PE/COFF: convert VA to RVA using ImageBase.
  uint64_t vaToRVA(va_t Addr) const { return Addr - Base; }

  /// Get all executable segments.
  std::vector<const Segment *> getExecutableSegments() const {
    std::vector<const Segment *> Result;
    for (const auto &Seg : Segments)
      if (Seg.isExecutable())
        Result.push_back(&Seg);
    return Result;
  }

  /// Get all writable segments.
  std::vector<const Segment *> getWritableSegments() const {
    std::vector<const Segment *> Result;
    for (const auto &Seg : Segments)
      if (Seg.isWritable())
        Result.push_back(&Seg);
    return Result;
  }

  /// Check whether this binary has any imports.
  bool hasImports() const { return !Imports.empty(); }

  /// Check whether this binary has any exports.
  bool hasExports() const { return !Exports.empty(); }

  /// Check whether this binary has any relocations.
  bool hasRelocations() const { return !Relocations.empty(); }

  /// True when loader metadata records a relocation/fixup whose storage slot
  /// begins at \p Address.  This is deliberately format-neutral: PE base
  /// relocations, ELF/Mach-O object relocations, dyld binds, imports, and the
  /// normalized absolute/relative pointer-slot sets all answer through the
  /// same query.  Target-address evidence such as RelocDataAddrs and
  /// CodeRefTargets is excluded because those sets do not identify the bytes
  /// being loaded.
  bool hasRelocationProvenanceAt(va_t Address) const {
    if (CodePtrRelocSlots.count(Address) || DataPtrRelocSlots.count(Address) ||
        RelDataPtrRelocSlots.count(Address) ||
        RelCodeRelocSlots.count(Address) || ImportPtrSlots.count(Address) ||
        DyldBindSlots.count(Address))
      return true;
    if (std::any_of(BaseRelocations.begin(), BaseRelocations.end(),
                    [&](const BaseRelocation &Reloc) {
                      return Reloc.Address == Address;
                    }))
      return true;
    return std::any_of(
        Relocations.begin(), Relocations.end(),
        [&](const RelocationEntry &Reloc) { return Reloc.Address == Address; });
  }

  /// Get the total virtual size of all loadable segments.
  uint64_t getVirtualSize() const {
    va_t Lo = InvalidVA, Hi = 0;
    for (const auto &Seg : Segments) {
      if (Seg.Size == 0 || Seg.Size > InvalidVA - Seg.VA)
        continue;
      if (Seg.VA < Lo)
        Lo = Seg.VA;
      va_t End = Seg.VA + Seg.Size;
      if (End > Hi)
        Hi = End;
    }
    return (Lo != InvalidVA && Hi > Lo) ? Hi - Lo : 0;
  }

  /// Find an import by name.
  const Import *findImport(llvm::StringRef Name) const {
    for (const auto &Imp : Imports)
      if (Imp.Name == Name)
        return &Imp;
    return nullptr;
  }

  /// Find an export by name.
  const Export *findExport(llvm::StringRef Name) const {
    for (const auto &Exp : Exports)
      if (Exp.Name == Name)
        return &Exp;
    return nullptr;
  }

  /// Find an export by address.
  const Export *findExportAt(va_t Addr) const {
    for (const auto &Exp : Exports)
      if (Exp.Addr == Addr)
        return &Exp;
    return nullptr;
  }

  /// Find a symbol by name.
  const Symbol *findSymbol(llvm::StringRef Name) const {
    for (const auto &Sym : Symbols)
      if (Sym.Name == Name)
        return &Sym;
    return nullptr;
  }

  /// Find a symbol by address.
  const Symbol *findSymbolAt(va_t Addr) const {
    for (const auto &Sym : Symbols)
      if (Sym.Addr == Addr)
        return &Sym;
    return nullptr;
  }

  /// True when typed symbol metadata owns p Addr as a function entry.
  /// Normalize ARM/Thumb spellings so patch-time synthetic original-VA
  /// symbols can recover the same code identity after reloading the image.
  bool hasFunctionSymbolAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    for (const auto &Sym : Symbols)
      if (Sym.IsFunc &&
          normalizeCodeAddress(Sym.Addr, Arch, Mode) == Normalized)
        return true;
    return false;
  }

  /// Largest non-function symbol size defined exactly at \p Addr (0 if none).
  /// Distinguishes a sized data object (a const array/table) from a bare label,
  /// so a NUL byte inside the object is not mistaken for a string terminator.
  uint64_t dataObjectSizeAt(va_t Addr) const {
    uint64_t Best = 0;
    for (const auto &Sym : Symbols)
      if (Sym.Addr == Addr && !Sym.IsFunc && Sym.Size > Best)
        Best = Sym.Size;
    return Best;
  }

  /// Find a segment by name.
  const Segment *findSegmentByName(llvm::StringRef Name) const {
    for (const auto &Seg : Segments)
      if (Seg.Name == Name)
        return &Seg;
    return nullptr;
  }

  /// Find a mutable segment by name.
  Segment *findSegmentByName(llvm::StringRef Name) {
    for (auto &Seg : Segments)
      if (Seg.Name == Name)
        return &Seg;
    return nullptr;
  }

  /// Get the image end address (highest VA + size across all segments).
  va_t getImageEnd() const {
    va_t Hi = 0;
    for (const auto &Seg : Segments) {
      if (Seg.Size == 0 || Seg.Size > InvalidVA - Seg.VA)
        continue;
      va_t End = Seg.VA + Seg.Size;
      if (End > Hi)
        Hi = End;
    }
    return Hi;
  }

  /// Get the format-appropriate text section name.
  llvm::StringRef getTextSectionName() const {
    if (isEVM())
      return kEVMCodeSectionName;
    return isMachO() ? section_names::macho::Text : section_names::elf::Text;
  }

  /// Get the data section (".data" / "__data").
  const Section *getDataSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Data);
    return getSectionByName(section_names::elf::Data);
  }

  /// Get all function symbols, sorted by address.
  std::vector<const Symbol *> getFunctionSymbols() const {
    std::vector<const Symbol *> Result;
    for (const auto &Sym : Symbols)
      if (Sym.IsFunc)
        Result.push_back(&Sym);
    std::sort(
        Result.begin(), Result.end(),
        [](const Symbol *A, const Symbol *B) { return A->Addr < B->Addr; });
    return Result;
  }

  /// Get the format name as a string.
  const char *getFormatName() const {
    switch (Format) {
    case BinaryFormat::ELF:
      return "ELF";
    case BinaryFormat::COFF:
      return "PE";
    case BinaryFormat::MachO:
      return "Mach-O";
    case BinaryFormat::EVM:
      return kEVMFormatName.data();
    default:
      return "Unknown";
    }
  }

  /// Check if this binary has debug info (PDB path or .debug_info).
  bool hasDebugInfo() const {
    if (!DynInfo.PDBPath.empty())
      return true;
    return getSectionByName(section_names::elf::DebugInfo) != nullptr;
  }

  /// Check if the image has a section with the given name.
  bool hasSection(llvm::StringRef Name) const {
    return getSectionByName(Name) != nullptr;
  }

  /// Check if the image has a segment with the given name.
  bool hasSegment(llvm::StringRef Name) const {
    return findSegmentByName(Name) != nullptr;
  }

  /// Find a section by name, returning a mutable pointer.
  Section *findSectionByName(llvm::StringRef Name) {
    for (auto &Sec : Sections)
      if (Sec.Name == Name)
        return &Sec;
    return nullptr;
  }

  // --- Typed section queries ---

  /// Get all executable sections.
  std::vector<const Section *> getExecutableSections() const {
    std::vector<const Section *> Result;
    for (const auto &Sec : Sections)
      if (Sec.isExecutable())
        Result.push_back(&Sec);
    return Result;
  }

  /// Get the .bss section (".bss" / "__bss").
  const Section *getBssSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Bss);
    return getSectionByName(section_names::elf::Bss);
  }

  /// Get the .rodata section (".rodata" for ELF, "__const" for MachO,
  /// ".rdata" for COFF).
  const Section *getRodataSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Const);
    if (isCOFF())
      return getSectionByName(section_names::coff::Rdata);
    return getSectionByName(section_names::elf::Rodata);
  }

  /// Get the GOT section (".got" / ".got.plt" / "__got").
  const Section *getGotSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Got);
    const Section *S = getSectionByName(section_names::elf::GotPlt);
    return S ? S : getSectionByName(section_names::elf::Got);
  }

  /// Get the PLT section (".plt" / "__stubs").
  const Section *getPltSection() const {
    if (isMachO())
      return getSectionByName(section_names::macho::Stubs);
    return getSectionByName(section_names::elf::Plt);
  }

  /// Get all sections in a segment (MachO) or all sections.
  std::vector<const Section *>
  getSectionsInSegment(llvm::StringRef SegName) const {
    std::vector<const Section *> Result;
    for (const auto &Sec : Sections) {
      if (Sec.SegmentName == SegName)
        Result.push_back(&Sec);
    }
    return Result;
  }

  /// Find an import by its format-native IAT address or an exact executable
  /// veneer registered for it.
  const Import *findImportAt(va_t Addr) const {
    for (const auto &Imp : Imports)
      if (Imp.IATAddr == Addr)
        return &Imp;
    auto It = ImportStubIndices.find(Addr);
    if (It != ImportStubIndices.end() && It->second < Imports.size())
      return &Imports[It->second];
    return nullptr;
  }

  /// Return every address spelling that can identify an import.  Data slots
  /// remain present for indirect-call recovery; executable veneers add the
  /// direct branch targets used by CFG and ABI recovery.
  std::map<va_t, std::string> getImportAddressNames() const {
    std::map<va_t, std::string> Result;
    for (const auto &Imp : Imports)
      if (Imp.IATAddr != 0 && !Imp.Name.empty())
        Result.try_emplace(Imp.IATAddr, Imp.Name);
    for (const auto &[Addr, Index] : ImportStubIndices)
      if (Index < Imports.size() && !Imports[Index].Name.empty())
        Result[Addr] = Imports[Index].Name;
    return Result;
  }

  /// Resolve the best available display name for a function address.
  std::string getFunctionNameAt(va_t Addr) const {
    if (const Export *Exp = findExportAt(Addr); Exp && !Exp->Name.empty())
      return Exp->Name;
    if (const Symbol *Sym = findSymbolAt(Addr); Sym && !Sym->Name.empty())
      return Sym->Name;
    return (kAutoFuncPrefix + llvm::utohexstr(Addr)).str();
  }

  /// Register an executable import veneer without changing Import::IATAddr.
  bool recordImportStub(va_t StubAddr, size_t ImportIndex) {
    if (ImportIndex >= Imports.size())
      return false;
    const Segment *Seg = getSegmentFor(StubAddr);
    if (!Seg || !Seg->isExecutable())
      return false;
    auto [It, Inserted] = ImportStubIndices.try_emplace(StubAddr, ImportIndex);
    return Inserted || It->second == ImportIndex;
  }

  /// Register a checked half-open range of import/binder machinery.
  bool recordImportStubRange(va_t Start, uint64_t Size) {
    if (Size == 0 || Size > InvalidVA - Start)
      return false;
    const Segment *Seg = getSegmentFor(Start);
    if (!Seg || !Seg->isExecutable() || Start - Seg->VA > Seg->Size ||
        Size > Seg->Size - (Start - Seg->VA))
      return false;
    std::pair<va_t, va_t> Range{Start, Start + Size};
    if (std::find(ImportStubRanges.begin(), ImportStubRanges.end(), Range) ==
        ImportStubRanges.end())
      ImportStubRanges.push_back(Range);
    return true;
  }

  /// True when \p Addr is an exact or range-backed executable import veneer.
  bool isImportStubAt(va_t Addr) const {
    auto Exact = ImportStubIndices.find(Addr);
    if (Exact != ImportStubIndices.end() && Exact->second < Imports.size())
      return true;
    for (const auto &[Start, End] : ImportStubRanges)
      if (Addr >= Start && Addr < End)
        return true;
    // Compatibility for format loaders that historically put a code stub in
    // IATAddr (Mach-O). PE/ELF loaders register callable thunks explicitly;
    // their IAT/GOT slots remain data even inside a coarse executable map.
    if (!isMachO())
      return false;
    for (const auto &Imp : Imports)
      if (Imp.IATAddr == Addr)
        return isCodeAddress(Addr);
    return false;
  }

  /// True when loader or format metadata gives \p Addr an executable-code
  /// owner. Exact section permissions are preferred, while typed function
  /// extents, unwind ranges, and explicitly registered import veneers remain
  /// authoritative for packed images whose section flags are incomplete.
  bool hasExecutableCodeOwnerAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    const Segment *Seg = getSegmentFor(Normalized);
    if (!Seg || !Seg->isExecutable())
      return false;
    if (isCodeAddress(Normalized) || isImportStubAt(Addr) ||
        isImportStubAt(Normalized) ||
        (Entry != 0 && normalizeCodeAddress(Entry, Arch, Mode) == Normalized) ||
        RuntimeFunctionAddrs.count(Addr) != 0 ||
        RuntimeFunctionAddrs.count(Normalized) != 0 ||
        VerifiedFunctionEntries.count(Normalized) != 0)
      return true;
    for (const auto &[Start, End] : KnownCodeRanges)
      if (Normalized >= Start && Normalized < End)
        return true;
    for (const Symbol &Sym : Symbols) {
      if (!Sym.IsFunc)
        continue;
      const va_t Start = normalizeCodeAddress(Sym.Addr, Arch, Mode);
      if (Normalized == Start)
        return true;
      if (Sym.Size != 0 && Sym.Size <= InvalidVA - Start &&
          Normalized > Start && Normalized < Start + Sym.Size)
        return true;
    }
    return false;
  }

  /// True when p Addr is an authenticated callable entry, rather than merely
  /// an address inside executable code. Untyped COFF exports are deliberately
  /// excluded; they require decode/unwind evidence before a patcher may place
  /// a trampoline over the original bytes.
  bool hasAuthenticatedFunctionEntryAt(va_t Addr) const {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    if (!hasExecutableCodeOwnerAt(Normalized))
      return false;
    if ((Entry != 0 && normalizeCodeAddress(Entry, Arch, Mode) == Normalized) ||
        RuntimeFunctionAddrs.count(Addr) != 0 ||
        RuntimeFunctionAddrs.count(Normalized) != 0 ||
        VerifiedFunctionEntries.count(Normalized) != 0 ||
        ImportStubIndices.count(Addr) != 0 ||
        ImportStubIndices.count(Normalized) != 0)
      return true;
    for (const auto &[Start, End] : ImportStubRanges)
      if (Start < End && Start == Normalized)
        return true;
    for (const auto &[Start, End] : KnownCodeRanges)
      if (Start < End && Start == Normalized)
        return true;
    if (hasFunctionSymbolAt(Normalized))
      return true;
    // Export tables are not type metadata: Mach-O export tries and PE/ELF
    // dynamic exports may name data.  An export is callable on its own only
    // when an exact mapped instruction section owns the address.  A
    // section-less image still accepts typed symbols, unwind ranges, entry
    // points, and explicit import stubs above, but never upgrades every RX
    // export into a function merely because the segment is executable.
    if (Format != BinaryFormat::COFF && getSectionFor(Normalized))
      for (const Export &Exp : Exports)
        if (normalizeCodeAddress(Exp.Addr, Arch, Mode) == Normalized &&
            isCodeAddress(Normalized))
          return true;
    return false;
  }

  /// True when every byte in one decoded instruction/range has the same
  /// structural code owner. This prevents a decoder from joining an opcode at
  /// the end of a code section with displacement bytes from adjacent data.
  bool hasExecutableCodeOwnerRange(va_t Addr, uint64_t Size) const {
    if (Size == 0)
      return false;
    const va_t Start = normalizeCodeAddress(Addr, Arch, Mode);
    if (Size - 1 > InvalidVA - Start)
      return false;
    const va_t Last = Start + Size - 1;
    const Segment *Seg = getSegmentFor(Start);
    if (!Seg || !Seg->isExecutable() || getSegmentFor(Last) != Seg)
      return false;
    if (isCodeRange(Start, Size))
      return true;
    for (const auto &[RangeStart, RangeEnd] : ImportStubRanges)
      if (Start >= RangeStart && Last < RangeEnd)
        return true;
    for (const auto &[RangeStart, RangeEnd] : KnownCodeRanges)
      if (Start >= RangeStart && Last < RangeEnd)
        return true;
    for (const Symbol &Sym : Symbols) {
      if (!Sym.IsFunc)
        continue;
      const va_t SymStart = normalizeCodeAddress(Sym.Addr, Arch, Mode);
      if (Sym.Size == 0)
        continue;
      if (Sym.Size <= InvalidVA - SymStart && Start >= SymStart &&
          Last < SymStart + Sym.Size)
        return true;
    }
    return false;
  }

  /// Record a structurally identified loader/runtime function when it maps to
  /// executable code.  Address zero is valid for relocatable images.
  bool recordRuntimeFunction(va_t Addr) {
    const va_t Normalized = normalizeCodeAddress(Addr, Arch, Mode);
    const Segment *Seg = getSegmentFor(Normalized);
    if (!Seg || !Seg->isExecutable() || !isCodeAddress(Normalized))
      return false;
    RuntimeFunctionAddrs.insert(Addr);
    return true;
  }

  bool isRuntimeFunctionAt(va_t Addr) const {
    return RuntimeFunctionAddrs.count(Addr) != 0;
  }

  /// Get imports for a specific module/library.
  std::vector<const Import *>
  getImportsForModule(llvm::StringRef Module) const {
    std::vector<const Import *> Result;
    for (const auto &Imp : Imports)
      if (Imp.Module == Module)
        Result.push_back(&Imp);
    return Result;
  }

  /// Check if the binary is position-independent (has base relocations
  /// or is a shared object).
  bool isPositionIndependent() const { return !BaseRelocations.empty(); }

  // --- Mutation helpers ---

  /// Add a new section to the image model (does not modify the raw binary).
  Section &addSection(llvm::StringRef Name, va_t VA, uint64_t Size,
                      SegmentFlags Flags = SegmentFlags::Readable) {
    Section Sec;
    Sec.Name = Name.str();
    Sec.VA = VA;
    Sec.Size = Size;
    Sec.Flags = Flags;
    Sections.push_back(std::move(Sec));
    return Sections.back();
  }

  /// Remove a section by name. Returns true if a section was removed.
  bool removeSection(llvm::StringRef Name) {
    auto It = std::remove_if(Sections.begin(), Sections.end(),
                             [&](const Section &S) { return S.Name == Name; });
    if (It == Sections.end())
      return false;
    Sections.erase(It, Sections.end());
    return true;
  }

  /// Add a new segment to the image model.
  Segment &addSegment(llvm::StringRef Name, va_t VA, uint64_t Size,
                      SegmentFlags Flags = SegmentFlags::Readable) {
    Segment Seg;
    Seg.Name = Name.str();
    Seg.VA = VA;
    Seg.Size = Size;
    Seg.Flags = Flags;
    Segments.push_back(std::move(Seg));
    return Segments.back();
  }

  /// Add a symbol to the image.
  Symbol &addSymbol(llvm::StringRef Name, va_t Addr, uint64_t Size = 0,
                    bool IsFunc = false) {
    Symbol S;
    S.Name = Name.str();
    S.Addr = Addr;
    S.Size = Size;
    S.IsFunc = IsFunc;
    Symbols.push_back(std::move(S));
    return Symbols.back();
  }

  /// Dump a summary for LLVM_DEBUG output (shared by all loaders).
  void debugDumpSummary(llvm::raw_ostream &OS, llvm::StringRef Prefix) const {
    OS << Prefix << " " << getFormatName() << getBitnessName(Bits)
       << " | arch=" << getArchName(Arch) << " base=0x" << llvm::utohexstr(Base)
       << " entry=0x" << llvm::utohexstr(Entry) << " segs=" << Segments.size()
       << " secs=" << Sections.size() << " syms=" << Symbols.size()
       << " imports=" << Imports.size() << " exports=" << Exports.size()
       << "\n";
  }
};

} // namespace neverd

#endif // NEVERD_LOADER_BINARYIMAGEMODEL_H
