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
/// unions and structures in place and bit-fields at their bits. Overlapping
/// padding of C++ subobjects becomes an anonymous overlay; their value bits
/// must remain disjoint. A record laid out tighter than its members takes the
/// packing that lays it out so.  A type C names only through a typedef,
/// `typedef struct { ... } X`, takes the typedef's name, as C declarations
/// read (TypeParse.h).  C++ and Rust types are named within their
/// namespaces and records, `ns::Outer::Inner`; Go's names hold their
/// package, `main.Point`.
///
/// The same name in many units, as a header's types are, is one type: the
/// first definition, which a declaration elsewhere names.  Another
/// record definition of another size is left out and reported. When a
/// bit-field or overlapping layout needs a forward declaration's definition,
/// publishing it wakes only its blocked dependents. Incomplete aliases remain
/// named references. Missing required definitions and by-value cycles retain
/// explicit diagnostics.
/// Unsupported layouts, such as dynamic virtual-base offsets, retain their
/// reasons rather than guessed layouts. Member pointers keep their source
/// identity alongside explicitly labelled ABI storage, not ordinary pointers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_DWARFTYPEIMPORT_H
#define NEVERD_TYPES_DWARFTYPEIMPORT_H

#include "neverd/types/LocalTypes.h"

#include "llvm/Support/Error.h"

#include <memory>
#include <optional>
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

/// The size of the long double \p Context's units name, as the compiler
/// that wrote them laid it out; none where no unit names one.  Of a Windows
/// program, it tells whose compiler built it: MinGW's keep the x87's long
/// double on x86, Microsoft's and clang-cl's a double.
std::optional<unsigned> dwarfLongDoubleBytes(llvm::DWARFContext &Context);

} // namespace neverd::types

#endif // NEVERD_TYPES_DWARFTYPEIMPORT_H
