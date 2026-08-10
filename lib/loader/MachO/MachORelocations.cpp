//===- MachORelocations.cpp - Mach-O object relocations ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachORelocations.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <limits>
#include <vector>

#define DEBUG_TYPE "neverd-macho-relocations"

namespace neverd::macho_loader::detail {

static bool isValidI386Width(uint8_t Width) {
  return Width == 1 || Width == 2 || Width == 4;
}

std::optional<int64_t> evaluateI386Vanilla(const I386VanillaValue &R) {
  if (!isValidI386Width(R.Width))
    return std::nullopt;

  int64_t Value = 0;
  if (llvm::AddOverflow(R.Target, R.Addend, Value))
    return std::nullopt;
  if (!R.IsPCRel)
    return Value;
  if (R.Place > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return std::nullopt;

  int64_t NextPC = 0;
  if (llvm::AddOverflow(static_cast<int64_t>(R.Place),
                        static_cast<int64_t>(R.Width), NextPC) ||
      llvm::SubOverflow(Value, NextPC, Value))
    return std::nullopt;
  return Value;
}

std::optional<int64_t>
evaluateI386SectionDifference(int64_t FinalA, int64_t FinalB, int64_t EncodedA,
                              int64_t EncodedB, int64_t Existing) {
  int64_t EncodedDifference = 0;
  int64_t ConstantAddend = 0;
  int64_t FinalDifference = 0;
  int64_t Result = 0;
  if (llvm::SubOverflow(EncodedA, EncodedB, EncodedDifference) ||
      llvm::SubOverflow(Existing, EncodedDifference, ConstantAddend) ||
      llvm::SubOverflow(FinalA, FinalB, FinalDifference) ||
      llvm::AddOverflow(FinalDifference, ConstantAddend, Result))
    return std::nullopt;
  return Result;
}

bool writeI386RelocationField(llvm::MutableArrayRef<uint8_t> Data,
                              uint64_t Offset, uint8_t Width, int64_t Value,
                              bool SignedValue) {
  if (!isValidI386Width(Width) || !rangeInBounds(Offset, Width, Data.size()))
    return false;

  int64_t Min = 0;
  int64_t Max = 0;
  if (SignedValue) {
    switch (Width) {
    case 1:
      Min = std::numeric_limits<int8_t>::min();
      Max = std::numeric_limits<int8_t>::max();
      break;
    case 2:
      Min = std::numeric_limits<int16_t>::min();
      Max = std::numeric_limits<int16_t>::max();
      break;
    case 4:
      Min = std::numeric_limits<int32_t>::min();
      Max = std::numeric_limits<int32_t>::max();
      break;
    }
  } else {
    Min = 0;
    switch (Width) {
    case 1:
      Max = std::numeric_limits<uint8_t>::max();
      break;
    case 2:
      Max = std::numeric_limits<uint16_t>::max();
      break;
    case 4:
      Max = std::numeric_limits<uint32_t>::max();
      break;
    }
  }
  if (Value < Min || Value > Max)
    return false;

  switch (Width) {
  case 1:
    writeLE<uint8_t>(Data.data() + Offset, static_cast<uint8_t>(Value));
    break;
  case 2:
    writeLE<uint16_t>(Data.data() + Offset, static_cast<uint16_t>(Value));
    break;
  case 4:
    writeLE<uint32_t>(Data.data() + Offset, static_cast<uint32_t>(Value));
    break;
  }
  return true;
}

} // namespace neverd::macho_loader::detail

namespace neverd::macho_loader {

namespace {

using llvm::object::MachOObjectFile;
using llvm::object::RelocationRef;
using llvm::object::SectionRef;

struct I386MappedSection {
  int64_t OriginalBase = 0;
  int64_t FinalBase = 0;
  uint64_t Size = 0;
};

struct I386AddressClass {
  bool IsValid = false;
  bool IsReadable = false;
  bool IsWritable = false;
  bool IsExecutable = false;
  bool HasData = false;
};

void diagnoseI386Relocation(llvm::StringRef Reason, uint64_t SectionAddress,
                            uint64_t RelocationAddress) {
  LLVM_DEBUG(llvm::dbgs() << "macho i386: skip relocation at section "
                          << SectionAddress << '+' << RelocationAddress << ": "
                          << Reason << '\n');
  (void)Reason;
  (void)SectionAddress;
  (void)RelocationAddress;
}

std::optional<int64_t> checkedI386Address(uint64_t Address) {
  if (Address > uint64_t(std::numeric_limits<int64_t>::max()))
    return std::nullopt;
  return static_cast<int64_t>(Address);
}

std::optional<int64_t> checkedI386AddressWithOffset(uint64_t Base,
                                                    uint64_t Offset) {
  auto SignedBase = checkedI386Address(Base);
  auto SignedOffset = checkedI386Address(Offset);
  int64_t Result = 0;
  if (!SignedBase || !SignedOffset ||
      llvm::AddOverflow(*SignedBase, *SignedOffset, Result))
    return std::nullopt;
  return Result;
}

std::optional<int64_t> readI386RelocationField(llvm::ArrayRef<uint8_t> Data,
                                               uint64_t Offset, uint8_t Width,
                                               bool SignedValue) {
  if (!rangeInBounds(Offset, Width, Data.size()))
    return std::nullopt;
  const uint8_t *Field = Data.data() + Offset;
  if (SignedValue) {
    switch (Width) {
    case 1:
      return readLE<int8_t>(Field);
    case 2:
      return readLE<int16_t>(Field);
    case 4:
      return readLE<int32_t>(Field);
    default:
      return std::nullopt;
    }
  }
  switch (Width) {
  case 1:
    return readLE<uint8_t>(Field);
  case 2:
    return readLE<uint16_t>(Field);
  case 4:
    return readLE<uint32_t>(Field);
  default:
    return std::nullopt;
  }
}

std::optional<I386MappedSection>
mappedI386SectionByNumber(llvm::ArrayRef<SectionRef> ObjectSections,
                          const BinaryImage &Img, uint32_t SectionNumber) {
  if (SectionNumber == llvm::MachO::R_ABS ||
      SectionNumber > ObjectSections.size() ||
      SectionNumber > Img.Sections.size())
    return std::nullopt;
  size_t Index = SectionNumber - 1;
  auto OriginalBase = checkedI386Address(ObjectSections[Index].getAddress());
  auto FinalBase = checkedI386Address(Img.Sections[Index].VA);
  if (!OriginalBase || !FinalBase)
    return std::nullopt;
  return I386MappedSection{*OriginalBase, *FinalBase,
                           ObjectSections[Index].getSize()};
}

std::optional<I386MappedSection>
mappedI386SectionForAddress(llvm::ArrayRef<SectionRef> ObjectSections,
                            const BinaryImage &Img, uint64_t Address) {
  for (size_t I = 0; I < ObjectSections.size() && I < Img.Sections.size();
       ++I) {
    uint64_t Base = ObjectSections[I].getAddress();
    uint64_t Size = ObjectSections[I].getSize();
    if (Address < Base || Address - Base >= Size)
      continue;
    auto OriginalBase = checkedI386Address(Base);
    auto FinalBase = checkedI386Address(Img.Sections[I].VA);
    if (!OriginalBase || !FinalBase)
      return std::nullopt;
    return I386MappedSection{*OriginalBase, *FinalBase, Size};
  }
  return std::nullopt;
}

std::optional<int64_t> resolveI386ExternalSymbol(
    const MachOObjectFile &Obj, const llvm::MachO::any_relocation_info &Info,
    llvm::ArrayRef<SectionRef> ObjectSections, const BinaryImage &Img) {
  uint32_t SymbolIndex = Obj.getPlainRelocationSymbolNum(Info);
  llvm::MachO::symtab_command Symtab = Obj.getSymtabLoadCommand();
  if (SymbolIndex >= Symtab.nsyms)
    return std::nullopt;

  auto Sym = Obj.getSymbolByIndex(SymbolIndex);
  llvm::object::DataRefImpl DRI = Sym->getRawDataRefImpl();
  llvm::MachO::nlist Entry = Obj.getSymbolTableEntry(DRI);
  if (Entry.n_type & llvm::MachO::N_STAB)
    return std::nullopt;

  uint8_t Type = Entry.n_type & llvm::MachO::N_TYPE;
  if (Type == llvm::MachO::N_ABS)
    return checkedI386Address(Entry.n_value);
  if (Type != llvm::MachO::N_SECT)
    return std::nullopt;

  auto TargetSection =
      mappedI386SectionByNumber(ObjectSections, Img, Entry.n_sect);
  if (!TargetSection || Entry.n_value < uint64_t(TargetSection->OriginalBase))
    return std::nullopt;
  uint64_t Offset = Entry.n_value - uint64_t(TargetSection->OriginalBase);
  if (Offset > TargetSection->Size)
    return std::nullopt;
  return checkedI386AddressWithOffset(uint64_t(TargetSection->FinalBase),
                                      Offset);
}

I386AddressClass classifyI386Address(const BinaryImage &Img, uint64_t Address) {
  if (const Section *Sec = Img.getSectionFor(Address))
    return {true, Sec->isReadable(), Sec->isWritable(), Sec->isExecutable(),
            !Sec->Data.empty()};
  if (const Segment *Seg = Img.getSegmentFor(Address))
    return {true, Seg->isReadable(), Seg->isWritable(), Seg->isExecutable(),
            !Seg->Data.empty()};
  return {};
}

void recordI386RelocationProvenance(BinaryImage &Img, uint64_t Place,
                                    uint64_t Target, bool IsPCRel) {
  I386AddressClass TargetClass = classifyI386Address(Img, Target);
  if (!TargetClass.IsValid)
    return;
  I386AddressClass PlaceClass = classifyI386Address(Img, Place);

  if (TargetClass.IsWritable && !TargetClass.IsExecutable)
    Img.WritableRelocDataAddrs.insert(Target);

  bool IsReadOnlyData = TargetClass.IsReadable && !TargetClass.IsWritable &&
                        !TargetClass.IsExecutable && TargetClass.HasData;
  if (IsPCRel) {
    bool IsReadOnlyDataSlot = PlaceClass.IsValid && PlaceClass.IsReadable &&
                              !PlaceClass.IsWritable &&
                              !PlaceClass.IsExecutable;
    if (IsReadOnlyDataSlot && TargetClass.IsExecutable)
      Img.RelCodeRelocSlots.insert(Place);
    if (PlaceClass.IsValid && PlaceClass.IsExecutable && IsReadOnlyData)
      Img.RelCodeTableAnchors.insert(Target);
    return;
  }

  if (TargetClass.IsExecutable) {
    Img.CodePtrRelocSlots.insert(Place);
    Img.CodeRefTargets.insert(Target);
  } else if (IsReadOnlyData) {
    Img.DataPtrRelocSlots.insert(Place);
    Img.RelocDataAddrs.insert(Target);
  }
}

void recordI386SectionDifferenceDataProvenance(BinaryImage &Img,
                                               uint64_t SectionTarget,
                                               uint64_t ResolvedTarget) {
  I386AddressClass SectionTargetClass = classifyI386Address(Img, SectionTarget);
  if (!SectionTargetClass.IsValid || SectionTargetClass.IsExecutable)
    return;

  I386AddressClass ResolvedTargetClass =
      classifyI386Address(Img, ResolvedTarget);
  if (ResolvedTargetClass.IsWritable && !ResolvedTargetClass.IsExecutable) {
    Img.WritableRelocDataAddrs.insert(ResolvedTarget);
    return;
  }
  if (ResolvedTargetClass.IsReadable && !ResolvedTargetClass.IsWritable &&
      !ResolvedTargetClass.IsExecutable && ResolvedTargetClass.HasData)
    Img.RelocDataAddrs.insert(ResolvedTarget);
}

Segment *findI386ApplySegment(BinaryImage &Img, const Section &Sec) {
  for (Segment &Seg : Img.Segments)
    if (Seg.contains(Sec.VA) && !Seg.Data.empty())
      return &Seg;
  return nullptr;
}

void applyI386SectionRelocations(const MachOObjectFile &Obj,
                                 const SectionRef &SecRef, size_t SectionIndex,
                                 llvm::ArrayRef<SectionRef> ObjectSections,
                                 BinaryImage &Img) {
  using namespace llvm::MachO;
  using namespace detail;

  if (SectionIndex >= Img.Sections.size())
    return;
  const Section &ImgSec = Img.Sections[SectionIndex];
  Segment *ApplySeg = findI386ApplySegment(Img, ImgSec);
  if (!ApplySeg || ImgSec.VA < ApplySeg->VA)
    return;
  uint64_t SectionOffset = ImgSec.VA - ApplySeg->VA;

  std::vector<RelocationRef> Relocations;
  for (const RelocationRef &Reloc : SecRef.relocations())
    Relocations.push_back(Reloc);

  for (size_t I = 0; I < Relocations.size(); ++I) {
    auto Info = Obj.getRelocation(Relocations[I].getRawDataRefImpl());
    uint32_t Type = Obj.getAnyRelocationType(Info);
    uint64_t RelocAddress = Obj.getAnyRelocationAddress(Info);
    bool IsScattered = Obj.isRelocationScattered(Info);

    bool IsDifference =
        Type == GENERIC_RELOC_SECTDIFF || Type == GENERIC_RELOC_LOCAL_SECTDIFF;
    if (Type != GENERIC_RELOC_VANILLA && !IsDifference) {
      diagnoseI386Relocation("unsupported or orphan relocation type",
                             SecRef.getAddress(), RelocAddress);
      continue;
    }

    if (IsDifference) {
      bool IsPCRel = Obj.getAnyRelocationPCRel(Info);
      if (!IsScattered || IsPCRel || I + 1 >= Relocations.size()) {
        diagnoseI386Relocation("malformed section-difference relocation",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      auto PairInfo = Obj.getRelocation(Relocations[I + 1].getRawDataRefImpl());
      if (Obj.getAnyRelocationType(PairInfo) != GENERIC_RELOC_PAIR) {
        diagnoseI386Relocation("section difference has no adjacent PAIR",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      ++I;

      uint32_t Length = Obj.getAnyRelocationLength(Info);
      if (Length > 2) {
        diagnoseI386Relocation("invalid encoded width", SecRef.getAddress(),
                               RelocAddress);
        continue;
      }
      if (!Obj.isRelocationScattered(PairInfo) ||
          Obj.getAnyRelocationAddress(PairInfo) != 0 ||
          Obj.getAnyRelocationLength(PairInfo) != Length ||
          Obj.getAnyRelocationPCRel(PairInfo) != IsPCRel) {
        diagnoseI386Relocation("malformed section-difference PAIR metadata",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      uint8_t Width = uint8_t(1u << Length);
      if (!rangeInBounds(RelocAddress, Width, SecRef.getSize()) ||
          !rangeInBounds(RelocAddress, Width, ImgSec.Size) ||
          RelocAddress > InvalidVA - SectionOffset) {
        diagnoseI386Relocation("section-difference field is out of bounds",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      uint64_t SegmentOffset = SectionOffset + RelocAddress;
      auto Existing =
          readI386RelocationField(ApplySeg->Data, SegmentOffset, Width, true);
      if (!Existing) {
        diagnoseI386Relocation("section-difference field is unavailable",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }

      uint64_t EncodedA = Obj.getScatteredRelocationValue(Info);
      uint64_t EncodedB = Obj.getScatteredRelocationValue(PairInfo);
      auto SectionA =
          mappedI386SectionForAddress(ObjectSections, Img, EncodedA);
      auto SectionB =
          mappedI386SectionForAddress(ObjectSections, Img, EncodedB);
      if (!SectionA || !SectionB) {
        diagnoseI386Relocation("section-difference target is unresolved",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      auto FinalA = checkedI386AddressWithOffset(
          uint64_t(SectionA->FinalBase),
          EncodedA - uint64_t(SectionA->OriginalBase));
      auto FinalB = checkedI386AddressWithOffset(
          uint64_t(SectionB->FinalBase),
          EncodedB - uint64_t(SectionB->OriginalBase));
      auto SignedA = checkedI386Address(EncodedA);
      auto SignedB = checkedI386Address(EncodedB);
      if (!FinalA || !FinalB || !SignedA || !SignedB) {
        diagnoseI386Relocation("section-difference address overflows",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      auto Value = evaluateI386SectionDifference(*FinalA, *FinalB, *SignedA,
                                                 *SignedB, *Existing);
      if (!Value || !writeI386RelocationField(ApplySeg->Data, SegmentOffset,
                                              Width, *Value, true)) {
        diagnoseI386Relocation("section-difference result does not fit",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      int64_t ResolvedTarget = 0;
      if (!llvm::AddOverflow(*FinalB, *Value, ResolvedTarget) &&
          ResolvedTarget >= 0)
        recordI386SectionDifferenceDataProvenance(
            Img, static_cast<uint64_t>(*FinalA),
            static_cast<uint64_t>(ResolvedTarget));
      continue;
    }

    uint32_t Length = Obj.getAnyRelocationLength(Info);
    if (Length > 2) {
      diagnoseI386Relocation("invalid encoded width", SecRef.getAddress(),
                             RelocAddress);
      continue;
    }
    uint8_t Width = uint8_t(1u << Length);
    if (!rangeInBounds(RelocAddress, Width, SecRef.getSize()) ||
        !rangeInBounds(RelocAddress, Width, ImgSec.Size) ||
        RelocAddress > InvalidVA - SectionOffset ||
        ImgSec.VA > InvalidVA - RelocAddress) {
      diagnoseI386Relocation("vanilla field is out of bounds",
                             SecRef.getAddress(), RelocAddress);
      continue;
    }
    uint64_t SegmentOffset = SectionOffset + RelocAddress;
    bool IsPCRel = Obj.getAnyRelocationPCRel(Info);
    auto Existing =
        readI386RelocationField(ApplySeg->Data, SegmentOffset, Width, IsPCRel);
    if (!Existing) {
      diagnoseI386Relocation("vanilla field is unavailable",
                             SecRef.getAddress(), RelocAddress);
      continue;
    }

    int64_t Target = 0;
    int64_t Addend = *Existing;
    if (IsScattered) {
      uint64_t EncodedTarget = Obj.getScatteredRelocationValue(Info);
      auto TargetSection =
          mappedI386SectionForAddress(ObjectSections, Img, EncodedTarget);
      if (!TargetSection ||
          llvm::SubOverflow(Addend, TargetSection->OriginalBase, Addend)) {
        diagnoseI386Relocation("scattered vanilla target is unresolved",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      Target = TargetSection->FinalBase;
    } else if (Obj.getPlainRelocationExternal(Info)) {
      auto ExternalTarget =
          resolveI386ExternalSymbol(Obj, Info, ObjectSections, Img);
      if (!ExternalTarget) {
        diagnoseI386Relocation("external symbol is unresolved",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      Target = *ExternalTarget;
    } else {
      auto TargetSection = mappedI386SectionByNumber(
          ObjectSections, Img, Obj.getPlainRelocationSymbolNum(Info));
      if (!TargetSection ||
          llvm::SubOverflow(Addend, TargetSection->OriginalBase, Addend)) {
        diagnoseI386Relocation("local target section is unresolved",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
      Target = TargetSection->FinalBase;
    }

    if (!IsScattered && IsPCRel) {
      auto OriginalPlace =
          checkedI386AddressWithOffset(SecRef.getAddress(), RelocAddress);
      int64_t OriginalNextPC = 0;
      if (!OriginalPlace ||
          llvm::AddOverflow(*OriginalPlace, int64_t(Width), OriginalNextPC) ||
          llvm::AddOverflow(Addend, OriginalNextPC, Addend)) {
        diagnoseI386Relocation("pcrel old-place adjustment overflows",
                               SecRef.getAddress(), RelocAddress);
        continue;
      }
    }

    int64_t ResolvedTarget = 0;
    if (llvm::AddOverflow(Target, Addend, ResolvedTarget)) {
      diagnoseI386Relocation("resolved target overflows", SecRef.getAddress(),
                             RelocAddress);
      continue;
    }
    uint64_t Place = ImgSec.VA + RelocAddress;
    auto Value = evaluateI386Vanilla({Target, Addend, Place, Width, IsPCRel});
    if (!Value || !writeI386RelocationField(ApplySeg->Data, SegmentOffset,
                                            Width, *Value, IsPCRel)) {
      diagnoseI386Relocation("vanilla result does not fit", SecRef.getAddress(),
                             RelocAddress);
      continue;
    }
    recordI386RelocationProvenance(
        Img, Place, static_cast<uint64_t>(ResolvedTarget), IsPCRel);
  }
}

void applyI386ObjectRelocations(const MachOObjectFile &Obj, BinaryImage &Img) {
  std::vector<SectionRef> ObjectSections;
  for (const SectionRef &Sec : Obj.sections())
    ObjectSections.push_back(Sec);
  for (size_t I = 0; I < ObjectSections.size(); ++I)
    applyI386SectionRelocations(Obj, ObjectSections[I], I, ObjectSections, Img);
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

void applyObjectRelocations(const llvm::object::MachOObjectFile &Obj,
                            BinaryImage &Img) {
  using namespace llvm::MachO;

  if (Img.Arch == Arch::X86) {
    applyI386ObjectRelocations(Obj, Img);
    synchronizeObjectSectionData(Img);
    return;
  }

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
    for (const auto &Reloc : SecRef.relocations()) {
      uint64_t RAddr = Reloc.getOffset();
      uint32_t RType = Reloc.getType();
      auto SymIt = Reloc.getSymbol();
      uint64_t SymVal = 0;
      if (SymIt != Obj.symbol_end()) {
        auto AddrOrErr = SymIt->getAddress();
        if (AddrOrErr)
          SymVal = *AddrOrErr;
        else
          llvm::consumeError(AddrOrErr.takeError());
      }
      uint64_t SectionOff = SecAddr - ApplySeg->VA;
      if (RAddr > InvalidVA - SectionOff || RAddr > InvalidVA - SecAddr)
        continue;
      uint64_t SegOff = SectionOff + RAddr;
      if (SegOff >= ApplySeg->Data.size())
        continue;
      va_t S = SymVal;
      va_t P = SecAddr + RAddr;
      if (Img.Arch == Arch::X64) {
        if (RType == X86_64_RELOC_SIGNED || RType == X86_64_RELOC_BRANCH ||
            RType == X86_64_RELOC_GOT_LOAD || RType == X86_64_RELOC_GOT) {
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            continue;
          int32_t Existing;
          std::memcpy(&Existing, ApplySeg->Data.data() + SegOff, 4);
          int32_t Val = static_cast<int32_t>(S + Existing - P);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 4);
        } else if (RType == X86_64_RELOC_UNSIGNED) {
          if (!rangeInBounds(SegOff, 8, ApplySeg->Data.size()))
            continue;
          uint64_t Val = S;
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 8);
        }
      } else if (Img.Arch == Arch::AArch64) {
        if (RType == ARM64_RELOC_BRANCH26) {
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            continue;
          int64_t Disp = static_cast<int64_t>(S - P);
          uint32_t Insn;
          std::memcpy(&Insn, ApplySeg->Data.data() + SegOff, 4);
          Insn = (Insn & 0xFC000000u) |
                 (static_cast<uint32_t>((Disp >> 2) & 0x03FFFFFFu));
          std::memcpy(ApplySeg->Data.data() + SegOff, &Insn, 4);
        } else if (RType == ARM64_RELOC_PAGE21 ||
                   RType == ARM64_RELOC_GOT_LOAD_PAGE21) {
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            continue;
          int64_t PageDelta =
              static_cast<int64_t>((S & ~0xFFFULL) - (P & ~0xFFFULL));
          uint32_t ImmLo = static_cast<uint32_t>((PageDelta >> 12) & 0x3);
          uint32_t ImmHi = static_cast<uint32_t>((PageDelta >> 14) & 0x7FFFF);
          uint32_t Insn;
          std::memcpy(&Insn, ApplySeg->Data.data() + SegOff, 4);
          Insn = (Insn & 0x9F00001Fu) | (ImmLo << 29) | (ImmHi << 5);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Insn, 4);
        } else if (RType == ARM64_RELOC_PAGEOFF12 ||
                   RType == ARM64_RELOC_GOT_LOAD_PAGEOFF12) {
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            continue;
          uint32_t Imm12 = static_cast<uint32_t>(S & 0xFFF);
          uint32_t Insn;
          std::memcpy(&Insn, ApplySeg->Data.data() + SegOff, 4);
          Insn = (Insn & 0xFFC003FFu) | ((Imm12 & 0xFFF) << 10);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Insn, 4);
        }
      } else if (Img.Arch == Arch::ARM) {
        if (RType == ARM_RELOC_VANILLA) {
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            continue;
          uint32_t Val = static_cast<uint32_t>(S);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 4);
        }
      }
    }
  }
  synchronizeObjectSectionData(Img);
}

} // namespace neverd::macho_loader

#undef DEBUG_TYPE
