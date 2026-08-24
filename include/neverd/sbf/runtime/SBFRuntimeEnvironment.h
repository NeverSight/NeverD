//===- SBFRuntimeEnvironment.h - Resolved Solana runtime -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Resolves a chain profile and an explicit version-range authority into one
/// immutable analysis environment.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_RUNTIME_SBFRUNTIMEENVIRONMENT_H
#define NEVERD_SBF_RUNTIME_SBFRUNTIMEENVIRONMENT_H

#include "neverd/sbf/image/SBFVersion.h"
#include "neverd/sbf/runtime/SBFRuntimeProfile.h"
#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::sbf {

enum class RuntimeEnvironmentOrigin : uint8_t { Agave, Custom };

/// Chooses the authority for the executable's accepted SBF-version range.
/// VM knobs, feature gates, and syscall registration still come from the
/// selected RuntimeProfile; this policy only answers which known ISA versions
/// an analysis may open.
enum class RuntimeVersionPolicy : uint8_t {
  ChainProfile,
  UpstreamToolchain,
  ExpertOverride,
  Count,
};

/// A self-consistent runtime and analysis policy. There are deliberately no
/// setters: both authorities are resolved once, then every consumer observes
/// the same answer.
class ResolvedRuntimeEnvironment final {
public:
  ResolvedRuntimeEnvironment(const ResolvedRuntimeEnvironment &) = default;
  ResolvedRuntimeEnvironment(ResolvedRuntimeEnvironment &&) = default;
  ResolvedRuntimeEnvironment &
  operator=(const ResolvedRuntimeEnvironment &) = delete;
  ResolvedRuntimeEnvironment &operator=(ResolvedRuntimeEnvironment &&) = delete;

  [[nodiscard]] const SBFVMConfig &vmConfig() const { return Config; }
  [[nodiscard]] Version minimumVersion() const { return MinimumVersion; }
  [[nodiscard]] Version maximumVersion() const { return MaximumVersion; }
  [[nodiscard]] RuntimeEnvironmentOrigin origin() const { return Origin; }
  [[nodiscard]] RuntimeVersionPolicy versionPolicy() const {
    return VersionPolicy;
  }
  /// The named Agave chain profile that produced these facts. Custom
  /// environments have no profile: their feature set, input ABI, and function
  /// registry are supplied directly and are never reconstructed from Agave's
  /// tables by a downstream consumer.
  [[nodiscard]] const std::optional<RuntimeProfile> &profile() const {
    return Profile;
  }
  [[nodiscard]] RuntimeFeature activeRuntimeFeatures() const {
    return ActiveRuntimeFeatures;
  }
  [[nodiscard]] AccountABI accountABI() const { return InputABI; }
  /// The exact function-registry keys available while this environment loads
  /// legacy relocation records.  The range is sorted and contains no
  /// duplicates, so membership checks do not need a second registry model.
  [[nodiscard]] llvm::ArrayRef<uint32_t> registeredSyscallHashes() const {
    return RegisteredSyscallHashes;
  }
  [[nodiscard]] bool isSyscallRegistered(uint32_t Hash) const;
  [[nodiscard]] bool isCustom() const {
    return Origin == RuntimeEnvironmentOrigin::Custom;
  }
  [[nodiscard]] bool supportsVersion(Version TheVersion) const;

private:
  ResolvedRuntimeEnvironment(SBFVMConfig Config, Version MinimumVersion,
                             Version MaximumVersion,
                             RuntimeEnvironmentOrigin Origin,
                             RuntimeVersionPolicy VersionPolicy,
                             std::optional<RuntimeProfile> Profile,
                             RuntimeFeature ActiveRuntimeFeatures,
                             AccountABI InputABI,
                             std::vector<uint32_t> RegisteredSyscallHashes);

  SBFVMConfig Config;
  Version MinimumVersion;
  Version MaximumVersion;
  RuntimeEnvironmentOrigin Origin;
  RuntimeVersionPolicy VersionPolicy;
  std::optional<RuntimeProfile> Profile;
  RuntimeFeature ActiveRuntimeFeatures;
  AccountABI InputABI;
  std::vector<uint32_t> RegisteredSyscallHashes;

  friend llvm::Expected<ResolvedRuntimeEnvironment>
  resolveRuntimeEnvironment(const RuntimeProfile &Profile,
                            RuntimeVersionPolicy VersionPolicy);
  friend llvm::Expected<ResolvedRuntimeEnvironment>
  resolveExpertRuntimeEnvironment(
      const struct ExpertRuntimeEnvironmentOverride &Override);
};

/// All fields of a custom policy must be supplied together.  Keeping this API
/// separate from profile resolution prevents a custom VM knob from being
/// silently mixed into an otherwise Agave-labelled result.
struct ExpertRuntimeEnvironmentOverride {
  Version MinimumVersion;
  Version MaximumVersion;
  SBFVMConfig VMConfig;
  /// Complete runtime function registry for legacy relocation resolution.
  /// An empty vector deliberately describes a custom environment with no
  /// registered syscalls; it never falls back to an Agave profile.
  std::vector<uint32_t> RegisteredSyscallHashes;
  /// Complete runtime-feature answer for this custom environment. No cluster
  /// activation table is consulted.
  RuntimeFeature ActiveRuntimeFeatures = RuntimeFeature::None;
  /// Exact serialization handed to the program by its custom loader.
  AccountABI InputABI = AccountABI::V1;
};

/// Earliest SBF version admitted by Agave for one explicit feature snapshot
/// and registry purpose. This is shared by named chain profiles and external
/// conformance snapshots so disable/re-enable and deployment-only gates cannot
/// drift between the two entry points.
llvm::Expected<Version> minimumAgaveVersion(RuntimePurpose Purpose,
                                            RuntimeFeature ActiveFeatures);

/// Resolve \p Profile using the pinned Agave runtime facts and the requested
/// authority for the executable-version range. The exact audited revision is
/// recorded once in SBFUpstreamSources.def.
llvm::Expected<ResolvedRuntimeEnvironment> resolveRuntimeEnvironment(
    const RuntimeProfile &Profile,
    RuntimeVersionPolicy VersionPolicy = RuntimeVersionPolicy::ChainProfile);

/// Build an explicitly custom environment after validating every supplied
/// component.
llvm::Expected<ResolvedRuntimeEnvironment> resolveExpertRuntimeEnvironment(
    const ExpertRuntimeEnvironmentOverride &Override);

} // namespace neverd::sbf

#endif // NEVERD_SBF_RUNTIME_SBFRUNTIMEENVIRONMENT_H
