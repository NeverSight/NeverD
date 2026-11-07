//===- MedTypePass.h - Type inference pass for MedIR --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the type inference pass that annotates MedFunc with inferred
/// return type, parameter types, and local variable types.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_MEDTYPEPASS_H
#define NEVERD_IR_MED_MEDTYPEPASS_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <map>

namespace neverd {

/// Visit pointer-capable inputs of validated original memory instructions.
/// This is a type heuristic, not permission to replace address semantics.
/// Returns true for a recognized intrinsic, including a malformed one whose
/// operands must not contribute pointer evidence through another fallback.
bool visitOriginalMemoryPointerInputs(
    const MedOp &Op, llvm::function_ref<void(const MedVar &)> Record);

/// \p CalleeFloatReturns maps a call target to the bytes of the scalar float
/// it returns through the FP return register, for a call whose output that
/// register is (promoteFloatCallResult).
void inferMedTypes(
    MedFunc &Func, Arch TheArch,
    const std::map<va_t, uint16_t> *CalleeFloatReturns = nullptr);

/// Refresh inferred parameter types after call recovery changes the carrier
/// list. Preserve explicit source signatures and the return/local types.
void refreshInferredMedParameters(MedFunc &Func, Arch TheArch);

/// Refresh the parameters call-ABI recovery added to \p Funcs, code of
/// \p TheArch, and propagate a callee's proven pointer role through exact
/// entry-register forwarding calls to a fixed point.
void propagateForwardedPointerParams(std::vector<MedFunc> &Funcs, Arch TheArch);

} // namespace neverd

#endif // NEVERD_IR_MED_MEDTYPEPASS_H
