//===- X86LiftBitCount.h - Scalar bit-count semantics --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared legacy, REX2 and APX bit-count admission and undefined-flag audit.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LIFTBITCOUNT_H
#define NEVERD_LIB_LIFT_X86_X86LIFTBITCOUNT_H

#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::bitcount {

bool isInstruction(unsigned Id);
bool form(const cs_insn *Insn, Arch Target);
bool matches(const cs_insn *Insn, llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects);
bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn);

} // namespace neverd::bitcount

#endif // NEVERD_LIB_LIFT_X86_X86LIFTBITCOUNT_H
