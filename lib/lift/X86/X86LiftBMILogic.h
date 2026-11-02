//===- X86LiftBMILogic.h - BMI logical values and flags -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own ANDN, BLSI/BLSMSK/BLSR and BZHI admission and undefined status flags.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LIFTBMILOGIC_H
#define NEVERD_LIB_LIFT_X86_X86LIFTBMILOGIC_H

#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::bmilogic {

bool isInstruction(unsigned Id);
bool form(const cs_insn *Insn, Arch Target);
bool matches(const cs_insn *Insn, llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects);
bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn);

} // namespace neverd::bmilogic

#endif // NEVERD_LIB_LIFT_X86_X86LIFTBMILOGIC_H
