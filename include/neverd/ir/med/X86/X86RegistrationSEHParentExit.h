//===- X86RegistrationSEHParentExit.h - Finally parent exits -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Binds finally branches and SavedESP restores to their source parent.

#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONSEHPARENTEXIT_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONSEHPARENTEXIT_H

#include "neverd/ir/RegistrationState.h"

#include <map>
#include <optional>
#include <vector>

namespace neverd {
struct MedFunc;
struct MedOp;

/// One physical finally branch can be reached by several ordinary calls and
/// by runtime dispatch. All invocations must agree on its restored parent.
struct RegistrationSEHParentExit {
  va_t EntryVA = InvalidVA;
  int TargetBlock = -1;
  int RestoreBlock = -1;
  int32_t StackOffset = 0;
  const MedOp *Operation = nullptr;
  const MedOp *Restore = nullptr;
  std::vector<const RegistrationSEHInvocationExit *> Invocations;
};

/// Bind parent-exit receipts to exact current operations and CFG edges. This
/// authenticates source flow; native lowering still needs a physical parent
/// continuation, registration-chain recovery and an independent install proof.
std::optional<std::map<int, RegistrationSEHParentExit>>
getRegistrationSEHParentExits(const MedFunc &Function);

} // namespace neverd

#endif // NEVERD_IR_MED_X86_X86REGISTRATIONSEHPARENTEXIT_H
