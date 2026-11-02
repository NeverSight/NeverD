//===- X86LiftBMI2.h - Flag-preserving BMI2 values ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own MULX, PDEP/PEXT, RORX and SARX/SHLX/SHRX encoding and value semantics.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LIFTBMI2_H
#define NEVERD_LIB_LIFT_X86_X86LIFTBMI2_H

#include "neverd/lift/X86/X86Lifter.h"

namespace neverd::bmi2 {

bool isInstruction(unsigned Id);
bool form(const cs_insn *Insn, Arch Target);
bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn);

} // namespace neverd::bmi2

#endif // NEVERD_LIB_LIFT_X86_X86LIFTBMI2_H
