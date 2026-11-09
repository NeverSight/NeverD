//===- MedLLVMSourceSEHFilter.h - Source filter outlining -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preflight an owned filter and retain its parent storage explicitly in C.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMSOURCESEHFILTER_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMSOURCESEHFILTER_H

#include "WindowsRegistrationFramePrivate.h"

namespace neverd {
struct MedFunc;
struct MedOp;
namespace x86_registration {

bool isSourceFilterReturn(const MedFunc &Function, const MedOp &Return);

/// Outlining consumes the temporary result. A refused projection must not
/// retain dead filter results in the unchanged parent CFG.
void discardSourceFilterResults(llvm::Function &Parent);

struct SourceFilterPlan {
  CallbackPlan Callback;
  std::vector<llvm::Value *> Captures;
  std::vector<llvm::StoreInst *> Initializers;
};

/// Request must outlive the returned plan. All checks precede mutation.
llvm::Expected<SourceFilterPlan>
prepareSourceFilter(llvm::Function &Parent,
                    const X86RegistrationCallbackRequest &Request,
                    size_t &WorkUsed);

struct SourceFilter {
  llvm::Function *Function = nullptr;
  std::vector<llvm::Value *> Captures;
};

/// Commit a preflighted plan. Original blocks stay until every scope commits.
SourceFilter commitSourceFilter(llvm::Function &Parent,
                                const SourceFilterPlan &Plan);

} // namespace x86_registration
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_MEDLLVMSOURCESEHFILTER_H
