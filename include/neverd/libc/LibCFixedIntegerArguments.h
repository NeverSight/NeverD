//===- LibCFixedIntegerArguments.h - Integer calls ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Authenticate fixed scalar declarations before binding their machine
/// operands. SourceParameterPlacement remains the physical location owner.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_LIBCFIXEDINTEGERARGUMENTS_H
#define NEVERD_LIBC_LIBCFIXEDINTEGERARGUMENTS_H

#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/libc/LibCCallDeclaration.h"

#include <optional>
#include <vector>

namespace neverd::libc {

inline std::optional<FixedIntegerCallArguments>
fixedIntegerCallArguments(const FunctionSym &Function, Arch A, BinaryFormat F,
                          const DebugContext *NativeDebug = nullptr) {
  if (!hasFixedPlatformCallDeclaration(Function, NativeDebug))
    return std::nullopt;
  std::vector<TypeRef> Parameters;
  for (const auto &[Name, Type] : Function.Params)
    Parameters.push_back(Type);
  return neverd::fixedIntegerCallArguments(A, F, Function.ReturnType,
                                           Parameters);
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFIXEDINTEGERARGUMENTS_H
