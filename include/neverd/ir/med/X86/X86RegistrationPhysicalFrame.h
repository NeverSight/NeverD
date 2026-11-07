//===- X86RegistrationPhysicalFrame.h - Shared frame binding ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bind independently replayed shared C++ frame effects to their source calls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_X86REGISTRATIONPHYSICALFRAME_H
#define NEVERD_IR_MED_X86_X86REGISTRATIONPHYSICALFRAME_H

#include "neverd/ir/RegistrationState.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

namespace neverd {
struct BinaryImage;
struct MedFunc;

/// Source calls and the chain operations proved by independently replaying
/// their expanded physical effects. Original source state receipts remain
/// unchanged. Consumers bind every projected occurrence before using it.
struct X86RegistrationPhysicalExit {
  RegistrationChainAccess Source;
  va_t HelperVA = InvalidVA;
};

struct X86RegistrationCxxPhysicalFrame {
  using Exit = X86RegistrationPhysicalExit;
  RegistrationCxxPrologueEffect Entry;
  coff_loader::X86CxxPrologueCall EntryCall;
  std::vector<Exit> Exits;
  std::vector<RegistrationChainAccess> ChainAccesses;
};

/// Exact source or projected chain occurrences, or null for a stale/incomplete
/// shared-frame binding. This is not a current-image authentication query.
const std::vector<RegistrationChainAccess> *
getX86RegistrationCxxChainAccesses(const MedFunc &Function);

/// Recheck the physical frame's source helpers against the current image.
/// Ordinary frames need no shared-helper binding.
bool hasCheckedX86RegistrationCxxPhysicalFrame(const BinaryImage &Image,
                                               const MedFunc &Function);

} // namespace neverd

#endif
