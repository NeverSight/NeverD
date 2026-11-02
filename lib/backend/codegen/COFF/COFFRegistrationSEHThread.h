//===- COFFRegistrationSEHThread.h - Thread read replay ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Rebind current thread reads from independently lifted source occurrences.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_COFF_COFFREGISTRATIONSEHTHREAD_H
#define NEVERD_BACKEND_CODEGEN_COFF_COFFREGISTRATIONSEHTHREAD_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <set>

namespace llvm {
class Function;
class LoadInst;
} // namespace llvm
namespace neverd {
struct MedFunc;
} // namespace neverd

namespace neverd::coff_registration {
llvm::Expected<std::set<const llvm::LoadInst *>>
bindSEHThreadReads(const MedFunc &Source, const llvm::Function &Parent,
                   llvm::ArrayRef<const llvm::Function *> Functions);
} // namespace neverd::coff_registration

#endif // NEVERD_BACKEND_CODEGEN_COFF_COFFREGISTRATIONSEHTHREAD_H
