//===- X86RegistrationCallback.h - Checked callback bodies -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86REGISTRATIONCALLBACK_H
#define NEVERD_IR_MED_X86REGISTRATIONCALLBACK_H

#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// Bind a private ESP definition to the current runtime-only callback entry.
/// A source-frame coordinate cannot stand in for this invocation's stack.
bool isRegistrationCallbackStackRoot(const MedFunc &Func, const MedOp &Op);

/// The ordinary CFG of one checked C++ catch. Every block belongs to that
/// invocation; ordinary exits are exact catch returns or nonreturning calls.
/// Ranges retain holes rather than claiming intervening native instructions.
struct RegistrationCallbackRegion {
  std::vector<ExceptionAddressRange> Ranges;
};

std::optional<RegistrationCallbackRegion>
registrationCallbackRegion(const MedFunc &Func, va_t Entry);

} // namespace neverd

#endif // NEVERD_IR_MED_X86REGISTRATIONCALLBACK_H
