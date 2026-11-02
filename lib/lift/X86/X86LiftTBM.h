//===- X86LiftTBM.h - TBM encoding and undefined flag contract -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared TBM admission, lifting and undefined-flag qualification.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LIFTTBM_H
#define NEVERD_LIB_LIFT_X86_X86LIFTTBM_H

#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/ADT/ArrayRef.h"

namespace neverd::tbm {

bool isInstruction(unsigned Id);
bool form(const cs_insn *Insn, Arch Target);
bool matches(llvm::ArrayRef<LowOp> Ops,
             const LowInstructionUndefinedEffects &Effects);
bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn);

} // namespace neverd::tbm

#endif // NEVERD_LIB_LIFT_X86_X86LIFTTBM_H
