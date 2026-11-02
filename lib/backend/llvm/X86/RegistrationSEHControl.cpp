//===- RegistrationSEHControl.cpp - Source finally returns ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/X86/X86RegistrationSEHControl.h"
#include "neverd/ir/med/X86/X86RegistrationSEHCall.h"
#include "neverd/ir/med/X86/X86RegistrationSEHParentExit.h"

namespace neverd {

std::optional<std::map<int, va_t>>
getX86RegistrationSEHReturnBlocks(const MedFunc &Function) {
  auto Returns = getRegistrationSEHReturns(Function);
  // Native callbacks must bind every physical exit. Analysis may recover the
  // near-return ABI independently of a parent branch's native projection.
  if (!Returns || !getRegistrationSEHParentExits(Function))
    return std::nullopt;
  return std::move(Returns->ReturnBlocks);
}

} // namespace neverd
