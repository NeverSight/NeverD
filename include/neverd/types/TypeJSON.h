//===- TypeJSON.h - Local types as JSON for frontends -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Local types as the JSON the command line, the workbench and scripts read,
/// so that every frontend lays a type out alike.  A record lists its members
/// in order with offsets from the outermost record, anonymous unions and
/// structures nested in place, and the padding the compiler adds as members
/// of their own.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_TYPEJSON_H
#define NEVERD_TYPES_TYPEJSON_H

#include "neverd/types/LocalTypes.h"
#include "neverd/types/TypeFit.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <optional>

namespace neverd::types {

/// The kind, origin and data model as JSON names them.
llvm::StringRef kindName(TypeKind Kind);
llvm::StringRef originName(TypeOrigin Origin);
llvm::StringRef dataModelName(DataModel Model);
/// The data model \p Name names (LLP64, LP64, ILP32), or none.
std::optional<DataModel> dataModelNamed(llvm::StringRef Name);
/// The record ABI \p Name names as recordABIName spells it, or none.
std::optional<RecordABI> recordABINamed(llvm::StringRef Name);

/// Every type in brief, in ordinal order:
/// `{"ordinal", "name", "kind", "size", "members", "origin"}`, where size is
/// absent for a type without one and members counts a record's named
/// members, anonymous ones' included.
llvm::json::Array typeListJSON(const TypeLibrary &Library);

/// \p Type in full.  Every type has `ordinal`, `name`, `kind` and `origin`.
/// A record adds `size`, `alignment`, `pack`, `declaredAlignment`,
/// `defined` and `members`.  Each member has `kind` ("field", "bitfield",
/// "struct" or "union" for a record in place, "padding"), `name` (empty
/// when anonymous), `offset` in bytes and `bitOffset` in bits from the
/// outermost record, `size` in bytes, `type` as C spells it in
/// \p Spelling, `refers` (the library type it names through pointers and
/// arrays, if any), a bitfield's `bitWidth` and `storageOffset`, and a
/// record in place's `members`.  An enumeration adds `size`, `underlying`,
/// `flags` and `values` (`name`, `value` as a signed or unsigned integer);
/// a typedef `target`; a function type `declaration`.
llvm::json::Object
typeJSON(const TypeLibrary &Library, const LocalType &Type,
         BuiltinSpelling Spelling = BuiltinSpelling::Windows);

/// How an access lands as JSON names it: exact, start, inside, padding or
/// outside.
llvm::StringRef accessFitName(AccessFit Fit);

/// Where an access lands (memberAt): `fit`, `path` as C spells it after
/// `->` (empty for the record itself and its own padding), `memberOffset`,
/// and for a member `memberSize`, its `type` as C spells it in \p Spelling
/// and `bitfield` when it is the storage of bitfields.
llvm::json::Object memberAtJSON(const TypeLibrary &Library, const MemberAt &At,
                                BuiltinSpelling Spelling);

/// A type a search found (fitTypes): `name`, `kind`, `origin`, `size`, the
/// `record` the accesses were judged against when it is another type, the
/// record's `aliases` and `pointerAliases`, `shift` and `shiftPath` where
/// the pointer points into the record, the counts `exact`,
/// `exactElements`, `partial`, `padding` and `outside`, `fits` (each
/// access's landing, memberAtJSON), and where the search text matched:
/// `matchedName` with `matched` runs as [begin, length] in bytes, and
/// `matchedMember` with `memberMatched` runs.
llvm::json::Object typeFitJSON(const TypeLibrary &Library, const TypeFit &Fit,
                               BuiltinSpelling Spelling);

/// typeFitJSON of each type a search found, in order.  The names of the
/// whole library are read once for all of them, not once a type.
llvm::json::Array typeFitsJSON(const TypeLibrary &Library,
                               llvm::ArrayRef<TypeFit> Fits,
                               BuiltinSpelling Spelling);

} // namespace neverd::types

#endif // NEVERD_TYPES_TYPEJSON_H
