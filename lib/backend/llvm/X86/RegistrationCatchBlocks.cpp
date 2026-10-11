//===- RegistrationCatchBlocks.cpp - PE32 catch invocation ownership ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

namespace neverd {
std::optional<std::map<int, X86RegistrationCatchIdentity>>
projectX86RegistrationCatchBlocks(const MedFunc &Function) {
  return registrationCatchBlocks(Function);
}
std::optional<std::vector<std::optional<X86RegistrationCatchIdentity>>>
projectX86RegistrationCatchParents(const MedFunc &Function) {
  return registrationCatchParents(Function);
}
} // namespace neverd
