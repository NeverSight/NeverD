//===- COFFRegistrationSEHParentExit.h - Parent transfer proof -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Declares independent proofs for native SEH parent continuations.

#ifndef NEVERD_COFFREGISTRATIONSEHPARENTEXIT_H
#define NEVERD_COFFREGISTRATIONSEHPARENTEXIT_H

#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/RegistrationFrameRuntime.h"

namespace neverd::coff_registration {

/// Authenticate the complete private machine context and every alias. This
/// checks native structure without granting it a source control-flow identity.
llvm::Expected<RegistrationSEHParentExitContract> bindSEHParentExitContext(
    const llvm::Function &Parent, va_t Owner,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::set<const llvm::LoadInst *> &LocalUnwindChainLoads = {});

/// Bind the checked transfers to independently replayed source instructions.
llvm::Error
bindSEHParentExitSources(RegistrationSEHParentExitContract &Contract,
                         const MedFunc &Source);

llvm::Error validateSEHParentExitTargets(
    const RegistrationSEHParentExitContract &Contract,
    const std::map<int, const llvm::BasicBlock *> &Entries);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHPARENTEXIT_H
