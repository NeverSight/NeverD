//===- ELFReloc.cpp - ELF relocation handling ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ELF PLT/GOT and relocation resolution implementation.  Parses the
/// .plt and .rela.plt/.rel.plt sections to build a symbol-to-PLT-entry
/// mapping.  Handles both ELF32 and ELF64.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/ELF/ELFReloc.h"

#include "neverd/support/ISAEncoding.h"
#include "neverd/backend/codegen/BinaryUtils.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Error.h"

#include <cstring>

#define DEBUG_TYPE "neverd-elf-reloc"

namespace neverd {

bool ELFRelocResolver::parse(const std::vector<uint8_t> &Binary,
                             Arch /*TargetArch*/) {
  Entries.clear();
  ByName.clear();

  auto Obj = createObjectFromBuffer(Binary);
  if (!Obj) {
    LLVM_DEBUG(llvm::dbgs() << "elf_reloc: failed to parse ELF\n");
    return false;
  }

  uint64_t PLTAddr = 0;
  uint32_t PLTEntrySize = elf::kDefaultPLTEntrySize;

  auto ParsePLT = [&]<typename ELFFileT>(ELFFileT *ELFObj) -> bool {
    auto &ELF = ELFObj->getELFFile();
    auto SectionsOr = ELF.sections();
    if (!SectionsOr) {
      llvm::consumeError(SectionsOr.takeError());
      return false;
    }

    using Shdr = std::remove_cvref_t<decltype((*SectionsOr)[0])>;
    const Shdr *DynSymSH = nullptr;
    const Shdr *RelPltSH = nullptr;
    bool HasAddend = false;

    for (const auto &Sec : *SectionsOr) {
      auto NameOr = ELF.getSectionName(Sec);
      if (!NameOr) {
        llvm::consumeError(NameOr.takeError());
        continue;
      }
      if (*NameOr == section_names::elf::Plt) {
        PLTAddr = Sec.sh_addr;
        if (Sec.sh_entsize > 0)
          PLTEntrySize = Sec.sh_entsize;
      }
      if (Sec.sh_type == llvm::ELF::SHT_DYNSYM)
        DynSymSH = &Sec;
      if ((*NameOr == section_names::elf::RelaPlt &&
           Sec.sh_type == llvm::ELF::SHT_RELA) ||
          (*NameOr == section_names::elf::RelPlt &&
           Sec.sh_type == llvm::ELF::SHT_REL)) {
        RelPltSH = &Sec;
        HasAddend = (Sec.sh_type == llvm::ELF::SHT_RELA);
      }
    }

    if (!DynSymSH || !RelPltSH || PLTAddr == 0)
      return false;

    auto SymTabOr = ELF.symbols(DynSymSH);
    if (!SymTabOr) {
      llvm::consumeError(SymTabOr.takeError());
      return false;
    }
    auto StrTabOr = ELF.getStringTableForSymtab(*DynSymSH);
    if (!StrTabOr) {
      llvm::consumeError(StrTabOr.takeError());
      return false;
    }

    auto AddSlots = [&](auto RelRange) {
      uint32_t Idx = 1;
      for (const auto &R : RelRange) {
        uint32_t SymIdx = R.getSymbol(false);
        if (SymIdx >= SymTabOr->size()) {
          Idx++;
          continue;
        }
        auto NameOr = (*SymTabOr)[SymIdx].getName(*StrTabOr);
        if (!NameOr) {
          llvm::consumeError(NameOr.takeError());
          Idx++;
          continue;
        }
        if (NameOr->empty()) {
          Idx++;
          continue;
        }

        RelocEntry E;
        E.Name = NameOr->str();
        E.Addr = PLTAddr + static_cast<uint64_t>(Idx) * PLTEntrySize;
        E.Size = PLTEntrySize;
        E.IsCode = true;
        ByName[E.Name] = Entries.size();
        Entries.push_back(E);
        LLVM_DEBUG(llvm::dbgs() << "elf_reloc: PLT '" << E.Name << "' at VA=0x"
                                << llvm::utohexstr(E.Addr) << "\n");
        Idx++;
      }
    };

    if (HasAddend) {
      auto RelasOr = ELF.relas(*RelPltSH);
      if (!RelasOr) {
        llvm::consumeError(RelasOr.takeError());
        return false;
      }
      AddSlots(*RelasOr);
    } else {
      auto RelsOr = ELF.rels(*RelPltSH);
      if (!RelsOr) {
        llvm::consumeError(RelsOr.takeError());
        return false;
      }
      AddSlots(*RelsOr);
    }
    return true;
  };

  bool Parsed = false;
  if (auto *E64 = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(Obj.get()))
    Parsed = ParsePLT(E64);
  else if (auto *E32 =
               llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(Obj.get()))
    Parsed = ParsePLT(E32);

  if (!Parsed) {
    LLVM_DEBUG(llvm::dbgs() << "elf_reloc: failed to parse PLT\n");
    return false;
  }

  LLVM_DEBUG(llvm::dbgs() << "elf_reloc: parsed " << Entries.size()
                          << " PLT entries\n");
  return !Entries.empty();
}

bool ELFRelocResolver::populateFromImage(const BinaryImage &Image,
                                         Arch TargetArch) {
  Entries.clear();
  ByName.clear();

  const Section *PltSec = Image.getSectionByName(section_names::elf::Plt);
  if (!PltSec || PltSec->VA == 0 || PltSec->Size == 0)
    return false;

  uint32_t EntrySize = elf::getPLTEntrySize(TargetArch);
  if (PltSec->Alignment > 0 && PltSec->Alignment <= 64)
    EntrySize = std::max(EntrySize, PltSec->Alignment);

  uint32_t Idx = 1;
  for (const auto &Imp : Image.Imports) {
    if (Imp.Name.empty())
      continue;
    uint64_t StubVA = PltSec->VA + static_cast<uint64_t>(Idx) * EntrySize;
    if (StubVA >= PltSec->VA + PltSec->Size)
      break;
    RelocEntry E;
    E.Name = Imp.Name;
    E.Addr = StubVA;
    E.Size = EntrySize;
    E.IsCode = true;
    if (ByName.count(E.Name) == 0) {
      ByName[E.Name] = Entries.size();
      Entries.push_back(E);
    }
    ++Idx;
  }

  if (Entries.empty())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "elf_reloc: populated " << Entries.size()
                          << " PLT entries (entry_size=" << EntrySize
                          << ") from image\n");
  return true;
}

} // namespace neverd
