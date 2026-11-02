//===- MachORelocations.cpp - Mach-O object relocations ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachOObjectExterns.h"
#include "MachORelocationsDetail.h"

#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/MachO/MachOArm64e.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#define DEBUG_TYPE "neverd-macho-relocations"

namespace neverd::macho_loader {

namespace {

using llvm::object::MachOObjectFile;
using llvm::object::RelocationRef;

struct RelocationMetadata {
  uint64_t Address = 0;
  uint32_t Type = 0;
  uint32_t Length = 0;
  uint32_t SymbolNumber = 0;
  bool IsPCRel = false;
  bool IsExternal = false;
  bool IsScattered = false;
};

struct ResolvedSymbol {
  uint64_t Address = 0;
  uint64_t OwnerVA = InvalidVA;
  bool IsThumb = false;
};

struct MappedObjectAddress {
  uint64_t Address = 0;
  uint64_t OwnerVA = InvalidVA;
};

std::optional<uint8_t>
symbolSectionNumber(const llvm::object::MachOObjectFile &Obj,
                    llvm::object::symbol_iterator SymIt) {
  if (SymIt == Obj.symbol_end())
    return std::nullopt;
  llvm::object::DataRefImpl DRI = SymIt->getRawDataRefImpl();
  uint8_t Type = 0;
  uint8_t SectionNumber = 0;
  if (Obj.is64Bit()) {
    const llvm::MachO::nlist_64 Entry = Obj.getSymbol64TableEntry(DRI);
    Type = Entry.n_type & llvm::MachO::N_TYPE;
    SectionNumber = Entry.n_sect;
  } else {
    const llvm::MachO::nlist Entry = Obj.getSymbolTableEntry(DRI);
    Type = Entry.n_type & llvm::MachO::N_TYPE;
    SectionNumber = Entry.n_sect;
  }
  if (Type != llvm::MachO::N_SECT || SectionNumber == 0)
    return std::nullopt;
  return SectionNumber;
}

RelocationMetadata relocationMetadata(const MachOObjectFile &Obj,
                                      const RelocationRef &Reloc) {
  const auto Info = Obj.getRelocation(Reloc.getRawDataRefImpl());
  RelocationMetadata Result;
  Result.Address = Obj.getAnyRelocationAddress(Info);
  Result.Type = Obj.getAnyRelocationType(Info);
  Result.Length = Obj.getAnyRelocationLength(Info);
  Result.IsPCRel = Obj.getAnyRelocationPCRel(Info);
  Result.IsScattered = Obj.isRelocationScattered(Info);
  if (!Result.IsScattered) {
    Result.SymbolNumber = Obj.getPlainRelocationSymbolNum(Info);
    Result.IsExternal = Obj.getPlainRelocationExternal(Info);
  }
  return Result;
}

std::optional<ResolvedSymbol>
resolveExternalSymbol(const MachOObjectFile &Obj, const RelocationRef &Reloc,
                      const BinaryImage &Img, const ObjectExterns &Externs,
                      bool Branch) {
  auto SymIt = Reloc.getSymbol();
  if (SymIt == Obj.symbol_end())
    return std::nullopt;
  const std::optional<uint8_t> SectionNumber = symbolSectionNumber(Obj, SymIt);
  if (!SectionNumber) {
    // An undefined or common symbol resolves to its extern address.
    if (const auto Extern = resolveObjectExtern(Obj, Externs, SymIt, Branch))
      return ResolvedSymbol{Extern->first, Extern->second};
    return std::nullopt;
  }
  if (*SectionNumber > Img.Sections.size())
    return std::nullopt;
  auto AddrOrErr = SymIt->getAddress();
  if (!AddrOrErr) {
    llvm::consumeError(AddrOrErr.takeError());
    return std::nullopt;
  }
  const Section &Owner = Img.Sections[*SectionNumber - 1];
  if (Owner.Size > InvalidVA - Owner.VA || *AddrOrErr < Owner.VA ||
      *AddrOrErr > Owner.VA + Owner.Size)
    return std::nullopt;
  llvm::object::DataRefImpl DRI = SymIt->getRawDataRefImpl();
  const uint16_t Description = Obj.is64Bit()
                                   ? Obj.getSymbol64TableEntry(DRI).n_desc
                                   : Obj.getSymbolTableEntry(DRI).n_desc;
  return ResolvedSymbol{*AddrOrErr, Owner.VA,
                        (Description & llvm::MachO::N_ARM_THUMB_DEF) != 0};
}

std::optional<ResolvedSymbol>
resolveRelocationTarget(const MachOObjectFile &Obj, const RelocationRef &Reloc,
                        const RelocationMetadata &Info, const BinaryImage &Img,
                        const ObjectExterns &Externs) {
  if (Info.IsScattered)
    return std::nullopt;
  if (Info.IsExternal)
    return resolveExternalSymbol(Obj, Reloc, Img, Externs,
                                 isBranchReference(Img.Arch, Info.Type));
  if (Info.SymbolNumber == 0 || Info.SymbolNumber > Img.Sections.size())
    return std::nullopt;
  const Section &Owner = Img.Sections[Info.SymbolNumber - 1];
  return ResolvedSymbol{Owner.VA, Owner.VA};
}

// A section-relative branch relocation names a section, not the function at
// its encoded addend. An exact defined code symbol supplies the interworking
// state used by the Mach-O linker; a nearby symbol does not.
std::optional<bool> thumbStateAtBranchTarget(const MachOObjectFile &Obj,
                                             uint32_t SectionNumber,
                                             uint64_t Target) {
  std::optional<bool> State;
  for (auto SymIt = Obj.symbol_begin(); SymIt != Obj.symbol_end(); ++SymIt) {
    if (symbolSectionNumber(Obj, SymIt) != SectionNumber)
      continue;
    const llvm::object::DataRefImpl DRI = SymIt->getRawDataRefImpl();
    if (Obj.getNValue(DRI) != Target)
      continue;
    const uint16_t Description = Obj.is64Bit()
                                     ? Obj.getSymbol64TableEntry(DRI).n_desc
                                     : Obj.getSymbolTableEntry(DRI).n_desc;
    const uint8_t Type = Obj.is64Bit() ? Obj.getSymbol64TableEntry(DRI).n_type
                                       : Obj.getSymbolTableEntry(DRI).n_type;
    if ((Description & llvm::MachO::N_ARM_THUMB_DEF) != 0)
      return true;
    if ((Type & llvm::MachO::N_EXT) != 0)
      State = false;
  }
  return State;
}

std::optional<MappedObjectAddress> mapObjectAddress(const MachOObjectFile &Obj,
                                                    const BinaryImage &Img,
                                                    uint64_t Address) {
  size_t SectionIndex = 0;
  for (const llvm::object::SectionRef &ObjectSec : Obj.sections()) {
    if (SectionIndex >= Img.Sections.size())
      return std::nullopt;
    const uint64_t OriginalBase = ObjectSec.getAddress();
    const uint64_t Size = ObjectSec.getSize();
    const Section &MappedSec = Img.Sections[SectionIndex++];
    if (Address < OriginalBase || Address - OriginalBase >= Size)
      continue;
    const uint64_t Offset = Address - OriginalBase;
    if (Offset > InvalidVA - MappedSec.VA)
      return std::nullopt;
    return MappedObjectAddress{MappedSec.VA + Offset, MappedSec.VA};
  }
  return std::nullopt;
}

void diagnoseRelocation(llvm::StringRef Reason, uint64_t SectionAddress,
                        uint64_t RelocationAddress) {
  LLVM_DEBUG(llvm::dbgs() << "macho: skip relocation at section "
                          << SectionAddress << '+' << RelocationAddress << ": "
                          << Reason << '\n');
  (void)Reason;
  (void)SectionAddress;
  (void)RelocationAddress;
}

llvm::Error relocationError(llvm::StringRef Reason, uint64_t SectionAddress,
                            uint64_t RelocationAddress) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("macho: relocation at section ") +
       llvm::Twine(SectionAddress) + "+" + llvm::Twine(RelocationAddress) +
       ": " + Reason)
          .str(),
      llvm::inconvertibleErrorCode());
}

llvm::Error recordARMRelocationMode(BinaryImage &Img, va_t Place,
                                    InstructionMode Mode,
                                    uint64_t SectionAddress,
                                    uint64_t RelocationAddress) {
  // An instruction relocation authenticates the encoding at its exact place,
  // even when no call reaches the containing function. It says nothing about
  // preceding bytes or an unflagged symbol elsewhere in the section.
  if (!Img.hasExecutableCodeOwnerRange(Place, 4))
    return llvm::Error::success();
  if (Img.ARMRequiredMode != InstructionMode::Default &&
      Img.ARMRequiredMode != Mode)
    return relocationError("ARM instruction relocation conflicts with "
                           "Thumb-only CPU subtype",
                           SectionAddress, RelocationAddress);
  const uint64_t Alignment = Mode == InstructionMode::Thumb ? 2 : 4;
  if ((Place & (Alignment - 1)) != 0)
    return relocationError("misaligned ARM/Thumb instruction relocation",
                           SectionAddress, RelocationAddress);
  const auto [It, Inserted] = Img.ARMCodeModeEntries.emplace(Place, Mode);
  if (!Inserted && It->second != Mode)
    return relocationError("conflicting ARM/Thumb instruction mode",
                           SectionAddress, RelocationAddress);
  return llvm::Error::success();
}

std::optional<uint64_t> addSigned(uint64_t Base, int64_t Addend) {
  if (Addend >= 0) {
    const uint64_t Magnitude = static_cast<uint64_t>(Addend);
    if (Base > std::numeric_limits<uint64_t>::max() - Magnitude)
      return std::nullopt;
    return Base + Magnitude;
  }
  const uint64_t Magnitude = uint64_t(-(Addend + 1)) + 1;
  if (Base < Magnitude)
    return std::nullopt;
  return Base - Magnitude;
}

std::optional<int64_t> signedDifference(uint64_t Left, uint64_t Right) {
  if (Left >= Right) {
    const uint64_t Difference = Left - Right;
    if (Difference > uint64_t(std::numeric_limits<int64_t>::max()))
      return std::nullopt;
    return static_cast<int64_t>(Difference);
  }
  const uint64_t Difference = Right - Left;
  if (Difference > uint64_t(std::numeric_limits<int64_t>::max()) + 1)
    return std::nullopt;
  if (Difference == uint64_t(std::numeric_limits<int64_t>::max()) + 1)
    return std::numeric_limits<int64_t>::min();
  return -static_cast<int64_t>(Difference);
}

bool isARM64InstructionRelocation(uint32_t Type) {
  using namespace llvm::MachO;
  return Type == ARM64_RELOC_BRANCH26 || Type == ARM64_RELOC_PAGE21 ||
         Type == ARM64_RELOC_PAGEOFF12;
}

bool hasValidARM64AddendMetadata(const RelocationMetadata &Info) {
  return !Info.IsScattered && !Info.IsPCRel && !Info.IsExternal &&
         Info.Length == 2;
}

bool hasValidARM64InstructionMetadata(const RelocationMetadata &Info) {
  using namespace llvm::MachO;
  if (Info.IsScattered || Info.Length != 2)
    return false;
  if (Info.Type == ARM64_RELOC_BRANCH26 || Info.Type == ARM64_RELOC_PAGE21)
    return Info.IsPCRel;
  return Info.Type == ARM64_RELOC_PAGEOFF12 && !Info.IsPCRel;
}

std::optional<unsigned> arm64PageOffShift(uint32_t Instruction) {
  constexpr uint32_t LoadStoreImm12Mask = 0x3b000000u;
  constexpr uint32_t Vec128Mask = 0x04800000u;
  if ((Instruction & LoadStoreImm12Mask) == 0x39000000u) {
    unsigned Shift = Instruction >> 30;
    if (Shift == 0 && (Instruction & Vec128Mask) == Vec128Mask)
      Shift = 4;
    return Shift;
  }

  constexpr uint32_t AddImmediateMask = 0x7f800000u;
  if ((Instruction & AddImmediateMask) == 0x11000000u &&
      (Instruction & (1u << 22)) == 0)
    return 0;
  return std::nullopt;
}

std::optional<uint32_t> applyARM64Branch26(uint32_t Instruction,
                                           uint64_t SymbolAddress,
                                           uint64_t Place, int64_t Addend) {
  // A B or BL whose offset field the relocation fills.
  if (!branch::A64BranchOrLink.matches(Instruction) ||
      branch::A64BranchOffset.extract(Instruction) != 0)
    return std::nullopt;
  auto Target = addSigned(SymbolAddress, Addend);
  if (!Target)
    return std::nullopt;
  auto Displacement = signedDifference(*Target, Place);
  if (!Displacement || !branch::isA64BranchDisplacement(*Displacement))
    return std::nullopt;
  return branch::withA64BranchDisplacement(Instruction, *Displacement);
}

std::optional<uint32_t> applyARM64Page21(uint32_t Instruction,
                                         uint64_t SymbolAddress, uint64_t Place,
                                         int64_t Addend) {
  if ((Instruction & 0xffffffe0u) != 0x90000000u)
    return std::nullopt;
  auto Target = addSigned(SymbolAddress, Addend);
  if (!Target)
    return std::nullopt;
  auto PageDelta = signedDifference(*Target & ~0xfffULL, Place & ~0xfffULL);
  if (!PageDelta || *PageDelta < -(int64_t(1) << 32) ||
      *PageDelta > (int64_t(1) << 32) - 0x1000)
    return std::nullopt;
  const uint64_t EncodedDelta = static_cast<uint64_t>(*PageDelta);
  const uint32_t ImmLo = (EncodedDelta >> 12) & 0x3;
  const uint32_t ImmHi = (EncodedDelta >> 14) & 0x7ffff;
  return Instruction | (ImmLo << 29) | (ImmHi << 5);
}

std::optional<uint32_t> applyARM64PageOff12(uint32_t Instruction,
                                            uint64_t SymbolAddress,
                                            int64_t Addend) {
  auto Shift = arm64PageOffShift(Instruction);
  if (!Shift)
    return std::nullopt;
  // Mach-O carries PAGEOFF12 addends in a preceding ARM64_RELOC_ADDEND.
  // A non-zero instruction immediate would therefore be a second addend.
  if (((Instruction >> 10) & 0xfff) != 0)
    return std::nullopt;
  auto Target = addSigned(SymbolAddress, Addend);
  if (!Target)
    return std::nullopt;
  const uint64_t PageOffset = *Target & 0xfff;
  if ((PageOffset & ((uint64_t(1) << *Shift) - 1)) != 0)
    return std::nullopt;
  const uint32_t EncodedImmediate = static_cast<uint32_t>(PageOffset >> *Shift);
  return (Instruction & 0xffc003ffu) | (EncodedImmediate << 10);
}

std::optional<uint32_t> applyARM64Instruction(uint32_t Type,
                                              uint32_t Instruction,
                                              uint64_t SymbolAddress,
                                              uint64_t Place, int64_t Addend) {
  using namespace llvm::MachO;
  switch (Type) {
  case ARM64_RELOC_BRANCH26:
    return applyARM64Branch26(Instruction, SymbolAddress, Place, Addend);
  case ARM64_RELOC_PAGE21:
  case ARM64_RELOC_GOT_LOAD_PAGE21:
    return applyARM64Page21(Instruction, SymbolAddress, Place, Addend);
  case ARM64_RELOC_PAGEOFF12:
  case ARM64_RELOC_GOT_LOAD_PAGEOFF12:
    return applyARM64PageOff12(Instruction, SymbolAddress, Addend);
  default:
    return std::nullopt;
  }
}

bool isSubtractorType(Arch TargetArch, uint32_t Type) {
  using namespace llvm::MachO;
  return (TargetArch == Arch::X64 && Type == X86_64_RELOC_SUBTRACTOR) ||
         (TargetArch == Arch::AArch64 && Type == ARM64_RELOC_SUBTRACTOR);
}

bool isUnsignedType(Arch TargetArch, uint32_t Type) {
  using namespace llvm::MachO;
  return (TargetArch == Arch::X64 && Type == X86_64_RELOC_UNSIGNED) ||
         (TargetArch == Arch::AArch64 && Type == ARM64_RELOC_UNSIGNED);
}

bool applySubtractorPair(const MachOObjectFile &Obj,
                         const RelocationRef &Subtractor,
                         const RelocationMetadata &SubtractorInfo,
                         const RelocationRef &Unsigned,
                         const RelocationMetadata &UnsignedInfo,
                         uint64_t SegmentOffset, BinaryImage &Img,
                         Segment &ApplySeg, const ObjectExterns &Externs) {
  if (SubtractorInfo.IsScattered || UnsignedInfo.IsScattered ||
      SubtractorInfo.IsPCRel || UnsignedInfo.IsPCRel ||
      !SubtractorInfo.IsExternal ||
      SubtractorInfo.Address != UnsignedInfo.Address ||
      SubtractorInfo.Length != UnsignedInfo.Length ||
      (SubtractorInfo.Length != 2 && SubtractorInfo.Length != 3))
    return false;

  auto From =
      resolveExternalSymbol(Obj, Subtractor, Img, Externs, /*Branch=*/false);
  const uint8_t Width = uint8_t(1u << SubtractorInfo.Length);
  if (!From || !rangeInBounds(SegmentOffset, Width, ApplySeg.Data.size()))
    return false;

  uint64_t RawField = 0;
  if (Width == 4) {
    uint32_t Encoded = 0;
    std::memcpy(&Encoded, ApplySeg.Data.data() + SegmentOffset, Width);
    RawField = Encoded;
  } else {
    std::memcpy(&RawField, ApplySeg.Data.data() + SegmentOffset, Width);
  }

  uint64_t Minuend = RawField;
  if (UnsignedInfo.IsExternal) {
    auto To =
        resolveExternalSymbol(Obj, Unsigned, Img, Externs, /*Branch=*/false);
    if (!To)
      return false;
    const int64_t Addend = Width == 4 ? static_cast<int32_t>(RawField)
                                      : static_cast<int64_t>(RawField);
    auto Target = addSigned(To->Address, Addend);
    if (!Target)
      return false;
    Minuend = *Target;
  } else if (!resolveRelocationTarget(Obj, Unsigned, UnsignedInfo, Img,
                                      Externs)) {
    return false;
  }

  auto Value = signedDifference(Minuend, From->Address);
  if (!Value || (Width == 4 && (*Value < std::numeric_limits<int32_t>::min() ||
                                *Value > std::numeric_limits<int32_t>::max())))
    return false;
  if (Width == 4) {
    const int32_t Encoded = static_cast<int32_t>(*Value);
    std::memcpy(ApplySeg.Data.data() + SegmentOffset, &Encoded, Width);
  } else {
    const int64_t Encoded = *Value;
    std::memcpy(ApplySeg.Data.data() + SegmentOffset, &Encoded, Width);
  }
  return true;
}

bool isARM64AddImmediate(uint32_t Instruction) {
  return (Instruction & 0x7f800000u) == 0x11000000u &&
         (Instruction & (1u << 22)) == 0;
}

std::optional<uint64_t> x64PCRelativeTailSize(uint32_t Type) {
  using namespace llvm::MachO;
  switch (Type) {
  case X86_64_RELOC_SIGNED:
  case X86_64_RELOC_BRANCH:
    return 0;
  case X86_64_RELOC_SIGNED_1:
    return 1;
  case X86_64_RELOC_SIGNED_2:
    return 2;
  case X86_64_RELOC_SIGNED_4:
    return 4;
  default:
    return std::nullopt;
  }
}

void recordARMAddressMaterialization(BinaryImage &Img, va_t Place,
                                     va_t TargetVA, va_t OwnerVA) {
  if (OwnerVA == InvalidVA)
    return;
  const Segment *OwnerSeg = Img.getSegmentFor(OwnerVA);
  if (!OwnerSeg)
    return;
  const Section *OwnerSec = Img.getSectionFor(OwnerVA);
  const va_t OwnerBegin = OwnerSec ? OwnerSec->VA : OwnerSeg->VA;
  const uint64_t OwnerSize = OwnerSec ? OwnerSec->Size : OwnerSeg->Size;
  if (OwnerSize == 0 || OwnerSize > InvalidVA - OwnerBegin)
    return;
  const va_t OwnerEnd = OwnerBegin + OwnerSize;
  const bool OwnerIsCode = Img.hasExecutableCodeOwnerAt(OwnerVA);
  if (TargetVA < OwnerBegin ||
      (OwnerIsCode ? TargetVA >= OwnerEnd : TargetVA > OwnerEnd))
    return;
  if (Img.hasExecutableCodeOwnerAt(Place))
    Img.InstructionAddressMaterializations[Place] = {TargetVA, OwnerVA};
  if (OwnerIsCode) {
    Img.CodeRefTargets.insert(
        normalizeCodeAddress(TargetVA, Img.Arch, Img.Mode));
    return;
  }
  if (!OwnerSeg->isReadable())
    return;
  bool Writable = OwnerSeg->isWritable();
  if (OwnerSec)
    Writable = OwnerSec->isWritable();
  if (Writable) {
    Img.WritableRelocDataAddrs.insert(TargetVA);
    return;
  }
  Img.RelocDataAddrs.insert(TargetVA);
  if (Img.hasExecutableCodeOwnerAt(Place))
    Img.RelCodeTableAnchors.insert(TargetVA);
}

void recordARMPageFragment(BinaryImage &Img, va_t Place, va_t TargetVA,
                           va_t OwnerVA) {
  if (Img.hasExecutableCodeOwnerAt(Place) && OwnerVA != InvalidVA &&
      Img.relocatedTargetBelongsToOwner(TargetVA, OwnerVA))
    Img.InstructionPageAddressFragments[Place] = {TargetVA, OwnerVA};
}

void synchronizeObjectSectionData(BinaryImage &Img) {
  for (Section &Sec : Img.Sections) {
    for (const Segment &Seg : Img.Segments) {
      if (Seg.VA != Sec.VA || Seg.Size != Sec.Size)
        continue;
      Sec.Data = Seg.Data;
      break;
    }
  }
}

} // namespace

llvm::Error applyObjectRelocations(const llvm::object::MachOObjectFile &Obj,
                                   BinaryImage &Img) {
  using namespace llvm::MachO;

  if (Img.Arch == Arch::X86) {
    applyI386ObjectRelocations(Obj, Img);
    synchronizeObjectSectionData(Img);
    return llvm::Error::success();
  }

  // The undefined and common symbols of the object take addresses past every
  // section, beside the GOT entries its GOT references reach.
  auto ExternsOr = planObjectExterns(Obj, Img);
  if (!ExternsOr)
    return ExternsOr.takeError();
  const ObjectExterns Externs = std::move(*ExternsOr);
  addObjectExterns(Obj, Externs, Img);

  for (const llvm::object::SectionRef &SecRef : Obj.sections()) {
    uint64_t SecAddr = SecRef.getAddress();
    Segment *ApplySeg = nullptr;
    for (auto &Seg : Img.Segments) {
      if (Seg.contains(SecAddr) && !Seg.Data.empty()) {
        ApplySeg = &Seg;
        break;
      }
    }
    if (!ApplySeg)
      continue;
    std::vector<RelocationRef> Relocations;
    for (const RelocationRef &Reloc : SecRef.relocations())
      Relocations.push_back(Reloc);

    for (size_t I = 0; I < Relocations.size(); ++I) {
      size_t RelocationIndex = I;
      RelocationMetadata Info =
          relocationMetadata(Obj, Relocations[RelocationIndex]);
      int64_t ARM64Addend = 0;

      if (Img.Arch == Arch::AArch64 && Info.Type == ARM64_RELOC_ADDEND) {
        size_t RunEnd = I + 1;
        while (RunEnd < Relocations.size() &&
               relocationMetadata(Obj, Relocations[RunEnd]).Type ==
                   ARM64_RELOC_ADDEND)
          ++RunEnd;
        const bool HasTarget = RunEnd < Relocations.size();
        RelocationMetadata TargetInfo;
        if (HasTarget)
          TargetInfo = relocationMetadata(Obj, Relocations[RunEnd]);
        const bool IsValidPair =
            RunEnd == I + 1 && HasTarget && hasValidARM64AddendMetadata(Info) &&
            isARM64InstructionRelocation(TargetInfo.Type) &&
            hasValidARM64InstructionMetadata(TargetInfo) &&
            Info.Address == TargetInfo.Address;
        if (!IsValidPair) {
          return relocationError("malformed ARM64 ADDEND pair", SecAddr,
                                 Info.Address);
        }
        ARM64Addend = llvm::SignExtend64<24>(Info.SymbolNumber);
        RelocationIndex = RunEnd;
        Info = TargetInfo;
        I = RunEnd;
      }

      if (isSubtractorType(Img.Arch, Info.Type)) {
        if (I + 1 >= Relocations.size()) {
          return relocationError("orphan SUBTRACTOR", SecAddr, Info.Address);
        }
        const size_t UnsignedIndex = I + 1;
        const RelocationMetadata UnsignedInfo =
            relocationMetadata(Obj, Relocations[UnsignedIndex]);
        if (!isUnsignedType(Img.Arch, UnsignedInfo.Type)) {
          return relocationError("SUBTRACTOR has no adjacent UNSIGNED", SecAddr,
                                 Info.Address);
        }
        I = UnsignedIndex;
        const uint64_t RAddr = Info.Address;
        const uint64_t SectionOff = SecAddr - ApplySeg->VA;
        if (RAddr > InvalidVA - SectionOff ||
            SectionOff + RAddr >= ApplySeg->Data.size()) {
          return relocationError("SUBTRACTOR field is out of bounds", SecAddr,
                                 RAddr);
        }
        const uint64_t SegOff = SectionOff + RAddr;
        if (!applySubtractorPair(Obj, Relocations[RelocationIndex], Info,
                                 Relocations[UnsignedIndex], UnsignedInfo,
                                 SegOff, Img, *ApplySeg, Externs))
          return relocationError("malformed or overflowing SUBTRACTOR pair",
                                 SecAddr, RAddr);
        continue;
      }

      const RelocationRef &Reloc = Relocations[RelocationIndex];
      // A GOT reference applies its direct form against the GOT entry, which
      // holds the symbol's address.
      std::optional<va_t> GOTEntry;
      if (!Info.IsScattered && Info.IsExternal)
        if (auto It = Externs.GOTCells.find(Info.SymbolNumber);
            It != Externs.GOTCells.end()) {
          GOTEntry = It->second;
          if (auto Direct = directTypeOfGOTReference(Img.Arch, Info.Type))
            Info.Type = *Direct;
        }
      const uint64_t RAddr = Info.Address;
      const uint32_t RType = Info.Type;
      auto Resolved =
          GOTEntry ? std::optional<ResolvedSymbol>(
                         ResolvedSymbol{*GOTEntry, Externs.CellBase})
                   : resolveRelocationTarget(Obj, Reloc, Info, Img, Externs);
      const uint64_t SymVal = Resolved ? Resolved->Address : 0;
      const uint64_t SymOwnerVA = Resolved ? Resolved->OwnerVA : InvalidVA;
      uint64_t SectionOff = SecAddr - ApplySeg->VA;
      if (RAddr > InvalidVA - SectionOff || RAddr > InvalidVA - SecAddr)
        return relocationError("relocation address overflows", SecAddr, RAddr);
      uint64_t SegOff = SectionOff + RAddr;
      if (SegOff >= ApplySeg->Data.size())
        return relocationError("relocation field is out of bounds", SecAddr,
                               RAddr);
      va_t S = SymVal;
      va_t P = SecAddr + RAddr;
      auto RecordRelDataPtr = [&]() {
        const Segment *PSeg = Img.getSegmentFor(P);
        const Segment *TSeg = Img.getSegmentFor(S);
        if (PSeg && PSeg->isReadable() && !PSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(P) && !PSeg->Data.empty() && TSeg &&
            TSeg->isReadable() && !TSeg->isWritable() &&
            !Img.hasExecutableCodeOwnerAt(S) && !TSeg->Data.empty())
          Img.RelDataPtrRelocSlots.insert(P);
      };
      if (Img.Arch == Arch::X64) {
        if (const std::optional<uint64_t> Tail = x64PCRelativeTailSize(RType)) {
          if (Info.IsScattered || !Info.IsPCRel || Info.Length != 2)
            return relocationError("invalid x86-64 PC-relative metadata",
                                   SecAddr, RAddr);
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            return relocationError("x86-64 relocation field is out of bounds",
                                   SecAddr, RAddr);
          if (!Resolved) {
            diagnoseRelocation("unresolved x86-64 target", SecAddr, RAddr);
            continue;
          }
          int32_t Existing = 0;
          std::memcpy(&Existing, ApplySeg->Data.data() + SegOff, 4);
          std::optional<int64_t> Displacement;
          if (Info.IsExternal) {
            auto Target = addSigned(S, Existing);
            if (!Target || P > InvalidVA - 4 - *Tail)
              return relocationError("x86-64 relocation overflows", SecAddr,
                                     RAddr);
            Displacement = signedDifference(*Target, P + 4 + *Tail);
          } else {
            auto SectionDelta = signedDifference(S, SecAddr);
            if (SectionDelta) {
              int64_t Sum = 0;
              if (!llvm::AddOverflow(*SectionDelta,
                                     static_cast<int64_t>(Existing), Sum))
                Displacement = Sum;
            }
          }
          if (!Displacement ||
              *Displacement < std::numeric_limits<int32_t>::min() ||
              *Displacement > std::numeric_limits<int32_t>::max())
            return relocationError("x86-64 displacement is not representable",
                                   SecAddr, RAddr);
          const int32_t Val = static_cast<int32_t>(*Displacement);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 4);
          if (Info.IsExternal && RType == X86_64_RELOC_SIGNED)
            RecordRelDataPtr();
        } else if (RType == X86_64_RELOC_UNSIGNED) {
          if (Info.IsScattered || Info.IsPCRel || Info.Length > 3)
            return relocationError("invalid x86-64 UNSIGNED metadata", SecAddr,
                                   RAddr);
          const uint8_t Width = uint8_t(1u << Info.Length);
          if (!rangeInBounds(SegOff, Width, ApplySeg->Data.size()))
            return relocationError("x86-64 UNSIGNED field is out of bounds",
                                   SecAddr, RAddr);
          if (!Resolved) {
            diagnoseRelocation("unresolved x86-64 target", SecAddr, RAddr);
            continue;
          }
          uint64_t Addend = 0;
          std::memcpy(&Addend, ApplySeg->Data.data() + SegOff, Width);
          if (S > InvalidVA - Addend)
            return relocationError("x86-64 UNSIGNED value overflows", SecAddr,
                                   RAddr);
          const uint64_t Val = S + Addend;
          if (Width < 8 && Val >= (uint64_t(1) << (Width * 8)))
            return relocationError("x86-64 UNSIGNED value is not representable",
                                   SecAddr, RAddr);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, Width);
          if (Width == Img.getPointerSize() && SymOwnerVA != InvalidVA)
            recordAbsolutePointerRelocation(Img, P, Val, SymOwnerVA);
        } else if (RType == X86_64_RELOC_GOT_LOAD ||
                   RType == X86_64_RELOC_GOT || RType == X86_64_RELOC_TLV) {
          return relocationError("unsupported x86-64 indirection relocation",
                                 SecAddr, RAddr);
        } else {
          return relocationError("unsupported x86-64 relocation", SecAddr,
                                 RAddr);
        }
      } else if (Img.Arch == Arch::AArch64) {
        if (RType == ARM64_RELOC_UNSIGNED) {
          if (Info.IsScattered || Info.IsPCRel || Info.Length != 3)
            return relocationError("invalid ARM64 UNSIGNED metadata", SecAddr,
                                   RAddr);
          if (!rangeInBounds(SegOff, 8, ApplySeg->Data.size()))
            return relocationError("ARM64 UNSIGNED field is out of bounds",
                                   SecAddr, RAddr);
          if (!Resolved) {
            diagnoseRelocation("unresolved ARM64 target", SecAddr, RAddr);
            continue;
          }
          uint64_t Addend = 0;
          std::memcpy(&Addend, ApplySeg->Data.data() + SegOff, 8);
          if (S > InvalidVA - Addend)
            return relocationError("ARM64 UNSIGNED value overflows", SecAddr,
                                   RAddr);
          const uint64_t Val = S + Addend;
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 8);
          if (SymOwnerVA != InvalidVA)
            recordAbsolutePointerRelocation(Img, P, Val, SymOwnerVA);
        } else if (RType == ARM64_RELOC_AUTHENTICATED_POINTER) {
          if (Info.IsScattered || Info.IsPCRel ||
              Info.Length != arm64e::kObjectPointerLength || !Info.IsExternal ||
              !Resolved ||
              !rangeInBounds(SegOff, arm64e::kPointerBytes,
                             ApplySeg->Data.size()))
            return relocationError(arm64e::kInvalidObjectPointer, SecAddr,
                                   RAddr);
          const uint64_t Raw = readLE<uint64_t>(ApplySeg->Data.data() + SegOff);
          if (!arm64e::kAuthentication.extract(Raw) ||
              arm64e::kObjectReserved.extract(Raw))
            return relocationError(arm64e::kInvalidObjectPointerBits, SecAddr,
                                   RAddr);
          const int64_t Addend = arm64e::kObjectAddend.signExtend(Raw);
          const auto Target = addSigned(S, Addend);
          if (!Target || Img.MachOAuthenticatedPointers.count(P))
            return relocationError(arm64e::kConflictingObjectPointer, SecAddr,
                                   RAddr);
          MachOAuthenticatedPointer Auth;
          Auth.Target = *Target;
          Auth.EncodedValue = Raw;
          Auth.Addend = Addend;
          Auth.Diversity = arm64e::kDiversity.extract(Raw);
          Auth.AddressDiversity = arm64e::kAddressDiversity.extract(Raw);
          Auth.Key = arm64e::kKey.extract(Raw);
          if (const auto Symbol = Reloc.getSymbol();
              Symbol != Obj.symbol_end()) {
            auto Name = Symbol->getName();
            if (!Name)
              return Name.takeError();
            Auth.Symbol = Name->str();
          }
          Img.MachOAuthenticatedPointers.emplace(P, std::move(Auth));
          // Keep the relocation recipe separate from ordinary pointer bits.
          // In particular it cannot establish an immutable raw-pointer load.
          Img.CodePtrRelocSlots.erase(P);
          Img.DataPtrRelocSlots.erase(P);
        } else if (RType == ARM64_RELOC_BRANCH26 ||
                   RType == ARM64_RELOC_PAGE21 ||
                   RType == ARM64_RELOC_PAGEOFF12) {
          if (!hasValidARM64InstructionMetadata(Info))
            return relocationError("invalid ARM64 instruction metadata",
                                   SecAddr, RAddr);
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            return relocationError("ARM64 instruction field is out of bounds",
                                   SecAddr, RAddr);
          if (!Resolved) {
            diagnoseRelocation("unresolved ARM64 target", SecAddr, RAddr);
            continue;
          }
          uint32_t Instruction = 0;
          std::memcpy(&Instruction, ApplySeg->Data.data() + SegOff, 4);
          auto Patched =
              applyARM64Instruction(RType, Instruction, S, P, ARM64Addend);
          if (!Patched)
            return relocationError("invalid ARM64 instruction relocation",
                                   SecAddr, RAddr);
          std::memcpy(ApplySeg->Data.data() + SegOff, &*Patched, 4);
          if (RType == ARM64_RELOC_PAGE21) {
            auto Target = addSigned(S, ARM64Addend);
            if (!Target)
              return relocationError("ARM64 page target overflows", SecAddr,
                                     RAddr);
            recordARMPageFragment(Img, P, *Target, SymOwnerVA);
          }
          if (RType == ARM64_RELOC_PAGEOFF12 &&
              isARM64AddImmediate(Instruction)) {
            auto Target = addSigned(S, ARM64Addend);
            if (!Target)
              return relocationError("ARM64 materialized address overflows",
                                     SecAddr, RAddr);
            recordARMAddressMaterialization(Img, P, *Target, SymOwnerVA);
          }
        } else if (isGOTPointerReference(Img.Arch, RType) && GOTEntry) {
          // The field holds the GOT entry's address, or its distance from the
          // field when the relocation is PC-relative.
          if (Info.IsPCRel && Info.Length == 2 &&
              rangeInBounds(SegOff, 4, ApplySeg->Data.size())) {
            auto Delta = signedDifference(*GOTEntry, P);
            if (!Delta || *Delta < std::numeric_limits<int32_t>::min() ||
                *Delta > std::numeric_limits<int32_t>::max())
              return relocationError("ARM64 GOT distance is not representable",
                                     SecAddr, RAddr);
            const int32_t Val = static_cast<int32_t>(*Delta);
            std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 4);
          } else if (!Info.IsPCRel && Info.Length == 3 &&
                     rangeInBounds(SegOff, 8, ApplySeg->Data.size())) {
            const uint64_t Val = *GOTEntry;
            std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 8);
            recordAbsolutePointerRelocation(Img, P, Val, Externs.CellBase);
          } else {
            return relocationError("invalid ARM64 GOT pointer metadata",
                                   SecAddr, RAddr);
          }
        } else if (RType == ARM64_RELOC_GOT_LOAD_PAGE21 ||
                   RType == ARM64_RELOC_GOT_LOAD_PAGEOFF12 ||
                   RType == ARM64_RELOC_POINTER_TO_GOT ||
                   RType == ARM64_RELOC_TLVP_LOAD_PAGE21 ||
                   RType == ARM64_RELOC_TLVP_LOAD_PAGEOFF12) {
          return relocationError("unsupported ARM64 indirection relocation",
                                 SecAddr, RAddr);
        } else {
          return relocationError("unsupported ARM64 relocation", SecAddr,
                                 RAddr);
        }
      } else if (Img.Arch == Arch::ARM) {
        if (RType == ARM_RELOC_VANILLA) {
          if (Info.IsPCRel || Info.Length != 2)
            return relocationError("invalid ARM VANILLA metadata", SecAddr,
                                   RAddr);
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            return relocationError("ARM VANILLA field is out of bounds",
                                   SecAddr, RAddr);
          if (!Info.IsScattered && !Resolved) {
            diagnoseRelocation("unresolved ARM target", SecAddr, RAddr);
            continue;
          }
          uint32_t Encoded = 0;
          std::memcpy(&Encoded, ApplySeg->Data.data() + SegOff, 4);
          std::optional<uint64_t> Target;
          uint64_t TargetOwnerVA = SymOwnerVA;
          if (Info.IsScattered) {
            const auto RawReloc = Obj.getRelocation(Reloc.getRawDataRefImpl());
            const uint64_t OriginalTarget =
                Obj.getScatteredRelocationValue(RawReloc);
            auto MappedTarget = mapObjectAddress(Obj, Img, OriginalTarget);
            if (MappedTarget && OriginalTarget <= UINT32_MAX) {
              const int32_t Addend = static_cast<int32_t>(
                  Encoded - static_cast<uint32_t>(OriginalTarget));
              Target = addSigned(MappedTarget->Address, Addend);
              TargetOwnerVA = MappedTarget->OwnerVA;
            }
          } else if (Info.IsExternal) {
            Target = addSigned(S, static_cast<int32_t>(Encoded));
          } else if (Info.SymbolNumber > 0 &&
                     Info.SymbolNumber <= Img.Sections.size()) {
            uint32_t SectionOrdinal = 0;
            for (const llvm::object::SectionRef &ObjectSec : Obj.sections()) {
              if (++SectionOrdinal != Info.SymbolNumber)
                continue;
              const uint64_t OriginalBase = ObjectSec.getAddress();
              if (OriginalBase <= UINT32_MAX) {
                const int32_t Addend = static_cast<int32_t>(
                    Encoded - static_cast<uint32_t>(OriginalBase));
                Target = addSigned(S, Addend);
              }
              break;
            }
          }
          if (!Target || *Target > UINT32_MAX)
            return relocationError("ARM VANILLA target is unresolved", SecAddr,
                                   RAddr);
          const uint32_t Val = static_cast<uint32_t>(*Target);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 4);
          if (TargetOwnerVA != InvalidVA)
            recordAbsolutePointerRelocation(Img, P, Val, TargetOwnerVA);
        } else if (RType == ARM_RELOC_BR24) {
          if (Info.IsScattered || !Info.IsPCRel || Info.Length != 2)
            return relocationError("invalid ARM BR24 metadata", SecAddr, RAddr);
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            return relocationError("ARM BR24 field is out of bounds", SecAddr,
                                   RAddr);
          if (!Resolved) {
            diagnoseRelocation("unresolved ARM branch target", SecAddr, RAddr);
            continue;
          }
          uint32_t Instruction = 0;
          std::memcpy(&Instruction, ApplySeg->Data.data() + SegOff, 4);
          const bool IsLink = (Instruction & 0x01000000u) != 0;
          if ((Instruction & 0x0e000000u) != 0x0a000000u ||
              (Instruction >> 28) == 0xf || (Resolved->IsThumb && !IsLink))
            return relocationError("invalid ARM BR24 instruction", SecAddr,
                                   RAddr);
          int32_t EncodedWords =
              static_cast<int32_t>(Instruction & 0x00ffffffu);
          if ((EncodedWords & 0x00800000) != 0)
            EncodedWords |= ~0x00ffffff;
          if (P > InvalidVA - 8)
            return relocationError("ARM BR24 place overflows", SecAddr, RAddr);
          const int64_t EncodedDisplacement =
              static_cast<int64_t>(EncodedWords) * 4;
          auto OriginalTarget = addSigned(P + 8, EncodedDisplacement);
          std::optional<uint64_t> Target;
          if (OriginalTarget && Info.IsExternal &&
              *OriginalTarget <= UINT32_MAX) {
            Target = addSigned(S, static_cast<int32_t>(
                                      static_cast<uint32_t>(*OriginalTarget)));
          } else if (OriginalTarget && Info.SymbolNumber > 0 &&
                     Info.SymbolNumber <= Img.Sections.size()) {
            uint32_t SectionOrdinal = 0;
            for (const llvm::object::SectionRef &ObjectSec : Obj.sections()) {
              if (++SectionOrdinal != Info.SymbolNumber)
                continue;
              auto TargetOffset =
                  signedDifference(*OriginalTarget, ObjectSec.getAddress());
              if (TargetOffset)
                Target = addSigned(S, *TargetOffset);
              break;
            }
          }
          if (!Target)
            return relocationError("ARM BR24 target overflows", SecAddr, RAddr);
          auto Displacement = signedDifference(*Target, P + 8);
          const int64_t Alignment = Resolved->IsThumb ? 2 : 4;
          const int64_t Maximum =
              (int64_t(1) << 25) - (Resolved->IsThumb ? 2 : 4);
          if (!Displacement || (*Displacement & (Alignment - 1)) != 0 ||
              *Displacement < -(int64_t(1) << 25) || *Displacement > Maximum)
            return relocationError("ARM BR24 target is out of range", SecAddr,
                                   RAddr);
          if (Resolved->IsThumb) {
            Instruction =
                0xfa000000u |
                (static_cast<uint32_t>(*Displacement >> 2) & 0x00ffffffu) |
                ((static_cast<uint32_t>(*Displacement >> 1) & 1u) << 24);
          } else {
            Instruction =
                (Instruction & 0xff000000u) |
                (static_cast<uint32_t>(*Displacement >> 2) & 0x00ffffffu);
          }
          if (llvm::Error Error = recordARMRelocationMode(
                  Img, P, InstructionMode::ARM, SecAddr, RAddr))
            return Error;
          std::memcpy(ApplySeg->Data.data() + SegOff, &Instruction, 4);
        } else if (RType == ARM_THUMB_RELOC_BR22) {
          if (Info.IsScattered || !Info.IsPCRel || Info.Length != 2)
            return relocationError("invalid Thumb BR22 metadata", SecAddr,
                                   RAddr);
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            return relocationError("Thumb BR22 field is out of bounds", SecAddr,
                                   RAddr);
          if (!Resolved) {
            diagnoseRelocation("unresolved Thumb branch target", SecAddr,
                               RAddr);
            continue;
          }
          const uint8_t *Bytes = ApplySeg->Data.data() + SegOff;
          uint16_t Hi = readLE<uint16_t>(Bytes);
          uint16_t Lo = readLE<uint16_t>(Bytes + 2);
          const uint16_t Opcode = Lo & 0xd000u;
          const bool IsJump = Opcode == 0x9000u;
          const bool IsBL = Opcode == 0xd000u;
          const bool IsBLX = Opcode == 0xc000u;
          if ((Hi & 0xf800u) != 0xf000u || (!IsJump && !IsBL && !IsBLX) ||
              (IsBLX && (Lo & 1u) != 0))
            return relocationError("invalid Thumb BR22 instruction", SecAddr,
                                   RAddr);
          if (P > UINT32_MAX - 4)
            return relocationError("Thumb BR22 place overflows", SecAddr,
                                   RAddr);
          size_t Length = 0;
          auto OriginalTarget = decodeDirectBranchTarget(
              Arch::ARM, InstructionMode::Thumb, Bytes, 4, P, Length);
          if (!OriginalTarget || Length != 4)
            return relocationError("invalid Thumb BR22 target", SecAddr, RAddr);
          std::optional<uint64_t> Target;
          std::optional<bool> TargetIsThumb;
          if (Info.IsExternal && *OriginalTarget <= UINT32_MAX) {
            Target = addSigned(S, static_cast<int32_t>(*OriginalTarget));
            TargetIsThumb = Resolved->IsThumb;
          } else if (Info.SymbolNumber > 0 &&
                     Info.SymbolNumber <= Img.Sections.size()) {
            uint32_t SectionOrdinal = 0;
            for (const llvm::object::SectionRef &ObjectSec : Obj.sections()) {
              if (++SectionOrdinal != Info.SymbolNumber)
                continue;
              auto TargetOffset =
                  signedDifference(*OriginalTarget, ObjectSec.getAddress());
              if (TargetOffset) {
                Target = addSigned(S, *TargetOffset);
                TargetIsThumb = thumbStateAtBranchTarget(Obj, Info.SymbolNumber,
                                                         *OriginalTarget);
              }
              break;
            }
          }
          if (!Target || *Target > UINT32_MAX)
            return relocationError("Thumb BR22 target overflows", SecAddr,
                                   RAddr);
          const bool ToARM = TargetIsThumb ? !*TargetIsThumb : IsBLX;
          if (IsJump && ToARM)
            return relocationError(
                "Thumb branch needs an unsupported interworking veneer",
                SecAddr, RAddr);
          const uint64_t Base = ToARM ? ((P + 4) & ~uint64_t(3)) : P + 4;
          auto Displacement = signedDifference(*Target, Base);
          const int64_t Alignment = ToARM ? 4 : 2;
          if (!Displacement || (*Displacement & (Alignment - 1)) != 0 ||
              *Displacement < -(int64_t(1) << 24) ||
              *Displacement > (int64_t(1) << 24) - Alignment)
            return relocationError("Thumb BR22 target is out of range", SecAddr,
                                   RAddr);
          const uint32_t Encoded =
              static_cast<uint32_t>(*Displacement) & 0x01ffffffu;
          const uint32_t Sign = Encoded >> 24;
          const uint32_t I1 = (Encoded >> 23) & 1u;
          const uint32_t I2 = (Encoded >> 22) & 1u;
          const uint32_t J1 = (~(I1 ^ Sign)) & 1u;
          const uint32_t J2 = (~(I2 ^ Sign)) & 1u;
          Hi = static_cast<uint16_t>(0xf000u | (Sign << 10) |
                                     ((Encoded >> 12) & 0x03ffu));
          Lo = static_cast<uint16_t>((IsJump  ? 0x9000u
                                      : ToARM ? 0xc000u
                                              : 0xd000u) |
                                     (J1 << 13) | (J2 << 11) |
                                     ((Encoded >> 1) & 0x07ffu));
          if (llvm::Error Error = recordARMRelocationMode(
                  Img, P, InstructionMode::Thumb, SecAddr, RAddr))
            return Error;
          writeLE<uint16_t>(ApplySeg->Data.data() + SegOff, Hi);
          writeLE<uint16_t>(ApplySeg->Data.data() + SegOff + 2, Lo);
        } else if (RType == ARM_RELOC_HALF ||
                   RType == ARM_RELOC_HALF_SECTDIFF) {
          const bool SectionDifference = RType == ARM_RELOC_HALF_SECTDIFF;
          if (Info.IsScattered != SectionDifference || Info.IsPCRel ||
              I + 1 >= Relocations.size())
            return relocationError("malformed ARM HALF pair", SecAddr, RAddr);
          const RelocationMetadata PairInfo =
              relocationMetadata(Obj, Relocations[I + 1]);
          if (PairInfo.IsScattered != SectionDifference || PairInfo.IsPCRel ||
              PairInfo.IsExternal || PairInfo.Type != ARM_RELOC_PAIR ||
              PairInfo.Length != Info.Length)
            return relocationError("malformed ARM HALF pair", SecAddr, RAddr);
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            return relocationError("ARM HALF field is out of bounds", SecAddr,
                                   RAddr);
          if (!Resolved && !SectionDifference) {
            diagnoseRelocation("unresolved ARM HALF target", SecAddr, RAddr);
            ++I;
            continue;
          }
          uint32_t Instruction = 0;
          std::memcpy(&Instruction, ApplySeg->Data.data() + SegOff, 4);
          const bool IsHigh = (Info.Length & 1u) != 0;
          const bool IsThumb = (Info.Length & 2u) != 0;
          uint16_t InstructionHalf = 0;
          if (IsThumb) {
            const uint16_t Hi = static_cast<uint16_t>(Instruction);
            const uint16_t Lo = static_cast<uint16_t>(Instruction >> 16);
            const uint16_t ExpectedOpcode = IsHigh ? 0xf2c0u : 0xf240u;
            if ((Hi & 0xfbf0u) != ExpectedOpcode)
              return relocationError("invalid Thumb HALF instruction", SecAddr,
                                     RAddr);
            InstructionHalf = static_cast<uint16_t>(
                ((Hi & 0x000fu) << 12) | ((Hi & 0x0400u) << 1) |
                ((Lo & 0x7000u) >> 4) | (Lo & 0x00ffu));
          } else {
            const uint32_t ExpectedOpcode = IsHigh ? 0x03400000u : 0x03000000u;
            if ((Instruction & 0x0ff00000u) != ExpectedOpcode)
              return relocationError("invalid ARM HALF instruction", SecAddr,
                                     RAddr);
            InstructionHalf = static_cast<uint16_t>(
                ((Instruction >> 4) & 0xf000u) | (Instruction & 0x0fffu));
          }
          const uint16_t OtherHalf =
              static_cast<uint16_t>(PairInfo.Address & 0xffffu);
          const uint32_t EncodedAddress =
              IsHigh
                  ? (static_cast<uint32_t>(InstructionHalf) << 16) | OtherHalf
                  : (static_cast<uint32_t>(OtherHalf) << 16) | InstructionHalf;
          std::optional<uint64_t> Target;
          if (SectionDifference) {
            const auto Raw = Obj.getRelocation(Reloc.getRawDataRefImpl());
            const auto Pair =
                Obj.getRelocation(Relocations[I + 1].getRawDataRefImpl());
            const uint32_t OriginalTo = Obj.getScatteredRelocationValue(Raw);
            const uint32_t OriginalFrom = Obj.getScatteredRelocationValue(Pair);
            const auto To = mapObjectAddress(Obj, Img, OriginalTo);
            const auto From = mapObjectAddress(Obj, Img, OriginalFrom);
            if (!To || !From || To->Address > UINT32_MAX ||
                From->Address > UINT32_MAX)
              return relocationError("ARM HALF_SECTDIFF owner is unresolved",
                                     SecAddr, RAddr);
            // Both halves encode A-B+addend, not A. The pair's address is
            // the other half; its scattered value is B in the input image.
            const uint32_t Addend =
                EncodedAddress - (OriginalTo - OriginalFrom);
            Target =
                static_cast<uint32_t>(To->Address - From->Address + Addend);
          } else if (Info.IsExternal) {
            Target = addSigned(S, static_cast<int32_t>(EncodedAddress));
          } else if (Info.SymbolNumber > 0) {
            uint32_t SectionOrdinal = 0;
            std::optional<uint64_t> OriginalOwnerVA;
            for (const llvm::object::SectionRef &OriginalSec : Obj.sections()) {
              ++SectionOrdinal;
              if (SectionOrdinal == Info.SymbolNumber) {
                OriginalOwnerVA = OriginalSec.getAddress();
                break;
              }
            }
            if (OriginalOwnerVA) {
              auto OwnerAddend =
                  signedDifference(EncodedAddress, *OriginalOwnerVA);
              if (OwnerAddend)
                Target = addSigned(S, *OwnerAddend);
            }
          }
          if (!Target || *Target > UINT32_MAX)
            return relocationError("ARM HALF target overflows", SecAddr, RAddr);
          const uint16_t Result =
              static_cast<uint16_t>(IsHigh ? *Target >> 16 : *Target);
          if (IsThumb) {
            uint16_t Hi = static_cast<uint16_t>(Instruction);
            uint16_t Lo = static_cast<uint16_t>(Instruction >> 16);
            Hi = static_cast<uint16_t>((Hi & ~0x040fu) |
                                       ((Result >> 12) & 0x000fu) |
                                       ((Result >> 1) & 0x0400u));
            Lo = static_cast<uint16_t>((Lo & ~0x70ffu) |
                                       ((Result << 4) & 0x7000u) |
                                       (Result & 0x00ffu));
            Instruction =
                static_cast<uint32_t>(Hi) | (static_cast<uint32_t>(Lo) << 16);
          } else {
            Instruction = (Instruction & ~0x000f0fffu) |
                          ((static_cast<uint32_t>(Result) & 0xf000u) << 4) |
                          (static_cast<uint32_t>(Result) & 0x0fffu);
          }
          if (llvm::Error Error = recordARMRelocationMode(
                  Img, P,
                  IsThumb ? InstructionMode::Thumb : InstructionMode::ARM,
                  SecAddr, RAddr))
            return Error;
          std::memcpy(ApplySeg->Data.data() + SegOff, &Instruction, 4);
          // A difference is not an address. Its later PC/base addition owns
          // reconstruction; never label this immediate as the target pointer.
          if (!SectionDifference)
            recordARMAddressMaterialization(Img, P, *Target, SymOwnerVA);
          ++I;
        } else {
          return relocationError("unsupported ARM relocation", SecAddr, RAddr);
        }
      }
    }
  }
  synchronizeObjectSectionData(Img);
  return llvm::Error::success();
}

} // namespace neverd::macho_loader

#undef DEBUG_TYPE
