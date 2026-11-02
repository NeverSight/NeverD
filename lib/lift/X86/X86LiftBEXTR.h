//===- X86LiftBEXTR.h - Bit-field extraction semantics --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared XOP, VEX and APX BEXTR admission and undefined-flag contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LIFTBEXTR_H
#define NEVERD_LIB_LIFT_X86_X86LIFTBEXTR_H

#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::bextr {

bool form(const cs_insn *Insn, Arch Target);
bool matches(const cs_insn *Insn, llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects);
bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn);

} // namespace neverd::bextr

#endif // NEVERD_LIB_LIFT_X86_X86LIFTBEXTR_H
