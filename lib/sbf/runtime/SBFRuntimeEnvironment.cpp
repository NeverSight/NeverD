//===- SBFRuntimeEnvironment.cpp - Resolved Solana runtime ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/runtime/SBFRuntimeEnvironment.h"

#include "neverd/sbf/runtime/SBFSyscalls.h"

#include "llvm/ADT/Twine.h"

#include <algorithm>
#include <array>
#include <optional>
#include <type_traits>
#include <utility>

namespace neverd::sbf {
namespace {

llvm::Error environmentError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("sbf: runtime environment: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

constexpr auto versionNumber(Version TheVersion) {
  return static_cast<std::underlying_type_t<Version>>(TheVersion);
}

struct RuntimeVersionRange {
  Version Minimum;
  Version Maximum;
};

struct ChainVersionGate {
  Version TheVersion;
  RuntimeFeature EnableFeature;
};

llvm::ArrayRef<ChainVersionGate> chainVersionGates() {
  static const std::array Table = {
#define SBF_RUNTIME_VERSION(VERSION, ENABLE_FEATURE)                           \
  ChainVersionGate{Version::VERSION, RuntimeFeature::ENABLE_FEATURE},
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  };
  return Table;
}

llvm::Expected<RuntimeVersionRange>
resolveVersionRange(const RuntimeProfile &Profile,
                    RuntimeVersionPolicy Policy) {
  std::optional<Version> EarliestKnown;
  std::optional<Version> LatestKnown;
  std::optional<Version> EarliestChain;
  for (const VersionInfo &Info : versionInfos()) {
    if (!EarliestKnown ||
        versionNumber(Info.Value) < versionNumber(*EarliestKnown))
      EarliestKnown = Info.Value;
    if (!LatestKnown || versionNumber(Info.Value) > versionNumber(*LatestKnown))
      LatestKnown = Info.Value;
  }
  if (!EarliestKnown || !LatestKnown)
    return environmentError("version database cannot describe a runtime range");

  if (Policy == RuntimeVersionPolicy::UpstreamToolchain)
    return RuntimeVersionRange{*EarliestKnown, *LatestKnown};
  if (Policy == RuntimeVersionPolicy::ExpertOverride)
    return environmentError(
        "expert version ranges require a complete expert environment");

  std::optional<Version> PreviousVersion;
  std::optional<Version> LatestChain;
  bool SawInactiveVersion = false;
  for (const ChainVersionGate &Record : chainVersionGates()) {
    const VersionInfo *Info = getVersionInfo(Record.TheVersion);
    if (!Info || Info->Status == VersionStatus::Upstream)
      return environmentError(
          "chain version table names a version unavailable in Agave");
    if (PreviousVersion &&
        versionNumber(Record.TheVersion) <= versionNumber(*PreviousVersion))
      return environmentError(
          "chain version table must be strictly ordered without duplicates");
    PreviousVersion = Record.TheVersion;
    if (!EarliestChain)
      EarliestChain = Record.TheVersion;
    const bool IsActive = Record.EnableFeature == RuntimeFeature::None ||
                          isFeatureActive(Profile, Record.EnableFeature);
    if (!IsActive) {
      SawInactiveVersion = true;
      continue;
    }
    if (SawInactiveVersion)
      return environmentError(
          "chain version feature activations are not contiguous");
    LatestChain = Record.TheVersion;
  }

  for (const VersionInfo &Info : versionInfos()) {
    const size_t MappingCount = static_cast<size_t>(
        std::count_if(chainVersionGates().begin(), chainVersionGates().end(),
                      [&](const ChainVersionGate &Record) {
                        return Record.TheVersion == Info.Value;
                      }));
    const size_t ExpectedCount = Info.Status == VersionStatus::Upstream ? 0 : 1;
    if (MappingCount != ExpectedCount)
      return environmentError(
          "chain version table does not cover the Agave version database");
  }

  if (!EarliestChain || !LatestChain)
    return environmentError("chain has no enabled SBF version");

  auto MinimumVersion =
      minimumAgaveVersion(Profile.Purpose, activeFeatures(Profile));
  if (!MinimumVersion)
    return MinimumVersion.takeError();
  if (versionNumber(*MinimumVersion) > versionNumber(*LatestChain))
    return environmentError(
        "runtime policy disables every chain-enabled SBF version");
  return RuntimeVersionRange{*MinimumVersion, *LatestChain};
}

} // namespace

llvm::Expected<Version> minimumAgaveVersion(RuntimePurpose Purpose,
                                            RuntimeFeature ActiveFeatures) {
  std::optional<Version> EarliestChain;
  std::optional<Version> EarliestStaticSyscalls;
  std::optional<Version> Previous;
  for (const ChainVersionGate &Record : chainVersionGates()) {
    const VersionInfo *Info = getVersionInfo(Record.TheVersion);
    if (!Info || Info->Status == VersionStatus::Upstream)
      return environmentError(
          "chain version table names a version unavailable in Agave");
    if (Previous &&
        versionNumber(Record.TheVersion) <= versionNumber(*Previous))
      return environmentError(
          "chain version table must be strictly ordered without duplicates");
    Previous = Record.TheVersion;
    if (!EarliestChain)
      EarliestChain = Record.TheVersion;
    if (!EarliestStaticSyscalls &&
        versionHasFeature(Record.TheVersion, VersionFeature::StaticSyscalls))
      EarliestStaticSyscalls = Record.TheVersion;
  }
  if (!EarliestChain || !EarliestStaticSyscalls)
    return environmentError(
        "chain version table cannot describe Agave's minimum version");

  const bool V0Disabled =
      hasFeature(ActiveFeatures, RuntimeFeature::DisableSBPFV0Execution);
  const bool V0Reenabled =
      hasFeature(ActiveFeatures, RuntimeFeature::ReenableSBPFV0Execution);
  if ((V0Disabled && !V0Reenabled) ||
      (Purpose == RuntimePurpose::Deployment &&
       hasFeature(ActiveFeatures, RuntimeFeature::DisableLegacyDeployment)))
    return *EarliestStaticSyscalls;
  return *EarliestChain;
}

ResolvedRuntimeEnvironment::ResolvedRuntimeEnvironment(
    SBFVMConfig Config, Version MinimumVersion, Version MaximumVersion,
    RuntimeEnvironmentOrigin Origin, RuntimeVersionPolicy VersionPolicy,
    std::optional<RuntimeProfile> Profile, RuntimeFeature ActiveRuntimeFeatures,
    AccountABI InputABI, std::vector<uint32_t> RegisteredSyscallHashes)
    : Config(Config), MinimumVersion(MinimumVersion),
      MaximumVersion(MaximumVersion), Origin(Origin),
      VersionPolicy(VersionPolicy), Profile(std::move(Profile)),
      ActiveRuntimeFeatures(ActiveRuntimeFeatures), InputABI(InputABI),
      RegisteredSyscallHashes(std::move(RegisteredSyscallHashes)) {}

bool ResolvedRuntimeEnvironment::supportsVersion(Version TheVersion) const {
  return isConcreteVersion(TheVersion) &&
         versionNumber(TheVersion) >= versionNumber(MinimumVersion) &&
         versionNumber(TheVersion) <= versionNumber(MaximumVersion);
}

bool ResolvedRuntimeEnvironment::isSyscallRegistered(uint32_t Hash) const {
  return std::binary_search(RegisteredSyscallHashes.begin(),
                            RegisteredSyscallHashes.end(), Hash);
}

llvm::Expected<ResolvedRuntimeEnvironment>
resolveRuntimeEnvironment(const RuntimeProfile &Profile,
                          RuntimeVersionPolicy VersionPolicy) {
  if (!isValidCluster(Profile.OnCluster))
    return environmentError("profile names an unknown cluster");
  if (!isValidRuntimePurpose(Profile.Purpose))
    return environmentError("profile names an unknown runtime purpose");
  if (!isValidLoader(Profile.OwningLoader))
    return environmentError("profile names an unknown loader");
  if (VersionPolicy >= RuntimeVersionPolicy::Count)
    return environmentError("runtime version policy is unknown");
  const RuntimeFeatureMask KnownFeatureBits = knownRuntimeFeatureMask();
  if ((runtimeFeatureMask(Profile.Forced) & ~KnownFeatureBits) != 0 ||
      (runtimeFeatureMask(Profile.Suppressed) & ~KnownFeatureBits) != 0)
    return environmentError("profile feature overrides contain an unknown bit");
  const RuntimeFeatureMask ConflictingOverrides =
      runtimeFeatureMask(Profile.Forced) &
      runtimeFeatureMask(Profile.Suppressed);
  if (ConflictingOverrides != 0)
    return environmentError("a feature cannot be both forced and suppressed");

  auto VersionRange = resolveVersionRange(Profile, VersionPolicy);
  if (!VersionRange)
    return VersionRange.takeError();

  const bool HasAdjustedAddressSpace =
      isFeatureActive(Profile, RuntimeFeature::VirtualAddressSpaceAdjustments);
  SBFVMConfig Config;
  Config.EnableStackFrameGaps = !HasAdjustedAddressSpace;
  Config.OptimizeRodata = false;
  Config.AlignedMemoryMapping = !HasAdjustedAddressSpace;
  Config.RejectBrokenELFs = Profile.Purpose == RuntimePurpose::Deployment;

  const RuntimeFeature ActiveFeatures = activeFeatures(Profile);
  std::vector<uint32_t> RegisteredSyscallHashes =
      registeredSyscallHashes(Profile.Purpose, ActiveFeatures);

  return ResolvedRuntimeEnvironment(
      Config, VersionRange->Minimum, VersionRange->Maximum,
      RuntimeEnvironmentOrigin::Agave, VersionPolicy, Profile, ActiveFeatures,
      Profile.accountABI(), std::move(RegisteredSyscallHashes));
}

llvm::Expected<ResolvedRuntimeEnvironment> resolveExpertRuntimeEnvironment(
    const ExpertRuntimeEnvironmentOverride &Override) {
  if (!isConcreteVersion(Override.MinimumVersion))
    return environmentError("custom minimum version is not concrete");
  if (!isConcreteVersion(Override.MaximumVersion))
    return environmentError("custom maximum version is not concrete");
  if (versionNumber(Override.MinimumVersion) >
      versionNumber(Override.MaximumVersion))
    return environmentError("custom minimum version exceeds maximum version");
  if (llvm::Error Error = validateVMConfig(Override.VMConfig))
    return std::move(Error);

  const RuntimeFeatureMask KnownFeatureBits = knownRuntimeFeatureMask();
  if ((runtimeFeatureMask(Override.ActiveRuntimeFeatures) &
       ~KnownFeatureBits) != 0)
    return environmentError("custom feature set contains an unknown bit");
  if (std::none_of(accountABIInfos().begin(), accountABIInfos().end(),
                   [&](const AccountABIInfo &Info) {
                     return Info.ID == Override.InputABI;
                   }))
    return environmentError("custom input ABI is not tabulated");

  std::vector<uint32_t> RegisteredSyscallHashes =
      Override.RegisteredSyscallHashes;
  std::sort(RegisteredSyscallHashes.begin(), RegisteredSyscallHashes.end());
  RegisteredSyscallHashes.erase(std::unique(RegisteredSyscallHashes.begin(),
                                            RegisteredSyscallHashes.end()),
                                RegisteredSyscallHashes.end());

  return ResolvedRuntimeEnvironment(
      Override.VMConfig, Override.MinimumVersion, Override.MaximumVersion,
      RuntimeEnvironmentOrigin::Custom, RuntimeVersionPolicy::ExpertOverride,
      std::nullopt, Override.ActiveRuntimeFeatures, Override.InputABI,
      std::move(RegisteredSyscallHashes));
}

} // namespace neverd::sbf
