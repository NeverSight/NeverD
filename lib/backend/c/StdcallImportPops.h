//===- StdcallImportPops.h - What i386 stdcall imports pop -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_C_STDCALLIMPORTPOPS_H
#define NEVERD_BACKEND_C_STDCALLIMPORTPOPS_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include <map>
#include <optional>
#include <string>

namespace neverd::c_library {

/// Bytes popped, by the name a routine is imported by.
using ImportPops = std::map<std::string, std::optional<unsigned>, std::less<>>;

/// The bytes each i386 stdcall routine \p Img imports pops, by the name it
/// is imported by: its import library's thunk and slot spell them
/// (`_Validate@8`, `__imp__Validate@8`), as the call analysis reads them for
/// an import no declaration types.  None where two of its names disagree.
/// A routine whose calls C compiled from the output makes must be declared
/// stdcall with as many parameters, or the stack would lose its arguments
/// twice.
inline ImportPops stdcallImportPops(const BinaryImage &Img) {
  ImportPops Pops;
  for (const Symbol &Sym : Img.Symbols)
    if (const std::optional<unsigned> Bytes =
            stdcallArgumentBytes(Sym.Name, Img.abiFormat(), Img.Arch)) {
      const auto [Entry, Added] = Pops.emplace(
          importNameOfSymbol(Sym.Name, Img.abiFormat(), Img.Arch).str(), Bytes);
      if (!Added && Entry->second != Bytes)
        Entry->second.reset();
    }
  return Pops;
}

} // namespace neverd::c_library

#endif // NEVERD_BACKEND_C_STDCALLIMPORTPOPS_H
