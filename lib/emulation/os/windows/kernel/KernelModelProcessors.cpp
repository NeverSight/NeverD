//===- KernelModelProcessors.cpp - Single-processor thread affinity ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "KernelModel.h"

namespace neverd::emulation {

llvm::Expected<uint64_t> KernelModel::setSystemAffinity(uint64_t Mask) {
  if (!CurrentExecution || !CurrentThreadKey)
    return llvm::createStringError(
        "system affinity requires an active logical thread");
  // Other masks have no supported processor placement in this profile. The
  // x64 KAFFINITY is pointer-width; upper bits must not be truncated.
  if (Mask != profile::ActiveProcessorMask)
    return llvm::createStringError(
        "system affinity requires the single modeled processor mask");
  // At DISPATCH_LEVEL a migration would be deferred until IRQL is lowered.
  // This profile's only admitted mask already contains its current processor,
  // so no migration or additional ready thread can be fabricated.
  SystemAffinityThreads.insert(CurrentThreadKey);
  return 0;
}

llvm::Expected<uint64_t> KernelModel::revertSystemAffinity() {
  if (!CurrentExecution || !CurrentThreadKey)
    return llvm::createStringError(
        "system affinity requires an active logical thread");
  if (!SystemAffinityThreads.erase(CurrentThreadKey))
    return llvm::createStringError(
        "system affinity restoration has no matching thread setter");
  return 0;
}

} // namespace neverd::emulation
