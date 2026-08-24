//===- SBFVMConfig.h - Solana SBF VM configuration --------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_RUNTIME_SBFVMCONFIG_H
#define NEVERD_SBF_RUNTIME_SBFVMCONFIG_H

#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/image/SBFVersion.h"

#include "llvm/Support/Error.h"

#include <cstddef>

namespace neverd::sbf {

/// Runtime choices which affect the observable SBF address-space contract.
/// Defaults mirror anza-xyz/sbpf's Config::default().
struct SBFVMConfig {
  size_t StackFrameSize = kDefaultStackFrameSize;
  size_t MaxCallDepth = kDefaultMaxCallDepth;
  bool EnableStackFrameGaps = true;
  bool OptimizeRodata = true;
  bool AlignedMemoryMapping = false;
  bool RejectBrokenELFs = false;
};

#define SBF_VM_CONFIG_LIMIT(NAME, VALUE)                                       \
  inline constexpr size_t k##NAME = VALUE;
#include "neverd/sbf/runtime/SBFVMConfigLimits.def"
static_assert(kDefaultMaxCallDepth <= kMaximumHostCallDepth,
              "the default VM call depth must fit the host envelope");
static_assert(kDefaultStackFrameSize * kDefaultMaxCallDepth <=
                  kMaximumHostStackByteCount,
              "the default VM stack must fit the host envelope");
static_assert(kMaximumHostStackByteCount < kMemoryRegionSize,
              "the host stack envelope must fit one VM region");

llvm::Error validateVMConfig(const SBFVMConfig &Config);

/// Returns the eager stack backing size after validateVMConfig() succeeds.
constexpr size_t stackSize(const SBFVMConfig &Config) {
  return Config.StackFrameSize * Config.MaxCallDepth;
}

constexpr bool usesStackFrameGaps(Version V, const SBFVMConfig &Config) {
  return Config.EnableStackFrameGaps &&
         versionHasFeature(V, VersionFeature::StackFrameGaps);
}

/// Computes the initial pointer after validateVMConfig() succeeds.
constexpr uint64_t initialFramePointer(Version V, const SBFVMConfig &Config) {
  return kStackStart + (versionHasFeature(V, VersionFeature::ManualStackFrames)
                            ? stackSize(Config)
                            : Config.StackFrameSize);
}

/// Computes the automatic stride after validateVMConfig() succeeds.
constexpr uint64_t automaticFrameStride(Version V, const SBFVMConfig &Config) {
  return Config.StackFrameSize *
         (usesStackFrameGaps(V, Config) ? kStackFrameGapMultiplier : 1);
}

} // namespace neverd::sbf

#endif // NEVERD_SBF_RUNTIME_SBFVMCONFIG_H
