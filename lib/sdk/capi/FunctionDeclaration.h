//===- FunctionDeclaration.h - A function's C declaration -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The declaration a user edits with Y and the parameter locations used by
/// the C API's other requests (NeverDCAPIFunctionType.cpp).
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_FUNCTIONDECLARATION_H
#define NEVERD_SDK_CAPI_FUNCTIONDECLARATION_H

#include "SessionImpl.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/SourceTypeHint.h"
#include "neverd/types/TypeParse.h"

#include <optional>
#include <string>

namespace neverd::sdk {

/// A recorded prototype for the exact function entry, when available.
std::optional<FunctionSym> entryPrototype(Session &S, va_t Entry);

/// The declaration from the user's edit, the printed signature, or the
/// recorded prototype when the body is unavailable.
struct CurrentDeclaration {
  std::string Text;
  types::FunctionDeclaration Read;
  bool Stated = false;
};
/// None with the session's error when no declaration is available.
std::optional<CurrentDeclaration>
currentDeclaration(neverd_session_t Sess, Session &S, va_t Entry,
                   const types::TypeLibrary &Library);

/// Where the image's calling convention passes the parameter \p Index (from
/// 0, as the declaration lists it) of the function at \p Entry; none with
/// \p Error when the declaration does not read, has no such parameter, or
/// NeverD has no rules for the convention.
std::optional<SourceABIValueLocation> parameterLocation(neverd_session_t Sess,
                                                        Session &S, va_t Entry,
                                                        size_t Index,
                                                        std::string &Error);

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_FUNCTIONDECLARATION_H
