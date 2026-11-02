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

/// Infer the record pointer types of source parameters through full-width
/// copies and proven private-frame stores/loads; no other value originates a
/// type. Every definition and use of a local must agree.
/// ParameterTypes, when supplied, must match the function's recovered parameter
/// positions; the caller owns the binding to source declarations. Reused
/// storage, unknown writes, overlap and incomplete control flow retain no
/// memory fact. Budget exhaustion returns no facts. Neither the function nor
/// its types are mutated.
HighPointerTypeFacts
inferHighPointerTypes(const HighFunc &Function, Arch Architecture,
                      llvm::ArrayRef<TypeRef> ParameterTypes = {});
} // namespace neverd

#endif // NEVERD_IR_HIGH_HIGHPOINTERTYPEINFERENCE_H
