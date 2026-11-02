//===- COFFRegistrationSEHException.h - SEH runtime objects -------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONSEHEXCEPTION_H
#define NEVERD_COFFREGISTRATIONSEHEXCEPTION_H

#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/RegistrationFrameRuntime.h"

namespace neverd::coff_registration {

llvm::Expected<std::set<const llvm::LoadInst *>>
bindSEHNullReads(const MedFunc &Source,
                 llvm::ArrayRef<const llvm::Function *> Functions);

llvm::Expected<RegistrationSEHExceptionContract> bindSEHExceptionReads(
    const MedFunc &Source, const RegistrationFrame &Frame,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks,
    const std::map<const llvm::Function *, const llvm::StoreInst *> &Bridges,
    llvm::ArrayRef<const llvm::Function *> Functions);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHEXCEPTION_H
