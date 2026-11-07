//===- COFFRegistrationSEHDynamicScope.h - Scope replay --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONSEHDYNAMICSCOPE_H
#define NEVERD_COFFREGISTRATIONSEHDYNAMICSCOPE_H

#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/RegistrationFrameRuntime.h"

namespace neverd::coff_registration {

llvm::Expected<RegistrationSEHScopeState>
bindDynamicSEHState(const MedFunc &Source, const RegistrationFrame &Frame,
                    llvm::ArrayRef<const llvm::Function *> Functions,
                    const std::set<const llvm::StoreInst *> &RuntimeWrites);

struct DynamicSEHScopeCase {
  const llvm::InvokeInst *Enter;
  const llvm::InvokeInst *Exit;
  const llvm::BasicBlock *Unwind;
};

llvm::Error
validateDynamicSEHScope(const RegistrationBlockState &Source,
                        const RegistrationFrame &Frame,
                        const SourceSegment &Segment,
                        const llvm::CallInst &Enter, const llvm::CallInst &Exit,
                        const std::map<int32_t, DynamicSEHScopeCase> &Cases,
                        std::set<const llvm::CallBase *> &Calls);

llvm::Error
validateDynamicSEHCallMarkers(llvm::ArrayRef<const llvm::Function *> Functions,
                              const std::set<const llvm::CallBase *> &Calls);
} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHDYNAMICSCOPE_H
