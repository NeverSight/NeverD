//===- MedLLVMRegistrationCxxBlocks.h - PE32 copy block owners ---*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_MEDLLVMREGISTRATIONCXXBLOCKS_H
#define NEVERD_MEDLLVMREGISTRATIONCXXBLOCKS_H

#include "neverd/backend/llvm/X86RegistrationCatch.h"

namespace llvm {
class BasicBlock;
} // namespace llvm

namespace neverd {
/// A PHI-copy block inherits a callback only from the exact ordinary source
/// edge split by this emission. Both endpoints must belong to that invocation.
/// This is an emission plan; edited IR still needs independent installation
/// checks for its actual control flow, operations and private-frame accesses.
std::optional<std::map<llvm::BasicBlock *, X86RegistrationCatchIdentity>>
registrationCxxPHICopyOwners(
    const std::map<int, X86RegistrationCatchIdentity> &Owners,
    const std::map<int, llvm::BasicBlock *> &OriginalBlocks,
    const std::map<llvm::BasicBlock *, std::pair<int, int>> &CopyEdges);
} // namespace neverd

#endif // NEVERD_MEDLLVMREGISTRATIONCXXBLOCKS_H
