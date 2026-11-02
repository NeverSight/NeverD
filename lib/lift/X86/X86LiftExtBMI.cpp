//===- X86LiftExtBMI.cpp - x86/x64 BMI/BMI2/ADX lifter --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Bit-manipulation extensions: TZCNT/LZCNT/POPCNT, the BLS*
/// lowest-set-bit family, ANDN, BEXTR, BZHI, MULX, PDEP/PEXT,
/// RORX, the flag-preserving shifts SARX/SHLX/SHRX, and the
/// ADX carry chains ADCX/ADOX.
///
//===----------------------------------------------------------------------===//

#include "X86LiftADX.h"
#include "X86LiftBEXTR.h"
#include "X86LiftBMI2.h"
#include "X86LiftBMILogic.h"
#include "X86LiftBitCount.h"
#include "X86LiftDetail.h"

#include "neverd/lift/X86/X86Lifter.h"

#define DEBUG_TYPE "neverd-lift-x86"

namespace neverd {

bool liftExtBMI(X86Lifter &L, X86Lifter::LiftState &S, const cs_insn *Insn) {
  if (bmi2::isInstruction(Insn->id))
    return bmi2::lift(L, S, Insn);
  if (bmilogic::isInstruction(Insn->id))
    return bmilogic::lift(L, S, Insn);
  if (bitcount::isInstruction(Insn->id))
    return bitcount::lift(L, S, Insn);
  if (Insn->id == X86_INS_BEXTR)
    return bextr::lift(L, S, Insn);
  if (adx::isInstruction(Insn->id))
    return adx::lift(L, S, Insn);
  return false;
}

} // namespace neverd
