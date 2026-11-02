//===- X86BoundedLeafState.h - SGX/SMX LowIR state construction -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The decoder emits pending state; CFG reuses the same builder after proof.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIFT_X86_X86BOUNDEDLEAFSTATE_H
#define NEVERD_LIFT_X86_X86BOUNDEDLEAFSTATE_H

#include "neverd/ir/intrinsics/X86/X86BoundedLeaf.h"
#include "neverd/lift/LiftCommon.h"

namespace neverd {
void liftBoundedLeafState(LiftStateBase &S, Arch A, Intrinsic Id,
                          std::optional<uint64_t> Ax = std::nullopt);
} // namespace neverd

#endif // NEVERD_LIFT_X86_X86BOUNDEDLEAFSTATE_H
