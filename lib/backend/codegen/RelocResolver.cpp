//===- RelocResolver.cpp - Common relocation symbol resolution -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements the format-agnostic RelocResolver methods shared by the COFF,
/// ELF, and Mach-O relocation resolvers.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/RelocResolver.h"

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/BinaryUtils.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

#define DEBUG_TYPE "neverd-rewriter"

namespace neverd {

bool RelocResolver::populateFromImage(const BinaryImage &Image, Arch) {
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
    E.IsCode = false;
    ByName[E.Name] = Entries.size();
    Entries.push_back(std::move(E));
  }

  for (const auto &Sym : Image.Symbols) {
    if (!isValidFunctionSymbol(Image, Sym) || Sym.Name.empty())
      continue;
    if (ByName.count(Sym.Name))
      continue;
    RelocEntry E;
    E.Name = Sym.Name;
    E.Addr = Sym.Addr;
    E.IsCode = true;
    ByName[E.Name] = Entries.size();
    Entries.push_back(std::move(E));
  }

  LLVM_DEBUG(llvm::dbgs() << "reloc: populated " << Entries.size()
                          << " entries from image\n");
  return !Entries.empty();
}

va_t RelocResolver::findSymbol(const std::string &Symbol) const {
  const RelocEntry *Entry = findEntry(Symbol);
  return Entry ? Entry->Addr : InvalidVA;
}

const RelocResolver::RelocEntry *
RelocResolver::findEntry(const std::string &Symbol) const {
  std::string Key = resolveSymbolAlias(Symbol, ByName);
  if (!Key.empty())
    return &Entries[ByName.at(Key)];
  return nullptr;
}

} // namespace neverd
