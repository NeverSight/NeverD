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

#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/ir/med/MedIR.h"

#include <map>
#include <set>
#include <vector>

namespace neverd {

/// Architectural terminating operations recognized by the shared no-return
/// proof: an intrinsic that never returns (AArch64 BRK and HLT, an x86 INT
/// vector that does not resume).  The intrinsic names its architecture.  This
/// does not grant an intrinsic a source ABI or memory semantics.
bool isArchitecturalNoReturn(const MedOp &Op);

/// The same facts for callers that name the architecture; it adds nothing.
inline bool isArchitecturalNoReturn(const MedOp &Op, Arch) {
  return isArchitecturalNoReturn(Op);
}

/// Recheck the current graph using explicit terminators and already-proven
/// call effects, without accepting the function's DoesNotReturn flag as proof.
bool hasProvenNoReturnExit(const MedFunc &Func, Arch TheArch);

/// Prove internal no-return functions from explicit architectural terminators
/// already-known no-return calls and fully lifted closed cycles, then mark
/// every direct call to them.
/// \p Stated is what the user states of functions by entry
/// (BinaryImage::UserNoReturn): a call to one stated never to return ends
/// its block, and a call to one stated to return does not, whatever its body
/// proves.  A function's own DoesNotReturn stays what its body proves.  A
/// function at one of \p ImportVeneers (an import's PLT stub, which function
/// discovery also lists) is no internal function: a call to it keeps the
/// effect the import's declaration gave it.
void propagateInternalNoReturn(std::vector<MedFunc> &Funcs, Arch TheArch,
                               const std::map<va_t, bool> &Stated = {},
                               const std::set<va_t> &ImportVeneers = {});

} // namespace neverd

#endif // NEVERD_IR_MED_MEDNORETURN_H
