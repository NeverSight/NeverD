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

#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/libc/LibCCallDeclaration.h"

#include <optional>
#include <vector>

namespace neverd::libc {

inline std::optional<FloatingCallArguments>
floatingCallArguments(const FunctionSym &Function, Arch A, BinaryFormat F,
                      const DebugContext *NativeDebug = nullptr,
                      InstructionMode EntryMode = InstructionMode::Default) {
  if (!hasFixedPlatformCallDeclaration(Function, NativeDebug))
    return std::nullopt;
  std::vector<TypeRef> Parameters;
  for (const auto &[Name, Type] : Function.Params)
    Parameters.push_back(Type);
  return neverd::floatingCallArguments(A, F, Function.ReturnType, Parameters,
                                       std::nullopt, EntryMode);
}

} // namespace neverd::libc

#endif // NEVERD_LIBC_LIBCFLOATINGARGUMENTS_H
