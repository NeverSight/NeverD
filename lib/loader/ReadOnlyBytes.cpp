//===- ReadOnlyBytes.cpp - Immutable image storage queries ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate mapping, permissions and fixups for typed image reads.
//===----------------------------------------------------------------------===//

#include "neverd/loader/ReadOnlyBytes.h"

#include "ReadOnlyBytesIndex.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <bit>
#include <set>

namespace neverd {

bool BinaryImage::readOnlyAfterRelocations(va_t Addr, uint64_t Width) const {
  const Segment *Seg = getSegmentFor(Addr);
  if (!Seg || !Width || Width > Seg->Size - (Addr - Seg->VA))
    return false;
  if (!Seg->isWritable() || Seg->ReadOnlyAfterRelocations ||
      section_names::isReadOnlyAfterRelocSectionName(Seg->Name))
    return true;
  if (const Section *Sec = getSectionFor(Addr);
      Sec && Width <= Sec->Size - (Addr - Sec->VA) &&
      (section_names::isReadOnlyAfterRelocSectionName(Sec->Name) ||
       section_names::isReadOnlyAfterRelocSectionName(Sec->SegmentName)))
    return true;
  if (ELFMetadata)
    for (const ELFProgramHeader &Header : ELFMetadata->ProgramHeaders)
      if (Header.Type == llvm::ELF::PT_GNU_RELRO &&
          Addr >= Header.VirtualAddress &&
          Addr - Header.VirtualAddress <= Header.MemorySize &&
          Width <= Header.MemorySize - (Addr - Header.VirtualAddress))
        return true;
  return false;
}

namespace {
// Keep the cost model beside the storage readers it bounds. No index is
// cached across image changes, and an exhausted query grants no partial read.
bool chargeStorageQuery(const BinaryImage &Image, uint32_t Size,
                        bool CodePointer, size_t &Work, size_t Limit) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Image.IsRelocatable)
    return false;
  const auto Charge = [&](size_t Amount) {
    if (Amount > Limit - std::min(Work, Limit)) {
      Work = Limit;
      return false;
    }
    Work += Amount;
    return true;
  };
  const auto Lookup = [&](size_t Count, uint64_t Window) {
    // A balanced-tree search visits at most twice the binary height. Unique
    // integer keys bound the following overlap walk independently of size.
    return Charge(2 * std::bit_width(Count) + 1) &&
           Charge(size_t(std::min<uint64_t>(Count, Window)));
  };
  const auto Mapping = [&] {
    // Include the nested getSectionFor/getSegmentFor calls, mapping end,
    // permission checks, sectionless/execute-only fallbacks, overlap scans
    // and the final readVA lookup.
    for (unsigned I = 0; I != 12; ++I)
      if (!Charge(Image.Sections.size()) || !Charge(Image.Segments.size()))
        return false;
    return true;
  };
  const auto Fixups = [&](uint32_t Extent) {
    const uint64_t Window = uint64_t(Extent) + Image.getPointerSize() - 1;
    for (size_t Count :
         {Image.CodePtrRelocSlots.size(), Image.DataPtrRelocSlots.size(),
          Image.DataPtrRelocTargetOwners.size(), Image.RelCodeRelocSlots.size(),
          Image.RelDataPtrRelocSlots.size(),
          Image.MachOResolvedChainedPointerSlots.size(),
          Image.ConflictingImportStorageSlots.size(),
          Image.ImportPtrSlots.size(), Image.ImportStorageSlots.size(),
          Image.DyldBindSlots.size(), Image.ObjCSourceReferences.size(),
          Image.DataAddressRelocOperands.size(),
          Image.CodeAddressRelocOperands.size()})
      if (!Lookup(Count, Window))
        return false;
    for (size_t Count : {Image.Relocations.size(), Image.BaseRelocations.size(),
                         Image.RuntimeCallablePointerSlots.size()})
      if (!Charge(Count) || !Charge(1))
        return false;
    return true;
  };
  if (!Charge(Size))
    return false;
  if (!CodePointer)
    return Mapping() && Fixups(Size ? Size : 1);
  // PE32 code-pointer storage checks the exact HIGHLOW multiplicity, source
  // conflicts, raw executable target and immutable first target byte.
  return Lookup(Image.CodePtrRelocSlots.size(), 0) &&
         Lookup(Image.DataPtrRelocTargetOwners.size(), 0) &&
         Charge(Image.BaseRelocations.size()) && Charge(1) && Mapping() &&
         Fixups(4) && Mapping() && Mapping() && Fixups(1) && Charge(1);
}

bool supportedImage(const BinaryImage &Image) {
  return Image.Format == BinaryFormat::MachO && Image.Bits == Bitness::Bits64 &&
         supportsImmutableImageReads(Image);
}

bool supportedPEImage(const BinaryImage &Image) {
  return Image.Format == BinaryFormat::COFF &&
         supportsImmutableImageReads(Image);
}

bool supportedImmutableCodeImage(const BinaryImage &Image) {
  return supportedImage(Image) || supportedPEImage(Image) ||
         (Image.Format == BinaryFormat::ELF && !Image.IsRelocatable &&
          Image.Arch == Arch::ARM && Image.Bits == Bitness::Bits32);
}

bool overlaps(va_t Address, uint64_t Extent, va_t Base, uint64_t Width) {
  return Width && Extent &&
         (Base <= Address ? Address - Base < Width : Base - Address < Extent);
}

const uint8_t *mappedBytes(const BinaryImage &Image, va_t Address,
                           uint64_t Extent, bool Immutable, bool Code = false) {
  // A raw instruction-storage extent needs execution permission. The byte
  // readers additionally require readable storage for their stronger proof.
  const bool RequireReadable = !Code || Immutable;
  if (!Address || Address > InvalidVA - Extent)
    return nullptr;
  if (Code) {
    if (!Image.isCodeRange(Address, Extent))
      return nullptr;
  } else {
    const auto End = Image.mappedObjectOwnerEnd(Address);
    if (!End || *End < Address || Extent > *End - Address)
      return nullptr;
  }
  const auto *Section = Image.getSectionFor(Address);
  // getSectionFor deliberately exposes only readable section metadata. A
  // raw execute-only instruction extent still needs its precise section.
  if (!Section && Code && !RequireReadable)
    for (const auto &Candidate : Image.Sections)
      if (Candidate.isExecutable() && Candidate.contains(Address)) {
        Section = &Candidate;
        break;
      }
  const auto *Segment = Image.getSegmentFor(Address);
  if (!Section && Code && Image.Format == BinaryFormat::ELF &&
      Image.Arch == Arch::ARM && Image.ARMReachabilityConstrained && Segment &&
      (!RequireReadable || Segment->isReadable()) && Segment->isExecutable() &&
      Image.instructionModeAt(Address) &&
      (!Immutable || Segment->ReadOnlyAfterRelocations ||
       !Segment->isWritable()) &&
      rangeInBounds(Address - Segment->VA, Extent, Segment->Size) &&
      rangeInBounds(Address - Segment->VA, Extent, Segment->FileSz)) {
    for (const auto &Other : Image.Segments)
      if (&Other != Segment && overlaps(Address, Extent, Other.VA, Other.Size))
        return nullptr;
    return Image.readVA(Address, Extent);
  }
  if (!Section || !Segment ||
      (RequireReadable && (!Section->isReadable() || !Segment->isReadable())) ||
      Image.isCodeAddress(Address) != Code ||
      (Code && (!Section->isExecutable() || !Segment->isExecutable())) ||
      (Immutable && !Segment->ReadOnlyAfterRelocations &&
       (Section->isWritable() || Segment->isWritable())) ||
      Section->Size > InvalidVA - Section->VA ||
      Segment->Size > InvalidVA - Segment->VA || Section->VA < Segment->VA ||
      !rangeInBounds(Address - Section->VA, Extent, Section->Size) ||
      !rangeInBounds(Address - Section->VA, Extent, Section->FileSz) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->Size) ||
      !rangeInBounds(Address - Segment->VA, Extent, Segment->FileSz) ||
      Section->FileOff < Segment->FileOff ||
      Section->FileOff - Segment->FileOff != Section->VA - Segment->VA)
    return nullptr;
  if (Image.Format == BinaryFormat::MachO) {
    const auto Type = Section->Type & llvm::MachO::SECTION_TYPE;
    if (Type == llvm::MachO::S_ZEROFILL || Type == llvm::MachO::S_GB_ZEROFILL ||
        Type == llvm::MachO::S_THREAD_LOCAL_ZEROFILL)
      return nullptr;
  }
  for (const auto &Other : Image.Sections)
    if (&Other != Section && overlaps(Address, Extent, Other.VA, Other.Size))
      return nullptr;
  for (const auto &Other : Image.Segments)
    if (&Other != Segment && overlaps(Address, Extent, Other.VA, Other.Size))
      return nullptr;
  return Image.readVA(Address, Extent);
}

// Byte copies admit no fixup. Each pointer reader admits only its exact
// normalized slot kind, with independently checked resolution and target.
bool hasConflictingFixups(const BinaryImage &Image, va_t Address,
                          uint64_t Extent, bool Pointer, bool Import = false,
                          bool ObjCReference = false, bool CodePointer = false,
                          bool ExactPEPointer = false) {
  const uint64_t PointerWidth = Image.getPointerSize();
  auto Touches = [&](const auto &Slots, auto Key, bool AllowExact = false) {
    auto It = Slots.lower_bound(
        Address >= PointerWidth - 1 ? Address - (PointerWidth - 1) : 0);
    for (; It != Slots.end() &&
           (Key(*It) < Address || Key(*It) - Address < Extent);
         ++It)
      if (overlaps(Address, Extent, Key(*It), PointerWidth) &&
          !(AllowExact && Key(*It) == Address))
        return true;
    return false;
  };
  auto Set = [](va_t Slot) { return Slot; };
  auto Map = [](const auto &Slot) { return Slot.first; };
  if (Touches(Image.CodePtrRelocSlots, Set, CodePointer) ||
      Touches(Image.DataPtrRelocSlots, Set, Pointer) ||
      Touches(Image.DataPtrRelocTargetOwners, Map, Pointer) ||
      Touches(Image.RelCodeRelocSlots, Set) ||
      Touches(Image.RelDataPtrRelocSlots, Set) ||
      Touches(Image.MachOResolvedChainedPointerSlots, Set,
              Pointer || Import || CodePointer) ||
      Touches(Image.ConflictingImportStorageSlots, Set) ||
      Touches(Image.ImportPtrSlots, Map, Import) ||
      Touches(Image.ImportStorageSlots, Map, Import) ||
      Touches(Image.DyldBindSlots, Map, Import) ||
      Touches(Image.ObjCSourceReferences, Map, ObjCReference) ||
      Touches(Image.DataAddressRelocOperands, Map) ||
      Touches(Image.CodeAddressRelocOperands, Map))
    return true;
  for (const auto &Relocation : Image.Relocations)
    if (overlaps(Address, Extent, Relocation.Address, 8))
      return true;
  for (const auto &Relocation : Image.BaseRelocations) {
    const uint64_t Width =
        Relocation.Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW ? 4 : 8;
    if (overlaps(Address, Extent, Relocation.Address, Width) &&
        !(ExactPEPointer && Relocation.Address == Address &&
          Width == PointerWidth &&
          (Relocation.Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW ||
           Relocation.Type == llvm::COFF::IMAGE_REL_BASED_DIR64)))
      return true;
  }
  for (const auto &Slot : Image.RuntimeCallablePointerSlots)
    if (overlaps(Address, Extent, Slot.SlotVA, PointerWidth))
      return true;
  return false;
}
} // namespace

bool supportsImmutableImageReads(const BinaryImage &Image) {
  if (Image.IsRelocatable || (Image.Format != BinaryFormat::COFF &&
                              Image.Format != BinaryFormat::ELF &&
                              Image.Format != BinaryFormat::MachO))
    return false;
  if (Image.Format == BinaryFormat::MachO &&
      (Image.MachOChainedFixupsAmbiguous ||
       (Image.MachOHasChainedFixups && Image.Bits != Bitness::Bits64)))
    return false;
  return (Image.Bits == Bitness::Bits32 &&
          (Image.Arch == Arch::X86 || Image.Arch == Arch::ARM)) ||
         (Image.Bits == Bitness::Bits64 &&
          (Image.Arch == Arch::X64 || Image.Arch == Arch::AArch64));
}

std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (!supportsImmutableImageReads(Image) ||
      Size > ::neverd::limits::loader::kMaxImageReadBytes)
    return std::nullopt;
  // A zero-length borrow still requires a valid nonnull object address.
  const uint64_t Extent = Size ? Size : 1;
  const auto *Bytes = mappedBytes(Image, Address, Extent, true);
  if (!Bytes || hasConflictingFixups(Image, Address, Extent, false))
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

std::optional<std::vector<uint8_t>>
readImmutableImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size,
                        size_t &Work, size_t Limit) {
  if (!chargeStorageQuery(Image, Size, false, Work, Limit))
    return std::nullopt;
  return readImmutableImageBytes(Image, Address, Size);
}

std::optional<std::vector<uint8_t>>
readInitialImageBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (!supportedImage(Image) || !Size ||
      Size > ::neverd::limits::loader::kMaxImageReadBytes)
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, Size, false);
  if (!Bytes || hasConflictingFixups(Image, Address, Size, false))
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

std::optional<std::vector<uint8_t>>
readImmutableCodeBytes(const BinaryImage &Image, va_t Address, uint32_t Size) {
  if (!supportedImmutableCodeImage(Image) || !Size ||
      Size > ::neverd::limits::loader::kMaxImageReadBytes)
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, Size, true, true);
  if (!Bytes || hasConflictingFixups(Image, Address, Size, false))
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}

bool isFileBackedCodeImageRange(const BinaryImage &Image, va_t Address,
                                uint32_t Size) {
  return supportsImmutableImageReads(Image) && Size &&
         Size <= ::neverd::limits::loader::kMaxImageReadBytes &&
         mappedBytes(Image, Address, Size, false, true);
}

namespace {
using PE32CodeFixup = read_only_detail::PE32CodeFixup;

template <typename Visitor>
bool visitPE32CodeFixups(
    const BinaryImage &Image, Visitor Visit,
    const std::optional<ExceptionAddressRange> &Range = std::nullopt) {
  using Kind = PE32CodeFixup::Kind;
  const auto Ordered = [&](const auto &Items, uint64_t Width, auto Key,
                           auto Make) {
    const va_t First =
        Range && Range->Begin >= Width - 1 ? Range->Begin - (Width - 1) : 0;
    auto It = Range ? Items.lower_bound(First) : Items.begin();
    for (; It != Items.end() && (!Range || Key(*It) < Range->End); ++It)
      if (!Visit(Make(*It)))
        return false;
    return true;
  };
  for (const auto &R : Image.BaseRelocations)
    if (R.Type &&
        !Visit(PE32CodeFixup{
            Kind::Base, R.Address,
            uint8_t(R.Type == llvm::COFF::IMAGE_REL_BASED_HIGHLOW ? 4 : 8),
            R.Type}))
      return false;
  for (const auto &R : Image.Relocations)
    if (!Visit(PE32CodeFixup{Kind::Forbidden, R.Address, 8}))
      return false;
  for (const auto *Fields :
       {&Image.CodeAddressRelocOperands, &Image.DataAddressRelocOperands})
    if (!Ordered(
            *Fields, UINT8_MAX, [](const auto &Item) { return Item.first; },
            [](const auto &Item) {
              return PE32CodeFixup{Kind::Operand, Item.first, Item.second.Width,
                                   0, &Item.second};
            }))
      return false;
  for (const auto *Slots :
       {&Image.CodePtrRelocSlots, &Image.DataPtrRelocSlots,
        &Image.RelCodeRelocSlots, &Image.RelDataPtrRelocSlots})
    if (!Ordered(
            *Slots, 4, [](va_t Slot) { return Slot; },
            [](va_t Slot) { return PE32CodeFixup{Kind::Forbidden, Slot, 4}; }))
      return false;
  for (const auto &Slot : Image.RuntimeCallablePointerSlots)
    if (!Visit(PE32CodeFixup{Kind::Forbidden, Slot.SlotVA, 4}))
      return false;
  return true;
}

template <typename FixupQuery>
std::optional<std::vector<uint8_t>>
readPE32CodeBytes(const BinaryImage &Image, va_t Address, uint32_t Size,
                  const std::vector<va_t> &AbsoluteOperands, FixupQuery Query,
                  bool RequireRelocations = false) {
  if (Image.Format != BinaryFormat::COFF || Image.IsRelocatable ||
      Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 || !Size ||
      Size > ::neverd::limits::loader::kMaxImageReadBytes ||
      Address > UINT32_MAX || Size > uint64_t(UINT32_MAX) + 1 - Address)
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, Size, true, true);
  if (!Bytes)
    return std::nullopt;
  std::set<va_t> Operands;
  for (va_t Operand : AbsoluteOperands)
    if (Operand < Address || Operand - Address > Size ||
        Size - (Operand - Address) < 4 || !Operands.insert(Operand).second)
      return std::nullopt;
  for (auto It = Operands.begin(); It != Operands.end(); ++It)
    if (It != Operands.begin() && *It - *std::prev(It) < 4)
      return std::nullopt;
  std::set<va_t> Relocated;
  std::set<va_t> FieldSlots;
  if (!Query([&](const PE32CodeFixup &Fixup) {
        if (!overlaps(Address, Size, Fixup.Address, Fixup.Width))
          return true;
        using Kind = PE32CodeFixup::Kind;
        if (Fixup.Type == Kind::Base)
          return Fixup.BaseType == llvm::COFF::IMAGE_REL_BASED_HIGHLOW &&
                 Operands.count(Fixup.Address) &&
                 Relocated.insert(Fixup.Address).second;
        if (Fixup.Type == Kind::Forbidden)
          return false;
        const auto &Field = *Fixup.Field;
        if (!Operands.count(Fixup.Address) || Field.Width != 4 ||
            Field.PCRelativeFromInstructionEnd ||
            Field.Kind != RelocatedAddressFieldKind::Generic ||
            Field.TargetVA != llvm::support::endian::read32le(
                                  Bytes + (Fixup.Address - Address)))
          return false;
        FieldSlots.insert(Fixup.Address);
        return true;
      }))
    return std::nullopt;
  // Sorted indexes may visit a normalized operand before its HIGHLOW entry.
  // Both inventories still have to name exactly the same current field.
  for (va_t Slot : FieldSlots)
    if (!Relocated.count(Slot))
      return std::nullopt;
  if (RequireRelocations && Relocated != Operands)
    return std::nullopt;
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}
} // namespace

std::optional<std::vector<uint8_t>>
readImmutablePE32CodeBytes(const BinaryImage &Image, va_t Address,
                           uint32_t Size,
                           const std::vector<va_t> &AbsoluteOperands) {
  return readPE32CodeBytes(
      Image, Address, Size, AbsoluteOperands, [&](auto Visit) {
        // The validator has checked this extent. Ordered inventories only
        // need the overlapping window, including a wide malformed field
        // beginning before it, just as in the budgeted reader below.
        return visitPE32CodeFixups(
            Image, Visit, ExceptionAddressRange{Address, Address + Size});
      });
}

namespace {
std::optional<std::vector<uint8_t>>
readBudgetedPE32CodeBytes(const BinaryImage &Image, va_t Address, uint32_t Size,
                          const std::vector<va_t> &AbsoluteOperands,
                          size_t &Work, size_t Limit, bool RequireRelocations) {
  // The scalar storage charge already bounds mapping scans, all vector
  // inventories and the four-byte ordered slot windows used by code queries.
  if (!chargeStorageQuery(Image, Size, false, Work, Limit))
    return std::nullopt;
  const auto Charge = [&](size_t Amount) {
    if (Amount > Limit - std::min(Work, Limit)) {
      Work = Limit;
      return false;
    }
    Work += Amount;
    return true;
  };
  // Normalized instruction fields have an explicit uint8_t width, unlike
  // data-pointer slots. Include malformed wide fields preceding this range.
  for (size_t Count : {Image.CodeAddressRelocOperands.size(),
                       Image.DataAddressRelocOperands.size()})
    if (!Charge(
            size_t(std::min<uint64_t>(Count, uint64_t(Size) + UINT8_MAX - 1))))
      return std::nullopt;
  // Operand validation and the relocated/normalized field sets use balanced
  // trees. Successful insertions are bounded by the supplied operand count;
  // one additional conflicting entry can terminate the query.
  const size_t SetWork =
      6 * (2 * std::bit_width(AbsoluteOperands.size()) + 1) + 6;
  if (AbsoluteOperands.size() > (Limit - std::min(Work, Limit)) / SetWork) {
    Work = Limit;
    return std::nullopt;
  }
  if (!Charge(AbsoluteOperands.size() * SetWork) || !Charge(SetWork))
    return std::nullopt;
  return readPE32CodeBytes(
      Image, Address, Size, AbsoluteOperands,
      [&](auto Visit) {
        return visitPE32CodeFixups(
            Image, Visit, ExceptionAddressRange{Address, Address + Size});
      },
      RequireRelocations);
}
} // namespace

std::optional<std::vector<uint8_t>> readRelocatedPE32CodeBytes(
    const BinaryImage &Image, va_t Address, uint32_t Size,
    const std::vector<va_t> &AbsoluteOperands, size_t &Work, size_t Limit) {
  return readBudgetedPE32CodeBytes(Image, Address, Size, AbsoluteOperands, Work,
                                   Limit, true);
}

std::optional<std::vector<uint8_t>> readImmutablePE32CodeBytes(
    const BinaryImage &Image, va_t Address, uint32_t Size,
    const std::vector<va_t> &AbsoluteOperands, size_t &Work, size_t Limit) {
  return readBudgetedPE32CodeBytes(Image, Address, Size, AbsoluteOperands, Work,
                                   Limit, false);
}

bool IndexedPE32CodeBytes::charge(size_t Amount) {
  if (Amount > Limit - std::min(Work, Limit)) {
    Work = Limit;
    Valid = false;
    return false;
  }
  Work += Amount;
  return true;
}

IndexedPE32CodeBytes::IndexedPE32CodeBytes(const BinaryImage &Image,
                                           size_t &Work, size_t Limit)
    : Image(Image), Work(Work), Limit(Limit) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Image.IsRelocatable)
    return;
  const size_t Count =
      Image.BaseRelocations.size() + Image.Relocations.size() +
      Image.CodeAddressRelocOperands.size() +
      Image.DataAddressRelocOperands.size() + Image.CodePtrRelocSlots.size() +
      Image.DataPtrRelocSlots.size() + Image.RelCodeRelocSlots.size() +
      Image.RelDataPtrRelocSlots.size() +
      Image.RuntimeCallablePointerSlots.size();
  size_t SortWork = 2;
  for (size_t N = Count; N > 1; N = N / 2 + N % 2)
    ++SortWork;
  if (Count > (Limit - std::min(Work, Limit)) / SortWork) {
    Work = Limit;
    return;
  }
  if (!charge(Count * SortWork))
    return;
  Fixups.reserve(Count);
  visitPE32CodeFixups(Image, [&](const PE32CodeFixup &Fixup) {
    MaximumWidth = std::max(MaximumWidth, Fixup.Width);
    Fixups.push_back(Fixup);
    return true;
  });
  std::sort(Fixups.begin(), Fixups.end(),
            [](const auto &A, const auto &B) { return A.Address < B.Address; });
  Valid = true;
}

std::optional<std::vector<uint8_t>>
IndexedPE32CodeBytes::read(va_t Address, uint32_t Size,
                           const std::vector<va_t> &AbsoluteOperands) {
  size_t LookupWork = 1;
  for (size_t N = Fixups.size(); N > 1; N = N / 2 + N % 2)
    ++LookupWork;
  if (!Valid ||
      !charge(LookupWork + Size + AbsoluteOperands.size() +
              2 * (Image.Segments.size() + Image.Sections.size()) + 1))
    return std::nullopt;
  return readPE32CodeBytes(
      Image, Address, Size, AbsoluteOperands, [&](auto Visit) {
        const va_t First =
            Address >= MaximumWidth - 1 ? Address - (MaximumWidth - 1) : 0;
        auto It = std::lower_bound(
            Fixups.begin(), Fixups.end(), First,
            [](const auto &F, va_t A) { return F.Address < A; });
        for (; It != Fixups.end() && It->Address < Address + Size; ++It)
          if (!charge(1) || !Visit(*It))
            return false;
        return true;
      });
}

std::optional<uint64_t> readImmutableChainedImageValue(const BinaryImage &Image,
                                                       va_t Address) {
  if (!supportedImage(Image) || !Image.MachOHasChainedFixups ||
      !Image.MachOResolvedChainedPointerSlots.count(Address))
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, 8, true);
  if (!Bytes || hasConflictingFixups(Image, Address, 8, true))
    return std::nullopt;
  return llvm::support::endian::read64le(Bytes);
}

bool isImagePointerBitPattern(const BinaryImage &Image, uint64_t Bits,
                              uint16_t Width) {
  return Bits && Width == (Image.is64Bit() ? 8 : 4) &&
         Image.getSectionFor(Bits);
}

bool isFileBackedWritableImageRange(const BinaryImage &Image, va_t Address,
                                    uint32_t Size) {
  if ((!supportedImage(Image) && !supportedPEImage(Image)) || !Size ||
      Size > ::neverd::limits::loader::kMaxImageReadBytes ||
      !mappedBytes(Image, Address, Size, false))
    return false;
  const auto *Section = Image.getSectionFor(Address);
  const auto *Segment = Image.getSegmentFor(Address);
  return Section->isWritable() && !Section->isExecutable() &&
         Segment->isWritable() && !Segment->isExecutable() &&
         !Segment->ReadOnlyAfterRelocations;
}

namespace {
bool immutableLocalCodeTarget(const BinaryImage &Image, va_t Target) {
  return Image.Arch != Arch::ARM &&
         (Image.Arch != Arch::AArch64 || Target % 4 == 0) &&
         Image.hasAuthenticatedFunctionEntryAt(Target) &&
         !Image.findImportStubAt(Target) &&
         readImmutableCodeBytes(Image, Target,
                                Image.Arch == Arch::AArch64 ? 4 : 1)
             .has_value();
}

std::optional<va_t> readResolvedPointer(const BinaryImage &Image, va_t Address,
                                        bool Immutable,
                                        bool SelectorReference = false) {
  if (!supportedImage(Image) || !Image.DataPtrRelocSlots.count(Address) ||
      (Image.MachOHasChainedFixups &&
       !Image.MachOResolvedChainedPointerSlots.count(Address)))
    return std::nullopt;
  const auto Owner = Image.DataPtrRelocTargetOwners.find(Address);
  const auto *Bytes = mappedBytes(Image, Address, 8, Immutable);
  if (!Bytes || Owner == Image.DataPtrRelocTargetOwners.end() ||
      hasConflictingFixups(Image, Address, 8, true, false, SelectorReference))
    return std::nullopt;
  const auto Target = llvm::support::endian::read64le(Bytes);
  if (!mappedBytes(Image, Target, 1, false) ||
      Image.getSectionFor(Target)->VA != Owner->second)
    return std::nullopt;
  return Target;
}
std::optional<va_t> readImmutablePEPointerStorage(const BinaryImage &Image,
                                                  va_t Address, bool Code,
                                                  bool ExecutableSlot = false) {
  if (supportedPEImage(Image)) {
    const unsigned Width = Image.getPointerSize();
    const auto &Slots =
        Code ? Image.CodePtrRelocSlots : Image.DataPtrRelocSlots;
    if ((Width != 4 && Width != 8) || !Slots.count(Address))
      return std::nullopt;
    const unsigned Type = Width == 8 ? llvm::COFF::IMAGE_REL_BASED_DIR64
                                     : llvm::COFF::IMAGE_REL_BASED_HIGHLOW;
    unsigned ExactRelocations = 0;
    for (const auto &Relocation : Image.BaseRelocations)
      if (Relocation.Address == Address) {
        if (Relocation.Type != Type || ++ExactRelocations != 1)
          return std::nullopt;
      }
    if (ExactRelocations != 1)
      return std::nullopt;
    const auto *Bytes =
        mappedBytes(Image, Address, Width, true, ExecutableSlot);
    const auto Owner = Image.DataPtrRelocTargetOwners.find(Address);
    if (!Bytes || (!Code && Owner == Image.DataPtrRelocTargetOwners.end()) ||
        hasConflictingFixups(Image, Address, Width, !Code, false, false, Code,
                             true))
      return std::nullopt;
    const uint64_t Target = Width == 8 ? llvm::support::endian::read64le(Bytes)
                                       : llvm::support::endian::read32le(Bytes);
    if (!mappedBytes(Image, Target, 1, false, Code) ||
        (!Code && Image.getSectionFor(Target)->VA != Owner->second))
      return std::nullopt;
    return Target;
  }
  return std::nullopt;
}
} // namespace

std::optional<va_t>
readImmutablePE32CodePointerStorage(const BinaryImage &Image, va_t Address) {
  if (Image.Arch != Arch::X86 || Image.Bits != Bitness::Bits32 ||
      Image.Format != BinaryFormat::COFF || Image.Base > UINT32_MAX ||
      Address > UINT32_MAX - 3)
    return std::nullopt;
  const auto Target = readImmutablePEPointerStorage(Image, Address, true);
  if (!Target || !readImmutableCodeBytes(Image, *Target, 1))
    return std::nullopt;
  return Target;
}

std::optional<va_t>
readImmutablePE32CodePointerStorage(const BinaryImage &Image, va_t Address,
                                    size_t &Work, size_t Limit) {
  if (!chargeStorageQuery(Image, 4, true, Work, Limit))
    return std::nullopt;
  return readImmutablePE32CodePointerStorage(Image, Address);
}

std::optional<va_t> readImmutablePE32JumpTableTarget(const BinaryImage &Image,
                                                     va_t Address, size_t &Work,
                                                     size_t Limit) {
  if (Image.Base > UINT32_MAX || Address > UINT32_MAX - 3 ||
      !chargeStorageQuery(Image, 4, true, Work, Limit))
    return std::nullopt;
  for (unsigned I = 0; I != 2; ++I)
    for (size_t Count : {Image.Sections.size(), Image.Segments.size()}) {
      if (Count > Limit - std::min(Work, Limit)) {
        Work = Limit;
        return std::nullopt;
      }
      Work += Count;
    }
  // Select the storage role without relaxing either reader's mapping/fixup
  // policy; the additional owner lookup is prepaid above.
  const auto Target = readImmutablePEPointerStorage(
      Image, Address, true, Image.isCodeAddress(Address));
  return Target && readImmutableCodeBytes(Image, *Target, 1) ? Target
                                                             : std::nullopt;
}

std::optional<va_t> readImmutableImagePointer(const BinaryImage &Image,
                                              va_t Address) {
  if (supportedPEImage(Image))
    return readImmutablePEPointerStorage(Image, Address, false);
  return readResolvedPointer(Image, Address, true);
}

std::optional<va_t> readImmutableImageCodePointer(const BinaryImage &Image,
                                                  va_t Address) {
  if (supportedPEImage(Image)) {
    const auto Target = readImmutablePEPointerStorage(Image, Address, true);
    if (!Target || !immutableLocalCodeTarget(Image, *Target))
      return std::nullopt;
    return Target;
  }
  if (!supportedImage(Image) || Address % 8 || !Image.MachOHasChainedFixups ||
      !Image.CodePtrRelocSlots.count(Address) ||
      !Image.MachOResolvedChainedPointerSlots.count(Address))
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, 8, true);
  if (!Bytes ||
      hasConflictingFixups(Image, Address, 8, false, false, false, true))
    return std::nullopt;
  const auto Target = llvm::support::endian::read64le(Bytes);
  if (!immutableLocalCodeTarget(Image, Target))
    return std::nullopt;
  return Target;
}

namespace {
bool imageImportSlot(const BinaryImage &Image, va_t Address, bool Immutable,
                     bool ClassReference) {
  if (!supportedImage(Image) || !mappedBytes(Image, Address, 8, Immutable) ||
      hasConflictingFixups(Image, Address, 8, false, true, ClassReference))
    return false;
  const auto Bind = Image.DyldBindSlots.find(Address);
  if (Bind == Image.DyldBindSlots.end() || Bind->second.Name.empty() ||
      Bind->second.Module.empty() || Bind->second.WeakImport ||
      Bind->second.Addend ||
      !Image.isValidImportStorageSlot(Address, Bind->second.Name))
    return false;
  const auto Storage = Image.collectImportStorageSlot(Address);
  const auto Slot = Storage.Slots.find(Address);
  return !Storage.Conflicts.count(Address) && Slot != Storage.Slots.end() &&
         Slot->second.Name == Bind->second.Name && !Slot->second.Addend;
}
} // namespace

bool isImmutableImageImportSlot(const BinaryImage &Image, va_t Address) {
  return imageImportSlot(Image, Address, true, false);
}

bool isInitialImageImportSlot(const BinaryImage &Image, va_t Address) {
  return imageImportSlot(Image, Address, false, false);
}

bool isFileBackedPEImportSlot(const BinaryImage &Image, va_t Address) {
  const auto Width = Image.getPointerSize();
  if (!supportedPEImage(Image) || !Width || Address % Width ||
      (Width == 4 && Address > uint64_t(UINT32_MAX) - (Width - 1)) ||
      !mappedBytes(Image, Address, Width, false) ||
      hasConflictingFixups(Image, Address, Width, false, true))
    return false;
  const auto *Imported = Image.findImportAt(Address);
  if (!Imported || Imported->IATAddr != Address || Imported->IsDelayImport ||
      Imported->Name.empty() || Imported->Module.empty() ||
      !Image.isValidImportStorageSlot(Address, Imported->Name))
    return false;
  for (const auto &Other : Image.Imports)
    if (Other.IATAddr == Address &&
        (Other.Name != Imported->Name || Other.IsDelayImport ||
         !llvm::StringRef(Other.Module).equals_insensitive(Imported->Module)))
      return false;
  const auto Storage = Image.collectImportStorageSlot(Address);
  const auto Slot = Storage.Slots.find(Address);
  return !Storage.Conflicts.count(Address) && Slot != Storage.Slots.end() &&
         Slot->second.Name == Imported->Name && !Slot->second.Addend &&
         Slot->second.Evidence == ImportStorageEvidence::ImportDirectory;
}

std::optional<uint64_t> readImmutableImageIvarOffset(const BinaryImage &Image,
                                                     va_t Address) {
  const auto Ref = Image.ObjCSourceReferences.find(Address);
  const auto *Section = Image.getSectionFor(Address);
  if (!supportedImage(Image) || Address % 8 || !Section ||
      (Section->Type & llvm::MachO::SECTION_TYPE) != llvm::MachO::S_REGULAR ||
      Ref == Image.ObjCSourceReferences.end() ||
      Ref->second.TheKind != ObjCSourceReference::Kind::IvarOffset ||
      Ref->second.Address != Address || Ref->second.Size != 8 ||
      Ref->second.Name.empty() || Ref->second.ClassName.empty())
    return std::nullopt;
  const auto *Bytes = mappedBytes(Image, Address, 8, true);
  if (!Bytes || hasConflictingFixups(Image, Address, 8, false, false, true))
    return std::nullopt;
  return llvm::support::endian::read64le(Bytes);
}

bool isImmutableImageClassImportSlot(const BinaryImage &Image, va_t Address) {
  const auto Ref = Image.ObjCSourceReferences.find(Address);
  const auto Bind = Image.DyldBindSlots.find(Address);
  if (Address % 8 || Ref == Image.ObjCSourceReferences.end() ||
      Bind == Image.DyldBindSlots.end() || Ref->second.Address != Address ||
      Ref->second.Size != 8 || Ref->second.Name.empty() ||
      Ref->second.TheKind != ObjCSourceReference::Kind::Class ||
      Bind->second.Name != "_OBJC_CLASS_$_" + Ref->second.Name)
    return false;
  return imageImportSlot(Image, Address, true, true);
}

std::optional<va_t> readInitialImagePointer(const BinaryImage &Image,
                                            va_t Address) {
  return readResolvedPointer(Image, Address, false);
}
std::optional<va_t> readInitialImageSelectorPointer(const BinaryImage &Image,
                                                    va_t Address) {
  const auto R = Image.ObjCSourceReferences.find(Address);
  if (Image.Format != BinaryFormat::MachO || Address % 8 ||
      R == Image.ObjCSourceReferences.end() || R->second.Address != Address ||
      R->second.Size != 8 || R->second.Name.empty() ||
      R->second.TheKind != ObjCSourceReference::Kind::Selector)
    return std::nullopt;
  return readResolvedPointer(Image, Address, false, true);
}
} // namespace neverd
