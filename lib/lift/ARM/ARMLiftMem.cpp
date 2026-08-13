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
#include "neverd/lift/ARMLifter.h"

#include "llvm/Support/Debug.h"

#include <cstring>

#define DEBUG_TYPE "neverd-lift-arm"

namespace neverd {

bool isAliasMnemonic(const cs_insn *Insn, const char *Alias) {
  size_t AliasLen = std::strlen(Alias);
  return std::strcmp(Insn->mnemonic, Alias) == 0 ||
         (std::strncmp(Insn->mnemonic, Alias, AliasLen) == 0 &&
          std::strcmp(Insn->mnemonic + AliasLen, ".w") == 0);
}

bool ARMLifter::liftMem(LiftState &S, const cs_insn *Insn, const cs_arm &ARM) {
  return liftMemSingle(*this, S, Insn, ARM) ||
         liftMemMultiple(*this, S, Insn, ARM) ||
         liftMemAtomic(*this, S, Insn, ARM);
}

} // namespace neverd
