//===- ELFMIPSDynamic.cpp - MIPS dynamic GOT import identities ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// MIPS binds its global GOT suffix in dynamic-symbol order, independently of
/// the PLT relocation table. Decode that ABI-owned mapping in both byte orders.
//===----------------------------------------------------------------------===//

#include "ELFLoaderDetail.h"

#include "neverd/loader/ELF/ELFProgramMetadata.h"
#include "neverd/loader/ELF/MIPSCodeMode.h"
#include "neverd/loader/PointerRelocation.h"

#include <cstring>
#include <map>

namespace neverd::elf_loader::detail {

template <typename ELFT>
llvm::Error collectMIPSDynamicGOT(const llvm::object::ELFFile<ELFT> &ELF,
                                  llvm::ArrayRef<typename ELFT::Shdr> Sections,
                                  BinaryImage &Img) {
  using namespace llvm::ELF;
  // Runtime relocation admission is separate from the linker's retained
  // --emit-relocs records: those describe bytes already fixed in an ET_EXEC.
  std::vector<AbsolutePointerRelocation> LinkedPointers;
  for (const auto &Section : Sections) {
    const bool Runtime = Section.sh_flags & SHF_ALLOC;
    if (Runtime &&
        (Section.sh_type == SHT_RELR || Section.sh_type == SHT_ANDROID_REL ||
         Section.sh_type == SHT_ANDROID_RELA))
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "elf: unsupported MIPS packed relocation table");
    if (Section.sh_type != SHT_REL && Section.sh_type != SHT_RELA)
      continue;
    llvm::ArrayRef<typename ELFT::Sym> Symbols;
    if (Runtime && Section.sh_link < Sections.size()) {
      auto Entries = ELF.symbols(&Sections[Section.sh_link]);
      if (!Entries)
        return Entries.takeError();
      Symbols = *Entries;
    }
    const auto Check = [&](auto Entries) -> llvm::Error {
      if (!Entries)
        return Entries.takeError();
      for (const auto &Entry : *Entries) {
        const auto Type = Entry.getType(ELF.isMips64EL());
        if constexpr (ELFT::Is64Bits)
          if (Type & 0xffffff00u)
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "elf: packed MIPS64 relocations are not supported");
        if (!Runtime) {
          // A retained pointer relocation proves that this linked word is an
          // address, without reapplying its already-consumed linker addend.
          if (Type == (ELFT::Is64Bits ? R_MIPS_64 : R_MIPS_32)) {
            constexpr unsigned Width = sizeof(typename ELFT::Addr);
            if (const uint8_t *Bytes = Img.readVA(Entry.r_offset, Width)) {
              typename ELFT::Addr Target;
              std::memcpy(&Target, Bytes, Width);
              LinkedPointers.push_back({Entry.r_offset, Target});
            }
          }
          continue;
        }
        if (Type == R_MIPS_NONE)
          continue;
        const auto Index = Entry.getSymbol(ELF.isMips64EL());
        if (!Index || Index >= Symbols.size())
          return llvm::createStringError(
              llvm::inconvertibleErrorCode(),
              "elf: invalid MIPS runtime relocation symbol");
        const auto &Symbol = Symbols[Index];
        const bool Import = Symbol.st_shndx == SHN_UNDEF;
        if ((Type == R_MIPS_JUMP_SLOT || Type == R_MIPS_GLOB_DAT) && Import &&
            (Symbol.getType() == STT_FUNC || Symbol.getType() == STT_NOTYPE)) {
          if constexpr (requires { Entry.r_addend; })
            if (Entry.r_addend != 0)
              return llvm::createStringError(
                  llvm::inconvertibleErrorCode(),
                  "elf: unsupported MIPS import addend");
          continue;
        }
        if (Type == R_MIPS_COPY && !Import && Symbol.getType() == STT_OBJECT &&
            (Symbol.st_size == 1 || Symbol.st_size == 2 ||
             Symbol.st_size == 4 || Symbol.st_size == 8) &&
            Symbol.st_value == Entry.r_offset &&
            Img.readVA(Entry.r_offset, Symbol.st_size)) {
          const auto Strings = getLinkedStringTable<ELFT>(
              ELF, Sections, Sections[Section.sh_link]);
          auto Name = Symbol.getName(Strings);
          if (!Name)
            return Name.takeError();
          if (!Name->empty())
            continue;
        }
        return llvm::createStringError(
            llvm::inconvertibleErrorCode(),
            "elf: unsupported MIPS runtime relocation %u", Type);
      }
      return llvm::Error::success();
    };
    if (Section.sh_type == SHT_REL) {
      if (auto Error = Check(ELF.rels(Section)))
        return Error;
    } else if (auto Error = Check(ELF.relas(Section)))
      return Error;
  }
  recordAbsolutePointerRelocations(Img, LinkedPointers);
  auto Dynamic = readELFProgramDynamicTable(Img);
  if (!Dynamic)
    return Dynamic.takeError();
  std::map<int64_t, uint64_t> Tags;
  for (const auto &Entry : *Dynamic)
    Tags[Entry.Tag] = Entry.Value;
  const auto Fail = [](const char *Reason) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "elf: MIPS GOT %s", Reason);
  };
  const auto GOT = Tags.find(DT_PLTGOT);
  if (GOT == Tags.end())
    return llvm::Error::success();
  const auto Local = Tags.find(DT_MIPS_LOCAL_GOTNO);
  const auto First = Tags.find(DT_MIPS_GOTSYM);
  const auto Total = Tags.find(DT_MIPS_SYMTABNO);
  if (Local == Tags.end() || First == Tags.end() || Total == Tags.end() ||
      Local->second < 2 || First->second > Total->second)
    return Fail("has incomplete or inconsistent dynamic tags");
  constexpr uint64_t Width = sizeof(typename ELFT::Addr);
  const uint64_t Globals = Total->second - First->second;
  if (Local->second > InvalidVA / Width ||
      Globals > InvalidVA / Width - Local->second)
    return Fail("extent overflows the address space");
  // A linked GOT can end in the zero-filled tail of PT_LOAD. Those bytes
  // are defined by ELF just like file bytes (the dynamic linker fills its
  // reserved resolver cells at runtime).
  const uint64_t Size = (Local->second + Globals) * Width;
  const auto *Owner = Img.getSegmentFor(GOT->second);
  const uint8_t *Bytes = Img.readVA(GOT->second, Size);
  if (!Owner || !rangeInBounds(GOT->second - Owner->VA, Size, Owner->Size) ||
      !Bytes)
    return Fail("is not mapped");
  Img.recordRuntimeCallablePointerSlot(
      GOT->second, RuntimeCallablePointerSlotKind::ELFLazyResolver);
  if (const auto PLT = Tags.find(DT_MIPS_PLTGOT); PLT != Tags.end())
    Img.recordRuntimeCallablePointerSlot(
        PLT->second, RuntimeCallablePointerSlotKind::ELFLazyResolver);

  // The first two cells are the runtime resolver and module tag. Local cells
  // after them are link-time addresses rebased by the object's load bias,
  // which is zero in NeverD's linked-image model.
  std::vector<AbsolutePointerRelocation> Pointers;
  for (uint64_t I = 2; I < Local->second; ++I) {
    typename ELFT::Addr Value;
    std::memcpy(&Value, Bytes + I * Width, Width);
    Pointers.push_back({GOT->second + I * Width, Value});
  }
  recordAbsolutePointerRelocations(Img, Pointers);
  if (!Globals)
    return llvm::Error::success();
  const typename ELFT::Shdr *Symbols = nullptr;
  for (const auto &Section : Sections)
    if (Section.sh_type == SHT_DYNSYM)
      Symbols = &Section;
  if (!Symbols)
    return Fail("has no dynamic symbol table");
  const auto Strings = getLinkedStringTable<ELFT>(ELF, Sections, *Symbols);
  auto Entries = ELF.symbols(Symbols);
  if (!Entries)
    return Entries.takeError();
  if (Entries->size() != Total->second || Strings.empty())
    return Fail("symbol extent or string table is inconsistent");
  for (uint64_t I = First->second; I < Total->second; ++I) {
    const auto &Symbol = (*Entries)[I];
    const va_t Slot = GOT->second + (Local->second + I - First->second) * Width;
    if (Symbol.st_shndx != SHN_UNDEF) {
      typename ELFT::Addr Value;
      std::memcpy(&Value, Bytes + (Local->second + I - First->second) * Width,
                  Width);
      // st_other owns a defined function's encoding; st_value may be even
      // while its GOT cell contains the serialized callable tag. Data symbols
      // retain their literal address, even when they overlap executable bytes.
      uint64_t Expected = Symbol.st_value;
      if (Symbol.getType() == STT_FUNC &&
          Img.Mode == InstructionMode::MixedMIPS) {
        const va_t Address = normalizeCodeAddress(Expected, Img.Arch, Img.Mode);
        const auto Mode = Img.instructionModeAt(Address);
        if (!Mode)
          return Fail("defined function has no instruction mode");
        Expected = serializeCodePointer(Address, Img.Arch, *Mode);
      }
      if (Symbol.getType() == STT_TLS || Symbol.getType() == STT_GNU_IFUNC ||
          uint64_t(Value) != Expected)
        return Fail("requires unsupported runtime symbol resolution");
      recordAbsolutePointerRelocation(Img, Slot, Value);
      continue;
    }
    if (Symbol.getType() != STT_FUNC && Symbol.getType() != STT_NOTYPE)
      return Fail("requires unsupported imported data or TLS binding");
    auto Name = Symbol.getName(Strings);
    if (!Name)
      return Name.takeError();
    if (Name->empty())
      continue;
    if (!Img.recordImportStorageSlot(Slot, *Name, 0,
                                     ImportStorageEvidence::LoaderBind))
      return Fail("has a conflicting import cell");
    Import Imported;
    Imported.Name = Name->str();
    Imported.Module = kExternModule.str();
    Imported.IATAddr = Slot;
    Img.Imports.push_back(std::move(Imported));
  }
  return llvm::Error::success();
}

#define INSTANTIATE(ELFT)                                                      \
  template llvm::Error collectMIPSDynamicGOT<llvm::object::ELFT>(              \
      const llvm::object::ELFFile<llvm::object::ELFT> &,                       \
      llvm::ArrayRef<llvm::object::ELFT::Shdr>, BinaryImage &);
INSTANTIATE(ELF32LE)
INSTANTIATE(ELF32BE)
INSTANTIATE(ELF64LE)
INSTANTIATE(ELF64BE)
#undef INSTANTIATE

} // namespace neverd::elf_loader::detail
