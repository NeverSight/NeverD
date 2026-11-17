//===- CodeViewFunctionParameters.h - Checked argument lists ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One CodeView argument-list policy for native/legacy PDB and local types.

#ifndef NEVERD_TYPES_CODEVIEWFUNCTIONPARAMETERS_H
#define NEVERD_TYPES_CODEVIEWFUNCTIONPARAMETERS_H

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <optional>

namespace neverd::types {

struct CodeViewFunctionParameters {
  size_t FixedCount;
  bool Variadic;
};

/// The declared count includes the trailing no-type ellipsis marker. The
/// caller has validated the list's storage; no other no-type entry is valid.
inline std::optional<CodeViewFunctionParameters>
codeViewFunctionParameters(size_t DeclaredCount, size_t ListCount,
                           llvm::function_ref<bool(size_t)> IsNoType) {
  if (DeclaredCount != ListCount)
    return std::nullopt;
  for (size_t I = 0; I < ListCount; ++I)
    if (IsNoType(I))
      return I + 1 == ListCount
                 ? std::optional<CodeViewFunctionParameters>{{I, true}}
                 : std::nullopt;
  return CodeViewFunctionParameters{ListCount, false};
}

} // namespace neverd::types

#endif // NEVERD_TYPES_CODEVIEWFUNCTIONPARAMETERS_H
