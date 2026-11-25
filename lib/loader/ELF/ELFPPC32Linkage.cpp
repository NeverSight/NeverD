//===- ELFPPC32Linkage.cpp - PPC32 System V PLT bindings ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read the runtime jump table and the executable transfers through it.
/// A BSS-PLT relocation owns instructions. A secure PLT relocation owns a
/// pointer, and a PIC stub selects that pointer with its caller's r30.
///
//===----------------------------------------------------------------------===//

#include "ELFIntegerProfile.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/ELFProgramMetadata.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/loader/RelocationProvenance.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/AddressRanges.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <map>
#include <optional>

namespace neverd::elf_loader {
namespace {
using namespace ppc_elf;

uint64_t bssEntryOffset(uint64_t Index) {
  const auto Short = std::min<uint64_t>(Index, BSSPLTShortEntries);
  return BSSPLTHeaderSize + Short * BSSPLTShortSize +
         (Index - Short) * BSSPLTLongSize;
}

// The runtime relocations that can write linkage code, literals or slots.
// Ordinary PPC32 dynamic relocations fit in one word. COPY takes its bound
// from the referencing symbol, and the classic PLT can install four words.
uint64_t dynamicWriteSize(const RelocationEntry &R, bool Secure) {
  using namespace llvm::ELF;
  switch (R.Type) {
  case R_PPC_NONE:
    return 0;
  case R_PPC_COPY:
    return R.ELF->Symbol ? R.ELF->Symbol->Size : 0;
  case R_PPC_JMP_SLOT:
    return Secure ? PPC32WordSize : BSSPLTLongSize;
#define NEVERD_PPC32_HALFWORD_WRITE(Type) case Type:
#include "ELFDynamicRelocations.def"
    return sizeof(uint16_t);
  default:
    return PPC32WordSize;
  }
}

bool overlapsWrite(const llvm::AddressRanges &Writes, va_t Address,
                   uint64_t Size) {
  auto It = llvm::partition_point(
      Writes, [=](const llvm::AddressRange &R) { return R.end() <= Address; });
  return It != Writes.end() && It->start() < Address + Size;
}

void recordPICLiteral(BinaryImage &Image, const Segment &Seg, size_t Off,
                      size_t Size, bool Little,
                      const llvm::AddressRanges &Writes,
                      std::optional<RelocationProvenanceIndex> &Fixups) {
  // Clang computes an input object's GOT base from the saved PC and a word
  // in .text. SSA must still prove which address a given lwz reads.
  if (!Seg.isReadable() || Seg.isWritable())
    return;
  const auto *PC = Seg.Data.data() + Off + InstructionAlignment;
  const auto Move = readTargetEndian<uint32_t>(PC, Little);
  const auto Load =
      readTargetEndian<uint32_t>(PC + InstructionAlignment, Little);
  const unsigned Reg = (Move >> RTShift) & RegisterMask;
  if ((Move & ~RTMask) != ReadLR || !Reg ||
      (Load & PrimaryOpcodeMask) != LoadWordOpcode ||
      ((Load >> ImmediateBits) & RegisterMask) != Reg)
    return;
  const uint32_t Literal = Seg.VA + Off + InstructionAlignment +
                           llvm::SignExtend32<ImmediateBits>(Load);
  if (Literal < Seg.VA || Literal % PPC32WordSize ||
      !rangeInBounds(Literal - Seg.VA, PPC32WordSize, Size) ||
      !Image.isCodeRange(Literal, PPC32WordSize) ||
      overlapsWrite(Writes, Literal, PPC32WordSize) ||
      overlapsWrite(Writes, Seg.VA + Off, 3 * InstructionAlignment))
    return;
  if (!Fixups)
    Fixups.emplace(Image);
  if (!hasRelocationProvenanceIn(
          Image, Literal >= PPC32WordSize ? Literal - PPC32WordSize + 1 : 0,
          va_t(Literal) + PPC32WordSize, &*Fixups))
    Image.PPC32PICLiterals.emplace(
        Literal, readTargetEndian<uint32_t>(
                     Seg.Data.data() + (Literal - Seg.VA), Little));
}

void recordLinkageCode(BinaryImage &Image, const llvm::AddressRanges &Writes) {
  const bool Little = archLittleEndian(Image.Arch);
  std::optional<RelocationProvenanceIndex> Fixups;
  for (const auto &Seg : Image.Segments) {
    if (!Seg.isExecutable())
      continue;
    const size_t Size = std::min<uint64_t>(Seg.FileSz, Seg.Data.size());
    auto Word = [&](size_t Off) {
      return readTargetEndian<uint32_t>(Seg.Data.data() + Off, Little);
    };
    for (size_t Off = (-Seg.VA) % InstructionAlignment;
         Off + 3 * InstructionAlignment <= Size; Off += InstructionAlignment) {
      const uint32_t First = Word(Off);
      if (First == LinkToNext || First == (BranchLink | InstructionAlignment)) {
        recordPICLiteral(Image, Seg, Off, Size, Little, Writes, Fixups);
        continue;
      }
      if (Image.PPC32PLTSlots.empty())
        continue;
      const auto Op = First & DFormMask;
      const bool Absolute = Op == LisR11;
      const bool Near = Op == LoadWordR11R30;
      if (!Near && Op != AddisR11R30 && !(Absolute && Image.LoadsAtLinkAddress))
        continue;
      uint32_t Displacement = llvm::SignExtend32<ImmediateBits>(First);
      size_t Next = Off + InstructionAlignment;
      if (!Near) {
        if (Next + 3 * InstructionAlignment > Size)
          continue;
        const auto Load = Word(Next);
        if ((Load & DFormMask) != LoadWordR11R11)
          continue;
        Displacement =
            (First << ImmediateBits) + llvm::SignExtend32<ImmediateBits>(Load);
        Next += InstructionAlignment;
      }
      const va_t Stub = Seg.VA + Off;
      const size_t Extent = Next - Off + 2 * InstructionAlignment;
      if (Word(Next) != MoveR11ToCTR ||
          Word(Next + InstructionAlignment) != BranchCTR ||
          !Image.isCodeRange(Stub, Extent) ||
          overlapsWrite(Writes, Stub, Extent))
        continue;
      if (Absolute) {
        auto Slot = Image.PPC32PLTSlots.find(Displacement);
        if (Slot != Image.PPC32PLTSlots.end())
          Image.recordImportStub(Stub, Slot->second);
      } else {
        Image.PPC32PLTGOTDisplacements.emplace(Stub, Displacement);
      }
    }
  }
}
} // namespace

llvm::Expected<PPC32PLTTable> readPPC32PLTTable(const BinaryImage &Image) {
  using namespace llvm::ELF;
  auto Dynamic = readELFProgramDynamicTable(Image);
  if (!Dynamic)
    return Dynamic.takeError();
  std::optional<va_t> JumpRel, JumpSize, JumpType, PLT, GOT;
  for (const auto &[Tag, Value] : *Dynamic) {
    std::optional<va_t> *Field;
    switch (Tag) {
    case DT_JMPREL:
      Field = &JumpRel;
      break;
    case DT_PLTRELSZ:
      Field = &JumpSize;
      break;
    case DT_PLTREL:
      Field = &JumpType;
      break;
    case DT_PLTGOT:
      Field = &PLT;
      break;
    case DT_PPC_GOT:
      Field = &GOT;
      break;
    default:
      continue;
    }
    if (*Field)
      return llvm::createStringError("elf: duplicate PPC32 PLT dynamic tag");
    *Field = Value;
  }
  if (!JumpRel && !JumpSize && !JumpType)
    return PPC32PLTTable{};
  constexpr size_t RelaSize = sizeof(llvm::object::ELF32BE::Rela);
  if (!JumpRel || !JumpSize || JumpType != DT_RELA || !PLT ||
      *PLT % PPC32WordSize || *JumpSize % RelaSize)
    return llvm::createStringError("elf: invalid PPC32 PLT relocation table");

  // DT_PPC_GOT selects the secure ABI even when no GOT was allocated.
  return PPC32PLTTable{*PLT, *JumpRel, *JumpSize / RelaSize, GOT.has_value()};
}

llvm::Error readPPC32Linkage(BinaryImage &Image, const PPC32PLTTable &Table) {
  using namespace llvm::ELF;
  std::map<va_t, unsigned> Entries;
  for (const auto &R : Image.Relocations) {
    if (!R.ELF || R.ELF->Source != ELFRelocationSource::ProgramDynamicTable ||
        R.ELF->TableVirtualAddress != Table.Relocations ||
        R.Type != R_PPC_JMP_SLOT)
      continue;
    const uint64_t Index = R.ELF->Ordinal;
    const unsigned Width = Table.Secure                 ? PPC32WordSize
                           : Index < BSSPLTShortEntries ? BSSPLTShortSize
                                                        : BSSPLTLongSize;
    const auto *Seg = Image.getSegmentFor(R.Address);
    if (Index >= Table.Count || R.Address % PPC32WordSize || !Seg ||
        !Seg->isWritable() ||
        !rangeInBounds(R.Address - Seg->VA, Width, Seg->Size) ||
        (Table.Secure
             ? R.Address < Table.Address ||
                   R.Address - Table.Address >= Table.Count * PPC32WordSize ||
                   !Image.isDataAddress(R.Address)
             : R.Address != Table.Address + bssEntryOffset(Index) ||
                   !Image.isCodeRange(R.Address, Width)))
      return llvm::createStringError("elf: invalid PPC32 PLT entry extent");
    if (!Entries.try_emplace(R.Address, Width).second)
      return llvm::createStringError("elf: duplicate PPC32 PLT entry");

    const auto &Sym = R.ELF->Symbol;
    if (!R.HasExplicitAddend || R.Addend || !Sym || !Sym->isFunction() ||
        !Sym->Name || Sym->Name->empty() ||
        (Sym->Binding != STB_GLOBAL && Sym->Binding != STB_WEAK) ||
        (Sym->Other & SymbolVisibilityMask) != STV_DEFAULT)
      continue;
    const size_t Import = Image.Imports.size();
    Image.Imports.push_back(
        {kExternModule.str(), *Sym->Name, 0, Table.Secure ? R.Address : 0});
    if (Table.Secure) {
      Image.PPC32PLTSlots.emplace(R.Address, Import);
      Image.recordImportStorageSlot(R.Address, *Sym->Name, 0,
                                    ImportStorageEvidence::LoaderBind);
    } else if (!Image.recordImportStub(R.Address, Import)) {
      return llvm::createStringError("elf: conflicting PPC32 BSS-PLT entry");
    }
  }
  if (Entries.empty())
    return llvm::Error::success();
  llvm::SmallVector<llvm::AddressRange> Fields;
  for (const auto &R : Image.Relocations) {
    if (!R.ELF || R.ELF->Source != ELFRelocationSource::ProgramDynamicTable ||
        (R.Type == R_PPC_JMP_SLOT &&
         R.ELF->TableVirtualAddress == Table.Relocations))
      continue;
    if (auto Width = dynamicWriteSize(R, Table.Secure))
      Fields.emplace_back(R.Address, R.Address + Width);
  }
  llvm::sort(Fields);
  llvm::AddressRanges Writes;
  for (const auto &Field : Fields)
    Writes.insert(Field);
  for (const auto &[Start, Width] : Entries)
    if (overlapsWrite(Writes, Start, Width))
      return llvm::createStringError(
          "elf: conflicting writer to PPC32 PLT entry");
  recordLinkageCode(Image, Writes);
  return llvm::Error::success();
}
} // namespace neverd::elf_loader
