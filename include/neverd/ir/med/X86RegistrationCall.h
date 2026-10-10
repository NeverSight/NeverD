//===- X86RegistrationCall.h - Checked source call ABI ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86REGISTRATIONCALL_H
#define NEVERD_IR_MED_X86REGISTRATIONCALL_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// The physical arguments of a checked original PE32 callee. A leaf may
/// borrow ECX; a direct rethrow takes two null stack arguments. This describes
/// the callee, not the current values supplied by the caller.
struct RegistrationCallABI {
  bool BorrowsECX = false;
  bool RuntimeRethrow = false;
};

/// Bind the retained LowIR callee contract to this exact surviving CALL.
/// An incomplete, moved, duplicated or retargeted occurrence has no binding.
std::optional<RegistrationCallABI> registrationCallABI(const MedFunc &Func,
                                                       const MedBlock &Block,
                                                       const MedOp &Call);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONCALL_H
