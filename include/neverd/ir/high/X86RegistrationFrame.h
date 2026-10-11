//===- X86RegistrationFrame.h - Runtime root expressions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_HIGH_X86REGISTRATIONFRAME_H
#define NEVERD_IR_HIGH_X86REGISTRATIONFRAME_H

#include "neverd/ir/high/HighIR.h"

namespace neverd {

struct RegistrationFrameCoordinate;

/// Project a checked source-frame offset without relying on live callback
/// registers. Return null when the signed source displacement cannot fit.
ExprPtr x86RegistrationFrameAddress(const RegistrationFrameCoordinate &Frame,
                                    int32_t Offset = 0);

ExprPtr lowerX86RegistrationRoot(const MedFunc &Func, const MedOp &Op);

} // namespace neverd

#endif // NEVERD_IR_HIGH_X86REGISTRATIONFRAME_H
