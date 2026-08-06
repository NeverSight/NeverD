//===- COFFReloc.cpp - COFF/PE relocation handling ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// COFF/PE import and relocation resolution implementation.  Parses the
/// IAT via llvm::object::COFFObjectFile and resolves import references
/// in recompiled code.  Handles both PE32 and PE32+.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/COFF/COFFReloc.h"

#include "neverd/backend/codegen/BinaryUtils.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

#define DEBUG_TYPE "neverd-coff-reloc"

namespace neverd {

bool COFFRelocResolver::parse(const std::vector<uint8_t> &Binary,
                              Arch /*TargetArch*/) {
  Entries.clear();
  ByName.clear();

  auto Obj = createObjectFromBuffer(Binary);
  if (!Obj) {
    LLVM_DEBUG(llvm::dbgs() << "coff_reloc: failed to parse PE\n");
    return false;
  }

  auto *COFF = llvm::dyn_cast<llvm::object::COFFObjectFile>(Obj.get());
  if (!COFF)
    return false;

  uint64_t ImageBase = COFF->getImageBase();
  bool Is64 = COFF->getPE32PlusHeader() != nullptr;
  uint32_t PtrSize = getPointerSize(Is64);

  for (const auto &Dir : COFF->import_directories()) {
    uint32_t IATRVA = 0;
    if (auto Err = Dir.getImportAddressTableRVA(IATRVA)) {
      llvm::consumeError(std::move(Err));
      continue;
    }

    uint32_t Idx = 0;
    for (const auto &Sym : Dir.imported_symbols()) {
      llvm::StringRef SymName;
      if (auto Err = Sym.getSymbolName(SymName)) {
        llvm::consumeError(std::move(Err));
        ++Idx;
        continue;
      }
      if (!SymName.empty() && ByName.count(SymName.str()) == 0) {
        RelocEntry E;
        E.Name = SymName.str();
        E.Addr = ImageBase + IATRVA + static_cast<uint64_t>(Idx) * PtrSize;
        E.IsCode = false;
        ByName[E.Name] = Entries.size();
        Entries.push_back(E);
        LLVM_DEBUG(llvm::dbgs()
                   << "coff_reloc: import '" << E.Name << "' IAT=0x"
                   << llvm::utohexstr(E.Addr) << "\n");
      }
      ++Idx;
    }
  }

  for (auto I = COFF->delay_import_directory_begin(),
            E = COFF->delay_import_directory_end();
       I != E; ++I) {
    uint32_t Idx = 0;
    for (auto SI = I->imported_symbol_begin(), SE = I->imported_symbol_end();
         SI != SE; ++SI) {
      llvm::StringRef SymName;
      if (auto Err = SI->getSymbolName(SymName)) {
        llvm::consumeError(std::move(Err));
        ++Idx;
        continue;
      }
      if (!SymName.empty() && ByName.count(SymName.str()) == 0) {
        uint64_t Addr = 0;
        if (auto Err = I->getImportAddress(Idx, Addr))
          llvm::consumeError(std::move(Err));
        RelocEntry RE;
        RE.Name = SymName.str();
        RE.Addr = Addr;
        RE.IsCode = false;
        ByName[RE.Name] = Entries.size();
        Entries.push_back(RE);
        LLVM_DEBUG(llvm::dbgs()
                   << "coff_reloc: delay import '" << RE.Name << "' addr=0x"
                   << llvm::utohexstr(RE.Addr) << "\n");
      }
      ++Idx;
    }
  }

  LLVM_DEBUG(llvm::dbgs() << "coff_reloc: parsed " << Entries.size()
                          << " imports (incl. delay)\n");
  return !Entries.empty();
}

bool COFFRelocResolver::populateFromImage(const BinaryImage &Image,
                                          Arch /*TargetArch*/) {
  Entries.clear();
  ByName.clear();

  for (const auto &Imp : Image.Imports) {
    if (Imp.Name.empty() || Imp.IATAddr == 0)
      continue;
    if (ByName.count(Imp.Name))
      continue;
    RelocEntry E;
    E.Name = Imp.Name;
    E.Addr = Imp.IATAddr;
    E.Size = Image.getPointerSize();
    E.IsCode = false;
    ByName[E.Name] = Entries.size();
    Entries.push_back(std::move(E));
  }

  LLVM_DEBUG(llvm::dbgs() << "coff_reloc: populated " << Entries.size()
                          << " IAT entries from image\n");
  return !Entries.empty();
}

} // namespace neverd
