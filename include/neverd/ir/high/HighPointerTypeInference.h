//===- HighPointerTypeInference.h - Pointer facts --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Flow-sensitive source types for values transported through private frames.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_HIGHPOINTERTYPEINFERENCE_H
#define NEVERD_IR_HIGH_HIGHPOINTERTYPEINFERENCE_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include "llvm/ADT/ArrayRef.h"

#include <map>

namespace neverd {
using HighPointerTypeFacts = std::map<HighSourceLocalIdentity, TypeRef>;
using HighPointerTypeOrigins = std::map<const HighExpr *, TypeRef>;

/// A value holding a record address plus a byte displacement. A nonzero
/// displacement does not make the value a pointer to that record's beginning.
struct HighPointerLocation {
  TypeRef Type;
  int64_t ByteOffset = 0;
};
using HighPointerLocations =
    std::map<HighSourceLocalIdentity, HighPointerLocation>;
using HighPointerUseLocations = std::map<const HighExpr *, HighPointerLocation>;

/// The same all-definition/all-use proof as inferHighPointerTypes, retaining
/// constant byte arithmetic through copies, joins and private frame cells.
/// Uses optionally receives facts at exact expression occurrences, including
/// pointer-valued record members. A shared expression must agree at every use;
/// these facts alone do not justify changing a local's declaration.
HighPointerLocations
inferHighPointerLocations(const HighFunc &Function, Arch Architecture,
                          llvm::ArrayRef<TypeRef> ParameterTypes = {},
                          const HighPointerTypeOrigins &Origins = {},
                          HighPointerUseLocations *Uses = nullptr);

/// Infer the record pointer types of source parameters through full-width
/// copies and proven private-frame stores/loads. Origins binds authenticated
/// pointer declarations to exact load/call expressions or checked source-local
/// assertions to variable expressions; the caller owns that binding. A stated
/// non-record type suppresses inferred record types. Every definition and use
/// of a local must agree.
/// ParameterTypes, when supplied, must match the function's recovered parameter
/// positions; the caller owns the binding to source declarations. Reused
/// storage, overlapping writes and incomplete control flow retain no memory
/// fact. Calls and unrelated stores preserve private cells only until a frame
/// address escapes; derivation follows copies, arithmetic and every incoming
/// path. Unknown effects revoke that isolation. Budget exhaustion returns no
/// facts. Neither the function nor its types are mutated.
HighPointerTypeFacts
inferHighPointerTypes(const HighFunc &Function, Arch Architecture,
                      llvm::ArrayRef<TypeRef> ParameterTypes = {},
                      const HighPointerTypeOrigins &Origins = {});
} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHPOINTERTYPEINFERENCE_H
