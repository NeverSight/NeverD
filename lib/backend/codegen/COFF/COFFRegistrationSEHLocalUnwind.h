//===- COFFRegistrationSEHLocalUnwind.h - CRT unwind proof ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares independent validation of native local-unwind bridge bodies.

#ifndef NEVERD_COFFREGISTRATIONSEHLOCALUNWIND_H
#define NEVERD_COFFREGISTRATIONSEHLOCALUNWIND_H

#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/RegistrationFrameRuntime.h"

#include "llvm/Support/Error.h"

namespace llvm {
class Function;
}

namespace neverd::coff_registration {

/// Authenticate the physical protocol without claiming an immutable source
/// target. Callback/frame validation can run before source replay is bound.
llvm::Expected<const llvm::Function *>
inspectSEHLocalUnwindBridge(const llvm::Function &Bridge,
                            RegistrationLocalUnwindContract::Kind Protocol);

llvm::Expected<std::set<const llvm::LoadInst *>>
collectSEHLocalUnwindChainLoads(
    RegistrationLocalUnwindContract::Kind Protocol,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks);

/// Bind every bridge invocation and logical state restore to replayed source.
/// ChainLoads additionally require bindSEHParentExitContext to authenticate
/// their exact private storage and capture before they may be used.
llvm::Expected<RegistrationSEHLocalUnwindContract> bindSEHLocalUnwinds(
    const MedFunc &Source, const llvm::Function &Parent,
    const RegistrationFrame &Frame,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks);

/// Check the bridge body independently of its creation. This grants no frame
/// capture or call-site authority; those must be bound to source occurrences.
llvm::Error
validateSEHLocalUnwindBridge(const llvm::Function &Bridge,
                             const RegistrationLocalUnwindContract &Source);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHLOCALUNWIND_H
