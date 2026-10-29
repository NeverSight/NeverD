#include "neverd/loader/ReadOnlyBytes.h"

#include "ReadOnlyBytesIndex.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/Limits.h"
#include "neverd/object/ELFLayout.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <set>

namespace neverd {

bool BinaryImage::readOnlyAfterRelocations(va_t Addr, uint64_t Width) const {
  const Segment *Seg = getSegmentFor(Addr);
  if (!Seg || !Width || Seg->Size > InvalidVA - Seg->VA ||
      Width > Seg->Size - (Addr - Seg->VA))
    return false;
  if (!Seg->isWritable() || Seg->ReadOnlyAfterRelocations)
    return true;
  // Names describe linker conventions, not runtime protection. In particular,
  // -z norelro leaves .data.rel.ro writable and a Mach-O __DATA_CONST mapping
  // without the loader's SG_READ_ONLY guarantee need not become immutable.
  if (Format != BinaryFormat::ELF || IsRelocatable || !ELFMetadata)
    return false;
  // GNU_RELRO is a dynamic-loader action, not a kernel PT_LOAD permission.
  // An executable without an interpreter may never protect it; a DSO has no
  // independent process entry. Explicit loader guarantees above remain valid.
  const bool DynamicImage =
      (ELFMetadata->Type == llvm::ELF::ET_DYN && !Entry) ||
      ((ELFMetadata->Type == llvm::ELF::ET_DYN ||
        ELFMetadata->Type == llvm::ELF::ET_EXEC) &&
       !ELFMetadata->Interpreter.empty());
  const uint64_t Page = maximumELFProtectionPageSize(Arch);
  if (!DynamicImage || !Page)
    return false;
  for (const ELFProgramHeader &Header : ELFMetadata->ProgramHeaders) {
    if (Header.Type != llvm::ELF::PT_GNU_RELRO ||
        Header.MemorySize > InvalidVA - Header.VirtualAddress)
      continue;
    // Relocation protection excludes an incomplete final page. Use the
    // target's largest possible granule, never this analysis host's page.
    const va_t End = (Header.VirtualAddress + Header.MemorySize) & ~(Page - 1);
    if (Addr >= Header.VirtualAddress && Addr < End && Width <= End - Addr)
      return true;
  }
  return false;
}

namespace {
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
      (Immutable && !Image.readOnlyAfterRelocations(Address, Extent)) ||
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
  auto TouchesOperands = [&](const auto &Operands) {
    // Operand widths are uint8_t, not necessarily the target's pointer width.
    // In particular, malformed wide metadata cannot hide an overlapping tail
    // before the lower_bound. Preserve the existing pointer-width exclusion
    // for narrow/incomplete records while covering every wider field.
    constexpr uint64_t MaximumWidth = UINT8_MAX;
    auto It = Operands.lower_bound(
        Address >= MaximumWidth - 1 ? Address - (MaximumWidth - 1) : 0);
    for (; It != Operands.end() &&
           (It->first < Address || It->first - Address < Extent);
         ++It)
      if (overlaps(Address, Extent, It->first,
                   std::max<uint64_t>(PointerWidth, It->second.Width)))
        return true;
    return false;
  };
  if (Touches(Image.CodePtrRelocSlots, Set, CodePointer) ||
      Touches(Image.MachOAuthenticatedPointers, Map) ||
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
      TouchesOperands(Image.DataAddressRelocOperands) ||
      TouchesOperands(Image.CodeAddressRelocOperands))
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

std::optional<uint64_t> readImmutableImageScalar(const BinaryImage &Image,
                                                 va_t Address, uint32_t Size,
                                                 size_t *RemainingWork) {
  if (!Size || Size > sizeof(uint64_t) || !supportsImmutableImageReads(Image))
    return std::nullopt;
  if (RemainingWork) {
    size_t Available = *RemainingWork;
    // Charge inventories even when a mapping/fixup check could return early.
    // This bounds cold queries independently of which record happens to match
    // and includes the ordered indices used by the pointer-candidate check.
    for (size_t Work :
         {size_t{1} + Size,
          Image.Sections.size(),
          Image.Segments.size(),
          Image.ELFMetadata ? Image.ELFMetadata->ProgramHeaders.size()
                            : size_t{0},
          Image.ARMCodeRegions.size(),
          Image.Relocations.size(),
          Image.BaseRelocations.size(),
          Image.RuntimeCallablePointerSlots.size(),
          Image.CodePtrRelocSlots.size(),
          Image.MachOAuthenticatedPointers.size(),
          Image.DataPtrRelocSlots.size(),
          Image.DataPtrRelocTargetOwners.size(),
          Image.RelCodeRelocSlots.size(),
          Image.RelDataPtrRelocSlots.size(),
          Image.MachOResolvedChainedPointerSlots.size(),
          Image.ConflictingImportStorageSlots.size(),
          Image.ImportPtrSlots.size(),
          Image.ImportStorageSlots.size(),
          Image.DyldBindSlots.size(),
          Image.ObjCSourceReferences.size(),
          Image.DataAddressRelocOperands.size(),
          Image.CodeAddressRelocOperands.size(),
          Image.RelocDataAddrs.size(),
          Image.WritableRelocDataAddrs.size(),
          Image.RodataAnchorSeg.size(),
          Image.CodeRefTargets.size()}) {
      if (Work > Available)
        return std::nullopt;
      Available -= Work;
    }
    // Pointer-candidate checks can query section/segment ownership for each
    // segment end. Reserve those nested inventories without overflowing.
    if (Size == Image.getPointerSize())
      for (size_t Inventory : {Image.Sections.size(), Image.Segments.size()}) {
        if (Inventory && Image.Segments.size() > Available / Inventory)
          return std::nullopt;
        Available -= Inventory * Image.Segments.size();
      }
    *RemainingWork = Available;
  }
  auto Bytes = readImmutableImageBytes(Image, Address, Size);
  // A real LOAD may read scalar constants placed among instructions. Keep
  // the code reader's exact permissions, mapping and no-fixup requirements.
  if (!Bytes)
    Bytes = readImmutableCodeBytes(Image, Address, Size);
  if (!Bytes)
    return std::nullopt;
  uint64_t Bits = 0;
  for (unsigned I = 0; I < Size; ++I)
    Bits |= uint64_t{(*Bytes)[I]} << (I * 8);
  if (Size == Image.getPointerSize() &&
      Image.isPotentiallyRelocatableAddress(Bits))
    return std::nullopt;
  return Bits;
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
bool visitPE32CodeFixups(const BinaryImage &Image, Visitor Visit) {
  using Kind = PE32CodeFixup::Kind;
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
    for (const auto &[Slot, Field] : *Fields)
      if (!Visit(PE32CodeFixup{Kind::Operand, Slot, Field.Width, 0, &Field}))
        return false;
  for (const auto *Slots :
       {&Image.CodePtrRelocSlots, &Image.DataPtrRelocSlots,
        &Image.RelCodeRelocSlots, &Image.RelDataPtrRelocSlots})
    for (va_t Slot : *Slots)
      if (!Visit(PE32CodeFixup{Kind::Forbidden, Slot, 4}))
        return false;
  for (const auto &Slot : Image.RuntimeCallablePointerSlots)
    if (!Visit(PE32CodeFixup{Kind::Forbidden, Slot.SlotVA, 4}))
      return false;
  return true;
}

template <typename FixupQuery>
std::optional<std::vector<uint8_t>>
readPE32CodeBytes(const BinaryImage &Image, va_t Address, uint32_t Size,
                  const std::vector<va_t> &AbsoluteOperands, FixupQuery Query) {
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
  return std::vector<uint8_t>(Bytes, Bytes + Size);
}
} // namespace

std::optional<std::vector<uint8_t>>
readImmutablePE32CodeBytes(const BinaryImage &Image, va_t Address,
                           uint32_t Size,
                           const std::vector<va_t> &AbsoluteOperands) {
  return readPE32CodeBytes(
      Image, Address, Size, AbsoluteOperands,
      [&](auto Visit) { return visitPE32CodeFixups(Image, Visit); });
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
std::optional<va_t> readImmutablePEPointer(const BinaryImage &Image,
                                           va_t Address, bool Code) {
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
    const auto *Bytes = mappedBytes(Image, Address, Width, true);
    const auto Owner = Image.DataPtrRelocTargetOwners.find(Address);
    if (!Bytes || (!Code && Owner == Image.DataPtrRelocTargetOwners.end()) ||
        hasConflictingFixups(Image, Address, Width, !Code, false, false, Code,
                             true))
      return std::nullopt;
    const uint64_t Target = Width == 8 ? llvm::support::endian::read64le(Bytes)
                                       : llvm::support::endian::read32le(Bytes);
    if (!mappedBytes(Image, Target, 1, false, Code) ||
        (!Code && Image.getSectionFor(Target)->VA != Owner->second) ||
        (Code && !immutableLocalCodeTarget(Image, Target)))
      return std::nullopt;
    return Target;
  }
  return std::nullopt;
}
} // namespace

std::optional<va_t> readImmutableImagePointer(const BinaryImage &Image,
                                              va_t Address) {
  if (supportedPEImage(Image))
    return readImmutablePEPointer(Image, Address, false);
  return readResolvedPointer(Image, Address, true);
}

std::optional<va_t> readImmutableImageCodePointer(const BinaryImage &Image,
                                                  va_t Address) {
  if (supportedPEImage(Image))
    return readImmutablePEPointer(Image, Address, true);
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
