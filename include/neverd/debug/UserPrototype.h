//===- UserPrototype.h - Prototypes the user declares -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The prototype the user declares for a routine a program imports, as a
/// local type of the program's own.  It decides the routine's calls over the
/// call model's arity tables (LibCNames.h), as a header library's prototype
/// does not: the user may declare a routine of a C library's name that a
/// library of the program's own exports.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_USERPROTOTYPE_H
#define NEVERD_DEBUG_USERPROTOTYPE_H

#include "neverd/debug/DebugContext.h"

#include "llvm/ADT/StringRef.h"

#include <optional>

namespace neverd {

/// The FunctionSym::PrototypeLibrary of a prototype the user declares: one of
/// the program's local types, not one a header library holds.
inline constexpr llvm::StringLiteral UserPrototypeLibrary = "local-types";

/// A complete source declaration supplied by the analyst for the entry or by
/// its unique local C prototype resolves the logical types in a stripped ELF.
inline bool isUserFunctionDeclaration(const FunctionSym &Function) {
  return Function.ExplicitEntryDeclaration ||
         Function.PrototypeLibrary == UserPrototypeLibrary;
}

/// The prototype the user declares for the routine of C name \p CName, as
/// \p Dbg resolves it (DebugContext::resolvePrototype).  None where \p Dbg
/// resolves none, or one a header library holds.
inline std::optional<FunctionSym> userPrototype(const DebugContext *Dbg,
                                                llvm::StringRef CName) {
  if (!Dbg)
    return std::nullopt;
  std::optional<FunctionSym> Prototype = Dbg->resolvePrototype(CName);
  if (!Prototype || Prototype->PrototypeLibrary != UserPrototypeLibrary)
    return std::nullopt;
  return Prototype;
}

} // namespace neverd

#endif // NEVERD_DEBUG_USERPROTOTYPE_H
