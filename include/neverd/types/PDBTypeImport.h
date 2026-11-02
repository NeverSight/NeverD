//===- PDBTypeImport.h - Local types from a PDB's type records --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Every named union, structure and enumeration a PDB's type stream
/// defines, as local types.  Members keep the offsets MSVC chose; the
/// anonymous unions and structures it flattened into their parents are
/// rebuilt around those offsets, and records named only <unnamed-tag> are
/// defined in place where a member uses them.  A forward declaration names
/// the type the stream defines under the same unique name.
///
/// A public PDB, such as Windows' ntkrnlmp.pdb, keeps no typedefs and no
/// function signatures; its function types appear only where a member points
/// to one.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_PDBTYPEIMPORT_H
#define NEVERD_TYPES_PDBTYPEIMPORT_H

#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <string>
#include <vector>

namespace llvm::pdb {
class PDBFile;
} // namespace llvm::pdb

namespace neverd::types {

/// What an import read.
struct PDBTypeImportStats {
  unsigned Structs = 0;
  unsigned Unions = 0;
  unsigned Enums = 0;
  /// Records declared but never defined in the stream.
  unsigned DeclaredOnly = 0;
  /// Function type occurrences with a convention local types cannot represent.
  /// Pointers to these types stay opaque instead of inventing a C signature.
  unsigned UnsupportedFunctionTypes = 0;
  /// Named records whose rebuilt members do not flatten back to the list
  /// MSVC wrote; empty when every nesting holds.
  std::vector<std::string> NestingMismatches;
};

/// Adds the types \p File defines to \p Into.
llvm::Expected<PDBTypeImportStats> importPDBTypes(llvm::pdb::PDBFile &File,
                                                  TypeLibrary &Into);

/// The types \p File defines, in a library of the data model its machine
/// uses.
llvm::Expected<std::unique_ptr<TypeLibrary>>
importPDBTypes(llvm::pdb::PDBFile &File, PDBTypeImportStats *Stats);

/// The types of the PDB at \p Path, in a library of the data model its
/// machine uses.
llvm::Expected<std::unique_ptr<TypeLibrary>>
importPDBTypes(llvm::StringRef Path, PDBTypeImportStats *Stats = nullptr);

} // namespace neverd::types

#endif // NEVERD_TYPES_PDBTYPEIMPORT_H
