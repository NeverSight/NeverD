//===- AArch64AuthenticatedCall.h - Bound call authentication --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Prove which control-flow authentications belong to one indirect call.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_AARCH64_AARCH64AUTHENTICATEDCALL_H
#define NEVERD_IR_MED_AARCH64_AARCH64AUTHENTICATEDCALL_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/SmallVector.h"

namespace neverd {

struct AArch64AuthenticatedCall {
  const MedOp *Authentication = nullptr;
  const MedOp *Call = nullptr;
};

/// The authenticated temporary must have exactly one use, as the call target,
/// with no intervening effect or mutation of its original signed operands.
/// Other authentications remain explicit. This never mutates machine IR.
llvm::SmallVector<AArch64AuthenticatedCall, 4>
findAArch64AuthenticatedCalls(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_AARCH64_AARCH64AUTHENTICATEDCALL_H
