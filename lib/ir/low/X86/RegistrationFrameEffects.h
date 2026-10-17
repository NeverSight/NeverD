//===- RegistrationFrameEffects.h - PE32 frame effects ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Uses the common x86 intrinsic policy for PE32 integer-frame analysis.

#ifndef NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H
#define NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H

#include "FrameEffects.h"

namespace neverd::registration_state {

/// WAIT, FXAM and checked FP arithmetic write no integer registers or frame
/// memory. Their FP state effects and traps remain in LowIR. A memory
/// arithmetic form reads its operand; it cannot grant memory privacy or
/// arbitrary intrinsics.
inline bool isFramePreservingIntrinsic(const LowOp &Op) {
  return isX86FramePreservingIntrinsic(Op, Arch::X86);
}

} // namespace neverd::registration_state

#endif // NEVERD_IR_LOW_X86_REGISTRATIONFRAMEEFFECTS_H
