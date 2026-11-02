//===- ARMLiftMem.cpp - ARM32 memory access instruction lifter ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Memory access instruction handlers for ARM32: LDR/STR (including
/// sign-extending and byte/halfword variants), PUSH/POP, LDM/STM,
/// LDRD/STRD, LDREX/STREX (exclusive), LDA/STL (acquire/release),
/// and unprivileged LDRT/STRT.
///
//===----------------------------------------------------------------------===//

#include "ARMLiftDetail.h"

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/ARM/ARMLifter.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool isAliasMnemonic(const cs_insn *Insn, const char *Alias) {
  llvm::StringRef Mnemonic(Insn->mnemonic);
  if (!Mnemonic.consume_front(Alias))
    return false;
  // Every ARM condition is spelled with two letters.
  if (Insn->detail && isPredicated(Insn->detail->arm)) {
    if (Mnemonic.size() < 2 || Mnemonic.front() == '.')
      return false;
    Mnemonic = Mnemonic.drop_front(2);
  }
  return Mnemonic.empty() || Mnemonic == ".w";
}

bool ARMLifter::liftMem(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  return liftMemSingle(*this, S, Insn, ARM) ||
         liftMemMultiple(*this, S, Insn, ARM) ||
         liftMemAtomic(*this, S, Insn, ARM);
}

} // namespace neverd
