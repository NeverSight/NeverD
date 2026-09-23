//===- MedNoReturn.h - Whole-program no-return propagation ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the MedIR pass that proves internal functions do not return and
/// transfers that fact to their direct call sites.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDNORETURN_H
#define NEVERD_IR_MED_MEDNORETURN_H

#include "neverd/ir/med/MedIR.h"
#include "neverd/ir/low/LowIR.h"

#include <vector>

namespace neverd {

/// Architectural terminating operations recognized by the shared no-return
/// proof. This does not grant an intrinsic a source ABI or memory semantics.
bool isArchitecturalNoReturn(const MedOp &Op, Arch TheArch);

/// LowIR form of the same architectural termination fact. Consumers must
/// still prove that the operation ends its block before cutting a path.
bool isArchitecturalNoReturn(const LowOp &Op, Arch TheArch);

/// Recheck the current graph using explicit terminators and already-proven
/// call effects, without accepting the function's DoesNotReturn flag as proof.
bool hasProvenNoReturnExit(const MedFunc &Func, Arch TheArch);

/// Prove internal no-return functions from explicit architectural terminators
/// and already-known no-return calls, then mark every direct call to them.
void propagateInternalNoReturn(std::vector<MedFunc> &Funcs, Arch TheArch);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDNORETURN_H
