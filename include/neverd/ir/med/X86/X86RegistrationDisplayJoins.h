//===- X86RegistrationDisplayJoins.h - Frame joins of the view --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The stack and frame pointers of an x86 registration frame in the decompile
/// view, at the joins of its normal paths and its handlers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONDISPLAYJOINS_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONDISPLAYJOINS_H

namespace neverd {

struct MedFunc;

/// Each PHI of the stack or frame pointer that every way in gives one distance
/// from the entry stack pointer becomes that distance.  A handler joins the
/// normal path at each test of a long function, and the chain of joins would
/// outrun any bounded proof of where the frame is.
void settleRegistrationDisplayJoins(MedFunc &Func);

} // namespace neverd

#endif // NEVERD_IR_MED_X86_X86REGISTRATIONDISPLAYJOINS_H
