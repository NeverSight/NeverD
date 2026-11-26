//===- MachOObjectExterns.h - Addresses of a Mach-O object's externs -*- C++ -//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What a Mach-O object references without defining, placed by the shared
/// object-extern layer (neverd/loader/ObjectExterns.h).  A GOT reference
/// reaches its symbol through an entry the linker would create; the loader
/// makes those entries as the layer's cells, a read-only `__got`, each holding
/// its symbol's address.  Nothing outside this directory may include this
/// header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_MACHO_MACHOOBJECTEXTERNS_H
#define NEVERD_LIB_LOADER_MACHO_MACHOOBJECTEXTERNS_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ObjectExterns.h"

#include "llvm/Object/MachO.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace neverd::macho_loader {

/// A Mach-O object's externs, and the GOT cell of each symbol a GOT
/// reference names, by symbol table index.
struct ObjectExterns : object_externs::ExternLayout {
  std::map<uint32_t, va_t> GOTCells;
};

/// Whether \p Type relocates a call or branch of \p A, so the symbol it names
/// is a function (MachOObjectRelocations.def).
bool isBranchReference(Arch A, uint32_t Type);

/// The relocation a GOT reference \p Type of \p A applies against the GOT
/// entry of its symbol, or nullopt (MachOObjectRelocations.def).
std::optional<uint32_t> directTypeOfGOTReference(Arch A, uint32_t Type);

/// Whether \p Type of \p A is a data field holding the address, or the
/// distance, of its symbol's GOT entry (MachOObjectRelocations.def).
bool isGOTPointerReference(Arch A, uint32_t Type);

/// Place the undefined and common symbols the relocations of \p Obj name, and
/// the GOT entries its GOT references reach, past every section of \p Img.
llvm::Expected<ObjectExterns>
planObjectExterns(const llvm::object::MachOObjectFile &Obj,
                  const BinaryImage &Img);

/// Add the planned externs and GOT to \p Img: the undefined symbols as
/// symbols and the called ones as imports, the common symbols as data
/// symbols, and each GOT entry holding its symbol's address.
void addObjectExterns(const llvm::object::MachOObjectFile &Obj,
                      const ObjectExterns &Externs, BinaryImage &Img);

/// The address the undefined or common symbol \p Sym resolves to, with the
/// owner its address provenance names: InvalidVA for a call or branch, which
/// reaches an import rather than storage.  Nullopt for any other symbol.
std::optional<std::pair<va_t, va_t>>
resolveObjectExtern(const llvm::object::MachOObjectFile &Obj,
                    const ObjectExterns &Externs,
                    llvm::object::symbol_iterator Sym, bool Branch);

} // namespace neverd::macho_loader

#endif // NEVERD_LIB_LOADER_MACHO_MACHOOBJECTEXTERNS_H
