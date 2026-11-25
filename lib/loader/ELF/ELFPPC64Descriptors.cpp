//===- ELFPPC64Descriptors.cpp - ELFv1 function descriptors --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolve linked PPC64 function descriptors and index their entry contexts.
///
//===----------------------------------------------------------------------===//

#include "ELFIntegerProfile.h"

#include "neverd/ArchSupport.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/ELF/PPC64FunctionDescriptor.h"
#include "neverd/loader/ELF/PowerPCABI.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/ELF.h"

#include <algorithm>
#include <vector>

namespace neverd::elf_loader {

llvm::Error readPPC64Descriptors(BinaryImage &Image, PPC64LinkageTables Table) {
  using namespace ppc_elf;
  const va_t Start = Table.Address, End = Start + Table.Size;
  const auto Contains = [&](va_t VA) { return VA >= Start && VA < End; };
  const auto *Seg = Image.getSegmentFor(Start);
  const auto *Sec = Image.getSectionByName(DescriptorSection);
  const auto *Data = Image.readVA(Start, Table.Size);
  if (!Seg || !Seg->isReadable() || !Data ||
      !rangeInBounds(Start - Seg->VA, Table.Size, Seg->FileSz) ||
      (Sec ? Sec->FileOff != Seg->FileOff + (Start - Seg->VA)
           : Seg->isExecutable()))
    return llvm::createStringError("elf: unmapped PPC64 descriptor table");

  const size_t Count = Table.Size / WordSize;
  const bool Little = archLittleEndian(Image.Arch);
  auto Word = [&](size_t I) {
    return readPtr(Data + I * WordSize, true, Little);
  };
  std::vector<bool> Required(Count, false);
  if (Image.Entry) {
    if (!Contains(Image.Entry) || Image.Entry % WordSize)
      return llvm::createStringError(
          "elf: PPC64 entry does not name a descriptor");
    Required[(Image.Entry - Start) / WordSize] = true;
  }
  for (const auto &Sym : Image.Symbols) {
    if (!Sym.IsFunc)
      continue;
    if (!Contains(Sym.Addr)) {
      if (!Image.isCodeRange(Sym.Addr, InstructionAlignment))
        return llvm::createStringError(
            "elf: PPC64 function symbol has no descriptor or code entry");
      continue;
    }
    if (Sym.Addr % WordSize)
      return llvm::createStringError(
          "elf: misaligned PPC64 function descriptor");
    Required[(Sym.Addr - Start) / WordSize] = true;
  }

  // A shared image normally relocates each nonzero pointer. GNU ld's
  // NO_OPD_RELOCS convention describes the whole table with DT_PPC64_OPD
  // and DT_PPC64_OPDSZ instead. Retained static relocs are not new writers.
  std::vector<bool> Rebased(Count, false);
  for (const auto &R : Image.BaseRelocations)
    if (Contains(R.Address) && R.Address % WordSize == 0)
      Rebased[(R.Address - Start) / WordSize] = true;
  std::vector<bool> Written(Count, false);
  auto LocallyBound = [&](const ELFRelocationSymbol &Sym) {
    return Sym.isDefined() &&
           (Image.LoadsAtLinkAddress || Sym.Binding == llvm::ELF::STB_LOCAL ||
            (Sym.Other & SymbolVisibilityMask) != llvm::ELF::STV_DEFAULT);
  };
  for (const auto &R : Image.Relocations) {
    if (!Contains(R.Address) || !R.ELF ||
        (R.ELF->Source == ELFRelocationSource::SectionTable &&
         !R.ELF->TableVirtualAddress))
      continue;
    const auto &Sym = R.ELF->Symbol;
    const size_t I = (R.Address - Start) / WordSize;
    const bool LocalRelative =
        R.Type == llvm::ELF::R_PPC64_RELATIVE && !R.SymbolIndex;
    const bool LocalSymbol =
        R.Type == llvm::ELF::R_PPC64_ADDR64 && Sym && LocallyBound(*Sym);
    if (R.Address % WordSize || Written[I] || !Rebased[I] ||
        !(LocalRelative || LocalSymbol))
      return llvm::createStringError(
          "elf: unresolved PPC64 descriptor relocation");
    Written[I] = true;
  }

  // GNU ld can omit an unused environment word. Work backwards from the
  // table end, retaining only splits that respect every named descriptor
  // and have a mapped, aligned instruction at each entry. A stripped file
  // can still have a unique layout. Never choose between two valid layouts.
  constexpr unsigned Short = EnvironmentOffset / WordSize;
  constexpr unsigned Full = DescriptorSize / WordSize;
  struct Suffix {
    unsigned char Ways = 0;
    unsigned char Words = 0;
  };
  std::vector<Suffix> Layout(Count + 1);
  Layout.back().Ways = 1;
  for (size_t I = Count - Short + 1; I-- > 0;) {
    if (Required[I + 1])
      continue;
    const va_t Code = Word(I);
    if (Code % InstructionAlignment || Contains(Code) ||
        (!Image.LoadsAtLinkAddress && !Table.HasDynamicBounds &&
         (!Rebased[I] || (Word(I + 1) && !Rebased[I + 1]))) ||
        !Image.isCodeRange(Code, InstructionAlignment))
      continue;
    const auto *CodeSeg = Image.getSegmentFor(Code);
    if (!CodeSeg->isExecutable() ||
        !rangeInBounds(Code - CodeSeg->VA, InstructionAlignment,
                       CodeSeg->FileSz))
      continue;
    for (unsigned Words : {Short, Full}) {
      if ((Table.EntrySize && Words * WordSize != Table.EntrySize) ||
          Words > Count - I ||
          (Words == Full &&
           (Required[I + Short] ||
            (!Image.LoadsAtLinkAddress && !Table.HasDynamicBounds &&
             Word(I + Short) && !Rebased[I + Short]))))
        continue;
      const unsigned Ways = Layout[I + Words].Ways;
      if (Ways) {
        Layout[I].Ways = std::min<unsigned>(2, Layout[I].Ways + Ways);
        Layout[I].Words = Words;
      }
    }
  }
  if (Layout[0].Ways != 1)
    return llvm::createStringError(
        Layout[0].Ways ? "elf: ambiguous PPC64 descriptor layout"
                       : "elf: invalid PPC64 descriptor or code entry");

  for (size_t I = 0; I < Count; I += Layout[I].Words) {
    const va_t Code = Word(I), TOC = Word(I + TOCOffset / WordSize);
    const auto Env =
        Layout[I].Words == Full ? std::optional(Word(I + Short)) : std::nullopt;
    Image.PPC64Descriptors.emplace(
        Start + I * WordSize,
        BinaryImage::PPC64FunctionDescriptor{Code, TOC, Env});
    auto [It, New] = Image.PPC64EntryContexts.try_emplace(
        Code, BinaryImage::PPC64EntryContext{TOC, Env, Start + I * WordSize});
    if (!New) {
      It->second.Descriptor = InvalidVA;
      if (It->second.TOC != TOC)
        It->second.TOC.reset();
      if (It->second.Environment != Env)
        It->second.Environment.reset();
    }
    Image.recordRuntimeFunction(Code);
  }

  // A function-pointer variable still holds a descriptor VA in the original
  // image. Record its callable initializer for source emission only when the
  // linker's relocation identifies that word as a pointer. Leave the bytes
  // intact so a later write, or inspection of the original file, sees storage.
  for (const auto &R : Image.Relocations) {
    if (!R.ELF || Contains(R.Address))
      continue;
    const auto &Sym = R.ELF->Symbol;
    const bool FunctionSymbol = R.Type == llvm::ELF::R_PPC64_ADDR64 && Sym &&
                                Sym->isFunction() && R.Addend == 0 &&
                                LocallyBound(*Sym);
    const bool Relative = R.Type == llvm::ELF::R_PPC64_RELATIVE &&
                          !R.SymbolIndex &&
                          Image.DataPtrRelocSlots.count(R.Address);
    if ((!FunctionSymbol && !Relative) || !Image.isDataAddress(R.Address))
      continue;
    const auto *Bytes = Image.readVA(R.Address, WordSize);
    if (!Bytes)
      continue;
    const va_t Target = readPtr(Bytes, true, Little);
    if ((!FunctionSymbol || Sym->Value == Target) &&
        nativePPC64DescriptorEntry(Image, Target)) {
      Image.DataPtrRelocSlots.erase(R.Address);
      Image.CodePtrRelocSlots.insert(R.Address);
    }
  }

  auto Resolve = [&](va_t &VA) {
    if (auto It = Image.PPC64Descriptors.find(VA);
        It != Image.PPC64Descriptors.end())
      VA = It->second.Entry;
  };
  if (Image.Entry)
    Resolve(Image.Entry);
  for (auto &Sym : Image.Symbols)
    if (Sym.IsFunc)
      Resolve(Sym.Addr);
  for (auto &Exp : Image.Exports)
    Resolve(Exp.Addr);
  return llvm::Error::success();
}

} // namespace neverd::elf_loader
