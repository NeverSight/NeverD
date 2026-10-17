//===- DWARFTypeImport.h - Local types from DWARF type entries --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Every named union, structure, enumeration and typedef the DWARF of a
/// program's units defines, as local types.  DWARF keeps a record as its
/// source declares it: members at the offsets the compiler chose, anonymous
/// unions and structures in place, bit-fields at their bits, so nothing is
/// rebuilt; a record laid out tighter than its members align takes the
/// packing that lays it out so.  A type C names only through a typedef,
/// `typedef struct { ... } X`, takes the typedef's name, as C declarations
/// read (TypeParse.h).  C++ and Rust types are named within their
/// namespaces and records, `ns::Outer::Inner`; Go's names hold their
/// package, `main.Point`.
///
/// The same name in many units, as a header's types are, is one type: the
/// first definition, which a declaration elsewhere names.  Another
/// definition of the name that lays the type out otherwise is left out and
/// reported.  So is a record DWARF describes in a way local types do not
/// hold, such as a Rust enumeration's variants or a virtual base, with the
/// reason, rather than a guess at its layout.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_DWARFTYPEIMPORT_H
#define NEVERD_TYPES_DWARFTYPEIMPORT_H

#include "neverd/types/LocalTypes.h"

#include "llvm/Support/Error.h"

#include <memory>
#include <string>
#include <vector>

namespace llvm {
class DWARFContext;
} // namespace llvm

namespace neverd::types {

/// What an import read.
struct DWARFTypeImportStats {
  unsigned Structs = 0;
  unsigned Unions = 0;
  unsigned Enums = 0;
  unsigned Typedefs = 0;
  /// Records declared but never defined in any unit.
  unsigned DeclaredOnly = 0;
  /// The types left out, each as "name: why".
  std::vector<std::string> Skipped;
  /// The names another unit defines otherwise, whose first definition
  /// stays.
  std::vector<std::string> Conflicts;
};

/// Adds the types \p Context's units define to \p Into, whose data model
/// decides how wide long and pointers are where DWARF does not say.
llvm::Expected<DWARFTypeImportStats>
importDWARFTypes(llvm::DWARFContext &Context, TypeLibrary &Into);

/// The types \p Context's units define, in a library of \p Model.
llvm::Expected<std::unique_ptr<TypeLibrary>>
importDWARFTypes(llvm::DWARFContext &Context, DataModel Model,
                 DWARFTypeImportStats *Stats = nullptr);

} // namespace neverd::types

#endif // NEVERD_TYPES_DWARFTYPEIMPORT_H
