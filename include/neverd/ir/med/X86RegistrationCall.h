//===- X86RegistrationCall.h - Checked source call ABI ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86REGISTRATIONCALL_H
#define NEVERD_IR_MED_X86REGISTRATIONCALL_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// The argument registers of a checked original PE32 callee. These callees
/// read no incoming stack arguments; a leaf may borrow an object through ECX.
/// This describes the callee, not the current values of the caller's registers.
struct RegistrationCallABI {
  bool BorrowsECX = false;
};

/// Bind the retained LowIR callee contract to this exact surviving CALL.
/// An incomplete, moved, duplicated or retargeted occurrence has no binding.
std::optional<RegistrationCallABI> registrationCallABI(const MedFunc &Func,
                                                       const MedBlock &Block,
                                                       const MedOp &Call);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONCALL_H
