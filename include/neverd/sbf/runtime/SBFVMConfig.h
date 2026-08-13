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
};

llvm::Error validateVMConfig(const SBFVMConfig &Config);

constexpr size_t stackSize(const SBFVMConfig &Config) {
  return Config.StackFrameSize * Config.MaxCallDepth;
}

constexpr bool usesStackFrameGaps(Version V, const SBFVMConfig &Config) {
  return Config.EnableStackFrameGaps &&
         versionHasFeature(V, VersionFeature::StackFrameGaps);
}

constexpr uint64_t initialFramePointer(Version V, const SBFVMConfig &Config) {
  return kStackStart + (versionHasFeature(V, VersionFeature::ManualStackFrames)
                            ? stackSize(Config)
                            : Config.StackFrameSize);
}

constexpr uint64_t automaticFrameStride(Version V, const SBFVMConfig &Config) {
  return Config.StackFrameSize *
         (usesStackFrameGaps(V, Config) ? kStackFrameGapMultiplier : 1);
}

} // namespace neverd::sbf

#endif // NEVERD_SBF_RUNTIME_SBFVMCONFIG_H
