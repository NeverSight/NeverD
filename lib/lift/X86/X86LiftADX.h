//===- X86LiftADX.h - Independent carry chains ----------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own ADCX/ADOX encoding admission and unsigned carry-chain semantics.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LIFT_X86_X86LIFTADX_H
#define NEVERD_LIB_LIFT_X86_X86LIFTADX_H

#include "neverd/lift/X86/X86Lifter.h"

namespace neverd::adx {

bool isInstruction(unsigned Id);
bool form(const cs_insn *Insn, Arch Target);
bool lift(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn);

} // namespace neverd::adx

#endif // NEVERD_LIB_LIFT_X86_X86LIFTADX_H
