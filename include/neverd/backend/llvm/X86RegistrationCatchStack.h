//===- X86RegistrationCatchStack.h - PE32 catch scratch stack -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCHSTACK_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCHSTACK_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <vector>

namespace llvm {
class BasicBlock;
class StoreInst;
} // namespace llvm

namespace neverd {
struct X86RegistrationCatchStackResume {
  llvm::StoreInst *Seed = nullptr;
  int32_t Offset = 0;
  uint64_t TargetVA = 0;
  /// A nested dispatch observes the source block's incoming private cells.
  std::vector<llvm::BasicBlock *> Sources;
};
/// Bound an invocation's scratch stack before replacing its exact ESP seed.
/// SourceStores are authenticated source occurrences: the LowIR contract
/// permits a callback pointer there only for the parent's SavedESP cell.
/// Installation must independently recheck those actual LLVM destinations.
llvm::Expected<uint32_t> checkX86RegistrationCatchStack(
    llvm::BasicBlock &Entry, llvm::ArrayRef<llvm::BasicBlock *> Body,
    llvm::StoreInst &Seed, llvm::ArrayRef<llvm::StoreInst *> SourceStores,
    llvm::ArrayRef<X86RegistrationCatchStackResume> Resumes = {});
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86REGISTRATIONCATCHSTACK_H
