//===- LibCFloatingArguments.h - Header FP calls -----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Fixed C header and local declarations supported by the scalar FP adapter.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCFLOATINGARGUMENTS_H
#define NEVERD_LIBC_LIBCFLOATINGARGUMENTS_H

#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/ir/FloatingCallArguments.h"

#include <optional>
#include <vector>

namespace neverd::libc {

inline std::optional<FloatingCallArguments>
floatingCallArguments(const FunctionSym &Function, Arch A, BinaryFormat F,
                      const DebugContext *NativeDebug = nullptr) {
  // A native declaration must come from the exact image's validated type
  // reader. Names alone, ambiguous entries and missing return contracts do
  // not establish a complete call ABI.
  const bool NativeDeclaration =
      NativeDebug && NativeDebug->hasAuthenticatedFunctionSignatures() &&
      NativeDebug->resolveAuthenticatedReturnValueState(Function.Addr).Kind !=
          AuthenticatedReturnKind::Unknown;
  if ((!Function.PrototypeFromHeader && !Function.PrototypeFromCLibrary &&
       Function.PrototypeLibrary != UserPrototypeLibrary &&
       !NativeDeclaration) ||
      !Function.PlatformConvention || Function.IsVariadic ||
      !Function.ParamEntryLocations.empty() ||
      Function.CallConv == DebugCallConv::Fastcall ||
      Function.CallConv == DebugCallConv::Thiscall)
    return std::nullopt;
  std::vector<TypeRef> Parameters;
  for (const auto &[Name, Type] : Function.Params)
    Parameters.push_back(Type);
  return neverd::floatingCallArguments(A, F, Function.ReturnType, Parameters);
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFLOATINGARGUMENTS_H
