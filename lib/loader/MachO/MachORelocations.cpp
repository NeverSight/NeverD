//===- MachORelocations.cpp - Mach-O object relocations ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MachORelocationsDetail.h"

#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"

#include <cstring>

namespace neverd::macho_loader {

namespace {

va_t symbolOwnerVA(const llvm::object::MachOObjectFile &Obj,
                   llvm::object::symbol_iterator SymIt,
                   const BinaryImage &Img) {
  if (SymIt == Obj.symbol_end())
    return InvalidVA;
  llvm::object::DataRefImpl DRI = SymIt->getRawDataRefImpl();
  uint8_t Type = 0;
  uint8_t SectionNumber = 0;
  if (Obj.is64Bit()) {
    llvm::MachO::nlist_64 Entry = Obj.getSymbol64TableEntry(DRI);
    Type = Entry.n_type & llvm::MachO::N_TYPE;
    SectionNumber = Entry.n_sect;
  } else {
    llvm::MachO::nlist Entry = Obj.getSymbolTableEntry(DRI);
    Type = Entry.n_type & llvm::MachO::N_TYPE;
    SectionNumber = Entry.n_sect;
  }
  if (Type != llvm::MachO::N_SECT || SectionNumber == 0 ||
      SectionNumber > Img.Sections.size())
    return InvalidVA;
  return Img.Sections[SectionNumber - 1].VA;
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
      uint64_t SymOwnerVA = symbolOwnerVA(Obj, SymIt, Img);
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
        if (RType == X86_64_RELOC_SIGNED || RType == X86_64_RELOC_BRANCH ||
            RType == X86_64_RELOC_GOT_LOAD || RType == X86_64_RELOC_GOT) {
          if (!rangeInBounds(SegOff, 4, ApplySeg->Data.size()))
            continue;
          int32_t Existing;
          std::memcpy(&Existing, ApplySeg->Data.data() + SegOff, 4);
          int32_t Val = static_cast<int32_t>(S + Existing - P);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 4);
          if (RType == X86_64_RELOC_SIGNED)
            RecordRelDataPtr();
        } else if (RType == X86_64_RELOC_UNSIGNED) {
          if (!rangeInBounds(SegOff, 8, ApplySeg->Data.size()))
            continue;
          uint64_t Addend = 0;
          std::memcpy(&Addend, ApplySeg->Data.data() + SegOff, 8);
          uint64_t Val = S + Addend;
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 8);
          if (SymOwnerVA != InvalidVA)
            recordAbsolutePointerRelocation(Img, P, Val, SymOwnerVA);
        }
      } else if (Img.Arch == Arch::AArch64) {
        if (RType == ARM64_RELOC_UNSIGNED) {
          if (!rangeInBounds(SegOff, 8, ApplySeg->Data.size()))
            continue;
          uint64_t Addend = 0;
          std::memcpy(&Addend, ApplySeg->Data.data() + SegOff, 8);
          uint64_t Val = S + Addend;
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 8);
          if (SymOwnerVA != InvalidVA)
            recordAbsolutePointerRelocation(Img, P, Val, SymOwnerVA);
        } else if (RType == ARM64_RELOC_BRANCH26) {
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
          uint32_t Addend = 0;
          std::memcpy(&Addend, ApplySeg->Data.data() + SegOff, 4);
          uint32_t Val = static_cast<uint32_t>(S + Addend);
          std::memcpy(ApplySeg->Data.data() + SegOff, &Val, 4);
          if (SymOwnerVA != InvalidVA)
            recordAbsolutePointerRelocation(Img, P, Val, SymOwnerVA);
        }
      }
    }
  }
  synchronizeObjectSectionData(Img);
}

} // namespace neverd::macho_loader
