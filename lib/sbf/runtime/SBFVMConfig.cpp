//===- SBFVMConfig.cpp - Solana SBF VM configuration ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/Support/Error.h"

#include <limits>

namespace neverd::sbf {

llvm::Error validateVMConfig(const SBFVMConfig &Config) {
  if (Config.StackFrameSize == 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: stack frame size must be non-zero",
        llvm::inconvertibleErrorCode());
  if (Config.MaxCallDepth == 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: maximum call depth must be non-zero",
        llvm::inconvertibleErrorCode());
  if (Config.MaxCallDepth >
      std::numeric_limits<size_t>::max() / Config.StackFrameSize)
    return llvm::make_error<llvm::StringError>(
        "sbf: configured stack size overflows", llvm::inconvertibleErrorCode());
  const size_t StackSize = Config.StackFrameSize * Config.MaxCallDepth;
  if (StackSize >= kMemoryRegionSize)
    return llvm::make_error<llvm::StringError>(
        "sbf: configured stack exceeds one VM region",
        llvm::inconvertibleErrorCode());
  return llvm::Error::success();
}

} // namespace neverd::sbf
