//===- RegistrationDisplayFrame.h - x86 SEH in the decompile view -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_IR_LOW_X86_REGISTRATIONDISPLAYFRAME_H
#define NEVERD_LIB_IR_LOW_X86_REGISTRATIONDISPLAYFRAME_H

#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

namespace neverd::x86_registration {

/// Read what the frame of \p Func, an x86 `_except_handler3` or
/// `_except_handler4` function in the decompile view, does for its runtime, and
/// record it in Func.RegistrationStates->Display. The instructions that do only
/// that go: C's `__try` says the same.
void prepareRegistrationDisplayFrame(LowFunc &Func, const BinaryImage *Image);

} // namespace neverd::x86_registration

#endif // NEVERD_LIB_IR_LOW_X86_REGISTRATIONDISPLAYFRAME_H
