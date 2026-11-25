//===- ELFPPC64Linkage.cpp - PPC64 PLT and TOC switch stubs --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bind the executable linkage sequence to its runtime descriptor slot or
/// to the code entry whose TOC it establishes.
///
//===----------------------------------------------------------------------===//

#include "ELFIntegerProfile.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>

namespace neverd::elf_loader {
namespace {
using namespace ppc_elf;

struct Linkage {
  int64_t Displacement = 0;
  std::optional<va_t> LocalEntry;
  std::optional<va_t> LazyEntry;
  std::optional<va_t> AbsoluteSlot;
  bool SavesTOC = false;
  bool SetsGlobalEntry = false;
};

bool startsLinkage(uint32_t Word, bool V2) {
  if (V2)
    return Word == ELFv2SaveTOC || Word == SaveLRInR12 ||
           (Word & PCRelativePrefixMask) == PCRelativeLoadPrefix ||
           (Word & PCRelativePrefixMask) == PCRelativeAddPrefix ||
           (Word & DSFormMask) == LoadR12R2 || (Word & DFormMask) == AddisR12R2;
  return Word == SaveTOC || (Word & DSFormMask) == LoadR12R2 ||
         (Word & DFormMask) == AddisR11R2;
}

/// Read a single straight-line stub, including the linker's optional
/// high-adjusted address and the low adjustment when a descriptor straddles
/// a signed-displacement boundary.
std::optional<Linkage> linkageAt(const BinaryImage &Image, va_t Address,
                                 bool Little, bool V2) {
  va_t PC = Address;
  uint32_t Insn = 0;
  auto Take = [&](uint32_t Pattern, uint32_t Mask = UINT32_MAX) {
    if (!Image.isCodeRange(PC, InstructionAlignment))
      return false;
    const auto *P = Image.readVA(PC, InstructionAlignment);
    if (!P)
      return false;
    const auto Word = readTargetEndian<uint32_t>(P, Little);
    if ((Word & Mask) != Pattern)
      return false;
    Insn = Word;
    PC += InstructionAlignment;
    return true;
  };
  auto Imm = [&]() -> int64_t {
    return llvm::SignExtend64<ImmediateBits>(Insn);
  };
  auto BranchTarget = [&]() -> va_t {
    return PC - InstructionAlignment +
           llvm::SignExtend64<BranchDisplacementBits>(Insn &
                                                      BranchDisplacementMask);
  };
  auto Complete = [&]() {
    auto Next = Image.PPC64EntryContexts.upper_bound(Address);
    return Image.isCodeRange(Address, PC - Address) &&
           (Next == Image.PPC64EntryContexts.end() || Next->first >= PC);
  };
  Linkage Stub;
  Stub.SavesTOC = Take(V2 ? ELFv2SaveTOC : SaveTOC);
  if (Stub.SavesTOC) {
    bool High = Take(AddisR2R2, DFormMask);
    if (High)
      Stub.Displacement = Imm() * (int64_t(1) << ImmediateBits);
    bool Low = Take(AddiR2R2, DFormMask);
    if (Low)
      Stub.Displacement += Imm();
    if ((V2 || High || Low) && Take(Branch, BranchMask)) {
      if (!Complete())
        return std::nullopt;
      Stub.LocalEntry = BranchTarget();
      return Stub;
    }
    if (High || Low)
      return std::nullopt;
  }

  if (V2) {
    const va_t PrefixPC = PC;
    if (Take(PCRelativeLoadPrefix, PCRelativePrefixMask) ||
        Take(PCRelativeAddPrefix, PCRelativePrefixMask)) {
      const bool Load = (Insn & PCRelativePrefixMask) == PCRelativeLoadPrefix;
      uint64_t Displacement = uint64_t(Insn & PrefixHighImmediate)
                              << ImmediateBits;
      if (!Take(Load ? PLDR12Suffix : PAddiR12Suffix, DFormMask))
        return std::nullopt;
      Displacement |= Insn & ~DFormMask;
      const va_t Target =
          PrefixPC + llvm::SignExtend64<PrefixedDisplacementBits>(Displacement);
      if (Load)
        Stub.AbsoluteSlot = Target;
      else {
        Stub.LocalEntry = Target;
        Stub.SetsGlobalEntry = true;
      }
    } else if (Take(SaveLRInR12)) {
      if (!Take(LinkToNext))
        return std::nullopt;
      const va_t Base = PC;
      if (!Take(ReadLRToR11) || !Take(RestoreLRFromR12) ||
          !Take(AddisR12R11, DFormMask))
        return std::nullopt;
      const va_t High = Base + Imm() * (int64_t(1) << ImmediateBits);
      if (Take(LoadR12R12, DSFormMask))
        Stub.AbsoluteSlot = High + Imm();
      else if (Take(AddiR12R12, DFormMask)) {
        Stub.LocalEntry = High + Imm();
        Stub.SetsGlobalEntry = true;
      } else
        return std::nullopt;
    } else {
      bool Far = Take(AddisR12R2, DFormMask);
      if (Far)
        Stub.Displacement = Imm() * (int64_t(1) << ImmediateBits);
      if (!Take(Far ? LoadR12R12 : LoadR12R2, DSFormMask))
        return std::nullopt;
      Stub.Displacement += Imm();
    }
    if (!Take(MoveR12ToCTR) || !Take(BranchCTR) || !Complete())
      return std::nullopt;
    return Stub;
  }

  bool Far = Take(AddisR11R2, DFormMask);
  int64_t Base = Far ? Imm() * (int64_t(1) << ImmediateBits) : 0;
  if (!Take(Far ? LoadR12R11 : LoadR12R2, DSFormMask))
    return std::nullopt;
  Stub.Displacement = Base + Imm();
  if (Take(Far ? AddiR11R11 : AddiR2R2, DFormMask))
    Base += Imm();
  if (!Take(MoveR12ToCTR))
    return std::nullopt;
  // GNU ld can order a lazy entry/TOC pair with a register dependency.
  if (Take(Far ? XorR2R12R12 : XorR11R12R12) &&
      !Take(Far ? AddR11R11R2 : AddR2R2R11))
    return std::nullopt;
  auto Chain = [&]() {
    return !Take(Far ? LoadR11R11 : LoadR11R2, DSFormMask) ||
           Base + Imm() == Stub.Displacement + EnvironmentOffset;
  };
  if ((!Far && !Chain()) || !Take(Far ? LoadR2R11 : LoadR2R2, DSFormMask) ||
      Base + Imm() != Stub.Displacement + TOCOffset || (Far && !Chain()))
    return std::nullopt;
  if (!Take(BranchCTR)) {
    if (!Take(CompareR2Zero) || !Take(BranchCTRNotEqual) ||
        !Take(Branch, BranchMask))
      return std::nullopt;
    Stub.LazyEntry = BranchTarget();
  }
  return Complete() ? std::optional(Stub) : std::nullopt;
}

struct JumpSlot {
  std::optional<size_t> ImportIndex;
  std::optional<va_t> LazyEntry;
  bool PreservesTOC = false;
};

} // namespace

llvm::Error readPPC64Linkage(BinaryImage &Image,
                             const PPC64LinkageTables &Table) {
  using namespace ppc_elf;
  const bool V2 = isPowerPC64ELFv2(Image.Arch);
  const unsigned SlotSize = V2 ? WordSize : DescriptorSize;
  const char *SlotKind = V2 ? "slot" : "descriptor";
  std::map<va_t, JumpSlot> Slots;
  for (const auto &R : Image.Relocations) {
    if (!Table.JumpRelocationSize || !R.ELF ||
        R.ELF->Source != ELFRelocationSource::ProgramDynamicTable ||
        R.ELF->TableVirtualAddress != Table.JumpRelocations ||
        R.Type != llvm::ELF::R_PPC64_JMP_SLOT)
      continue;
    const auto &Sym = R.ELF->Symbol;
    const auto *Seg = Image.getSegmentFor(R.Address);
    if (!Seg || !Seg->isWritable() || Seg->isExecutable() ||
        (R.Address < Table.Address + Table.Size &&
         (R.Address >= Table.Address ||
          Table.Address - R.Address < SlotSize)) ||
        R.Address % WordSize ||
        !rangeInBounds(R.Address - Seg->VA, SlotSize, Seg->Size))
      return llvm::createStringError("elf: invalid PPC64 PLT %s extent",
                                     SlotKind);
    auto [Slot, Inserted] = Slots.try_emplace(R.Address);
    if (!Inserted)
      return llvm::createStringError("elf: duplicate PPC64 PLT %s", SlotKind);
    if (!R.HasExplicitAddend || R.Addend || !Sym || !Sym->isFunction() ||
        !Sym->Name || Sym->Name->empty() ||
        (Sym->Binding != llvm::ELF::STB_GLOBAL &&
         Sym->Binding != llvm::ELF::STB_WEAK) ||
        (Sym->Other & SymbolVisibilityMask) != llvm::ELF::STV_DEFAULT)
      continue;
    std::optional<va_t> Lazy;
    if (!V2 && Table.Glink) {
      const uint64_t Index = R.ELF->Ordinal;
      const uint64_t Short = std::min<uint64_t>(Index, GlinkShortEntries);
      const uint64_t Offset = GlinkFirstEntryOffset +
                              Short * GlinkShortEntrySize +
                              (Index - Short) * GlinkLongEntrySize;
      if (Offset <= InvalidVA - *Table.Glink)
        Lazy = *Table.Glink + Offset;
    }
    Slot->second = {Image.Imports.size(), Lazy,
                    V2 && Table.PLTLocalEntry &&
                        Sym->Binding == llvm::ELF::STB_GLOBAL &&
                        !(Sym->Other & llvm::ELF::STO_PPC64_LOCAL_MASK)};
    // An ELFv2 slot holds a callable pointer. ELFv1 stores the entire
    // descriptor inline, so only its veneer names a callable address.
    Image.Imports.push_back(
        {kExternModule.str(), *Sym->Name, 0, V2 ? R.Address : 0});
  }
  for (auto It = Slots.begin(); It != Slots.end(); ++It)
    if (It != Slots.begin() && It->first - std::prev(It)->first < SlotSize)
      return llvm::createStringError("elf: overlapping PPC64 PLT %ss",
                                     SlotKind);
  if (!Slots.empty())
    for (const auto &R : Image.Relocations) {
      if (!R.ELF || R.ELF->Source != ELFRelocationSource::ProgramDynamicTable ||
          !R.Type ||
          (R.ELF->TableVirtualAddress == Table.JumpRelocations &&
           R.Type == llvm::ELF::R_PPC64_JMP_SLOT))
        continue;
      auto End = Slots.upper_bound(R.Address);
      if (End != Slots.begin() && R.Address - std::prev(End)->first < SlotSize)
        return llvm::createStringError(
            "elf: conflicting writer to PPC64 PLT %s", SlotKind);
    }

  const auto &Contexts = Image.PPC64EntryContexts;
  if (V2)
    for (auto It = Contexts.begin(); It != Contexts.end(); ++It) {
      auto Next = std::next(It);
      if (Next != Contexts.end() &&
          Next->first - It->first <= It->second.LocalEntryOffset)
        return llvm::createStringError("elf: overlapping PPC64 ELFv2 entries");
    }
  if ((!V2 && Contexts.empty()) ||
      (Slots.empty() && llvm::all_of(Contexts, [&](const auto &E) {
         return E.second.TOC == Contexts.begin()->second.TOC &&
                (!V2 || E.second.PreservesTOC);
       })))
    return llvm::Error::success();

  // .text can contain stubs even after stripping. Examine only actual
  // relative branches, then check the caller's reaching r2 again in SSA.
  std::map<va_t, size_t> Imports;
  const bool Little = archLittleEndian(Image.Arch);
  for (const auto &Seg : Image.Segments) {
    if (!Seg.isExecutable())
      continue;
    const size_t Bytes = std::min<uint64_t>(Seg.FileSz, Seg.Data.size());
    for (size_t Off = (-Seg.VA) % InstructionAlignment;
         Off + InstructionAlignment <= Bytes; Off += InstructionAlignment) {
      const auto Word =
          readTargetEndian<uint32_t>(Seg.Data.data() + Off, Little);
      if ((Word & BranchMask) != BranchLink && (Word & BranchMask) != Branch)
        continue;
      const va_t Site = Seg.VA + Off;
      const va_t Dest = Site + llvm::SignExtend64<BranchDisplacementBits>(
                                   Word & BranchDisplacementMask);
      if (!V2 && Contexts.contains(Dest))
        continue;
      const auto *First = Image.readVA(Dest, InstructionAlignment);
      if (!First ||
          !startsLinkage(readTargetEndian<uint32_t>(First, Little), V2) ||
          !Image.isCodeRange(Site, InstructionAlignment))
        continue;
      auto Stub = linkageAt(Image, Dest, Little, V2);
      if (!Stub)
        continue;
      va_t TOC = InvalidVA, Target = InvalidVA;
      bool PreservesTOC = false;
      if (Stub->LocalEntry) {
        const va_t Global = Image.ppc64GlobalEntry(*Stub->LocalEntry);
        auto Callee = Contexts.find(Global);
        if (V2 && Callee != Contexts.end() && Global == *Stub->LocalEntry &&
            (Stub->SetsGlobalEntry || !Callee->second.LocalEntryOffset) &&
            !Stub->Displacement)
          Target = Global;
        else if (Callee != Contexts.end() && Callee->second.TOC &&
                 (!V2 || Global != *Stub->LocalEntry)) {
          Target = Callee->first;
          TOC = *Callee->second.TOC - Stub->Displacement;
        }
      } else {
        auto After = Contexts.upper_bound(Site);
        const auto Candidate = After == Contexts.begin()
                                   ? std::nullopt
                                   : std::prev(After)->second.TOC;
        if (Stub->AbsoluteSlot || Candidate) {
          auto Slot =
              Slots.find(Stub->AbsoluteSlot ? *Stub->AbsoluteSlot
                                            : *Candidate + Stub->Displacement);
          if (Slot != Slots.end() && Slot->second.ImportIndex &&
              (!Stub->LazyEntry ||
               (Slot->second.LazyEntry == Stub->LazyEntry &&
                Image.isCodeRange(*Stub->LazyEntry, InstructionAlignment)))) {
            Target = Dest;
            TOC = Stub->AbsoluteSlot ? InvalidVA : *Candidate;
            PreservesTOC = Slot->second.PreservesTOC;
            Imports.try_emplace(Dest, *Slot->second.ImportIndex);
          }
        }
      }
      auto [It, New] = Image.PPC64LinkageStubs.try_emplace(
          Dest, BinaryImage::PPC64LinkageStub{TOC, Target, Stub->SavesTOC,
                                              PreservesTOC});
      if (!New &&
          (It->second.IncomingTOC != TOC || It->second.Target != Target))
        It->second.Target = InvalidVA;
    }
  }
  for (auto [Stub, I] : Imports)
    if (Image.PPC64LinkageStubs.at(Stub).Target == Stub &&
        !Image.recordImportStub(Stub, I))
      return llvm::createStringError("elf: conflicting PPC64 import stub");
  return llvm::Error::success();
}

} // namespace neverd::elf_loader
