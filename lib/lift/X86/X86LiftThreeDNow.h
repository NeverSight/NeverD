//===- X86LiftThreeDNow.h - Owned packed MMX lifting ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate a complete 3DNow instruction before publishing any effect.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LIB_LIFT_X86_X86LIFTTHREEDNOW_H
#define NEVERD_LIB_LIFT_X86_X86LIFTTHREEDNOW_H

#include "neverd/lift/X86/X86Lifter.h"

namespace neverd {
bool liftThreeDNow(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn,
                   const cs_x86 &X86);
} // namespace neverd
#endif // NEVERD_LIB_LIFT_X86_X86LIFTTHREEDNOW_H
