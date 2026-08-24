//===- SBFRuntimeProfile.cpp - Which runtime the answer is about ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/runtime/SBFRuntimeProfile.h"

#include "llvm/ADT/Twine.h"

#include <array>

namespace neverd::sbf {
namespace {

llvm::Error profileError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("sbf: runtime profile: " + Message).str(),
      llvm::inconvertibleErrorCode());
}

/// One cluster switching one gate on, at the slot it happened.
struct ActivationRecord {
  RuntimeFeature Feature;
  Cluster OnCluster;
  uint64_t Slot;
};

llvm::ArrayRef<ActivationRecord> activationRecords() {
  static const std::array Table = {
#define SBF_RUNTIME_FEATURE_ACTIVATION(ID, CLUSTER, SLOT)                      \
  ActivationRecord{RuntimeFeature::ID, Cluster::CLUSTER, (SLOT)},
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  };
  return Table;
}

} // namespace

//===----------------------------------------------------------------------===//
// The chain
//===----------------------------------------------------------------------===//

llvm::ArrayRef<ClusterInfo> clusterInfos() {
  static const std::array Table = {
#define SBF_CLUSTER(ID, NAME, ACTIVATES_EVERYTHING, SUMMARY)                   \
  ClusterInfo{Cluster::ID, NAME, (ACTIVATES_EVERYTHING), SUMMARY},
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  };
  return Table;
}

const ClusterInfo &getClusterInfo(Cluster ID) {
  return clusterInfos()[static_cast<size_t>(ID)];
}

llvm::StringRef clusterName(Cluster ID) { return getClusterInfo(ID).Name; }

std::optional<Cluster> parseCluster(llvm::StringRef Name) {
  for (const ClusterInfo &Info : clusterInfos())
    if (Info.Name == Name)
      return Info.ID;
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// What is being asked
//===----------------------------------------------------------------------===//

llvm::ArrayRef<RuntimePurposeInfo> runtimePurposeInfos() {
  static const std::array Table = {
#define SBF_RUNTIME_PURPOSE(ID, NAME, SUMMARY)                                 \
  RuntimePurposeInfo{RuntimePurpose::ID, NAME, SUMMARY},
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  };
  return Table;
}

llvm::StringRef runtimePurposeName(RuntimePurpose ID) {
  return runtimePurposeInfos()[static_cast<size_t>(ID)].Name;
}

std::optional<RuntimePurpose> parseRuntimePurpose(llvm::StringRef Name) {
  for (const RuntimePurposeInfo &Info : runtimePurposeInfos())
    if (Info.Name == Name)
      return Info.ID;
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// The loader
//===----------------------------------------------------------------------===//

llvm::ArrayRef<LoaderInfo> loaderInfos() {
  static const std::array Table = {
#define SBF_LOADER(ID, NAME, KNOWN_ADDRESS, ACCOUNT_ABI, DEPLOYS, EXECUTES,    \
                   SUMMARY)                                                    \
  LoaderInfo{Loader::ID,                                                       \
             NAME,                                                             \
             KnownAddress::KNOWN_ADDRESS,                                      \
             AccountABI::ACCOUNT_ABI,                                          \
             (DEPLOYS),                                                        \
             (EXECUTES),                                                       \
             SUMMARY},
#include "neverd/sbf/runtime/SBFLoaders.def"
  };
  return Table;
}

const LoaderInfo &getLoaderInfo(Loader ID) {
  return loaderInfos()[static_cast<size_t>(ID)];
}

llvm::StringRef loaderName(Loader ID) { return getLoaderInfo(ID).Name; }

std::optional<Loader> parseLoader(llvm::StringRef Name) {
  for (const LoaderInfo &Info : loaderInfos())
    if (Info.Name == Name)
      return Info.ID;
  return std::nullopt;
}

std::optional<Loader> loaderForAddress(KnownAddress Address) {
  for (const LoaderInfo &Info : loaderInfos())
    if (Info.Address == Address)
      return Info.ID;
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// The gates
//===----------------------------------------------------------------------===//

llvm::ArrayRef<RuntimeFeatureDomainInfo> runtimeFeatureDomainInfos() {
  static const std::array Table = {
#define SBF_RUNTIME_FEATURE_DOMAIN(ID, NAME, SUMMARY)                          \
  RuntimeFeatureDomainInfo{RuntimeFeatureDomain::ID, NAME, SUMMARY},
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  };
  return Table;
}

llvm::StringRef runtimeFeatureDomainName(RuntimeFeatureDomain Domain) {
  if (!isValidRuntimeFeatureDomain(Domain))
    return {};
  return runtimeFeatureDomainInfos()[static_cast<size_t>(Domain)].Name;
}

llvm::ArrayRef<RuntimeFeatureDispositionInfo> runtimeFeatureDispositionInfos() {
  static const std::array Table = {
#define SBF_RUNTIME_FEATURE_DISPOSITION(ID, NAME, SUMMARY)                     \
  RuntimeFeatureDispositionInfo{RuntimeFeatureDisposition::ID, NAME, SUMMARY},
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  };
  return Table;
}

llvm::StringRef
runtimeFeatureDispositionName(RuntimeFeatureDisposition Disposition) {
  if (!isValidRuntimeFeatureDisposition(Disposition))
    return {};
  return runtimeFeatureDispositionInfos()[static_cast<size_t>(Disposition)]
      .Name;
}

llvm::ArrayRef<RuntimeFeatureInfo> runtimeFeatureInfos() {
  static const std::array Table = {
#define SBF_RUNTIME_FEATURE(ID, NAME, GATE, ADDRESS, SIMD, DOMAIN,             \
                            DISPOSITION, SUMMARY)                              \
  RuntimeFeatureInfo{RuntimeFeature::ID,                                       \
                     NAME,                                                     \
                     GATE,                                                     \
                     ADDRESS,                                                  \
                     SIMD,                                                     \
                     RuntimeFeatureDomain::DOMAIN,                             \
                     RuntimeFeatureDisposition::DISPOSITION,                   \
                     SUMMARY},
#include "neverd/sbf/runtime/SBFRuntimeFeatures.def"
  };
  return Table;
}

const RuntimeFeatureInfo &getRuntimeFeatureInfo(RuntimeFeatureBit Bit) {
  return runtimeFeatureInfos()[static_cast<size_t>(Bit)];
}

const RuntimeFeatureInfo *getRuntimeFeatureInfo(RuntimeFeature Feature) {
  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos())
    if (Info.ID == Feature)
      return &Info;
  return nullptr;
}

llvm::StringRef runtimeFeatureName(RuntimeFeature Feature) {
  const RuntimeFeatureInfo *Info = getRuntimeFeatureInfo(Feature);
  return Info ? llvm::StringRef(Info->Name) : llvm::StringRef();
}

std::optional<RuntimeFeature> parseRuntimeFeature(llvm::StringRef Name) {
  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos())
    if (Info.Name == Name || Info.Gate == Name)
      return Info.ID;
  return std::nullopt;
}

std::optional<uint64_t> runtimeFeatureActivation(RuntimeFeature Feature,
                                                 Cluster OnCluster) {
  for (const ActivationRecord &Record : activationRecords())
    if (Record.Feature == Feature && Record.OnCluster == OnCluster)
      return Record.Slot;
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// The profile
//===----------------------------------------------------------------------===//

AccountABI RuntimeProfile::accountABI() const {
  return getLoaderInfo(OwningLoader).ABI;
}

RuntimeProfile currentMainnetProfile() { return RuntimeProfile{}; }

bool isFeatureActive(const RuntimeProfile &Profile, RuntimeFeature Feature) {
  // An override is an answer about a validator somebody actually configured,
  // or about a proposal nobody has shipped. Either way it outranks the chain,
  // because the chain is not the thing being asked about.
  if (hasFeature(Profile.Suppressed, Feature))
    return false;
  if (hasFeature(Profile.Forced, Feature))
    return true;
  if (getClusterInfo(Profile.OnCluster).ActivatesEverything)
    return true;
  const std::optional<uint64_t> Slot =
      runtimeFeatureActivation(Feature, Profile.OnCluster);
  return Slot && Profile.Slot >= *Slot;
}

RuntimeFeature activeFeatures(const RuntimeProfile &Profile) {
  RuntimeFeature Active = RuntimeFeature::None;
  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos())
    if (isFeatureActive(Profile, Info.ID))
      Active = Active | Info.ID;
  return Active;
}

llvm::Error validateRuntimeProfileTables() {
  for (const LoaderInfo &Info : loaderInfos()) {
    const KnownAddressInfo *Address = getKnownAddressInfo(Info.Address);
    if (!Address)
      return profileError("loader '" + Info.Name + "' names no address");
    // A loader that is not recognized as one cannot be matched against a
    // program's owner, which is the only way the serialization is ever
    // decided from evidence rather than from a default.
    if (Address->Category != KnownAddressCategory::Loader)
      return profileError("loader '" + Info.Name + "' names '" + Address->Name +
                          "', which the address table does not call a loader");
  }

  for (const ActivationRecord &Record : activationRecords()) {
    const RuntimeFeatureInfo *Feature = getRuntimeFeatureInfo(Record.Feature);
    if (!Feature)
      return profileError("an activation names a gate no table declares");
    if (getClusterInfo(Record.OnCluster).ActivatesEverything)
      return profileError(
          "gate '" + Feature->Name + "' records an activation on '" +
          clusterName(Record.OnCluster) +
          "', which activates everything and therefore has no slot to record");
  }

  for (const RuntimeFeatureInfo &Info : runtimeFeatureInfos()) {
    if (Info.Gate.empty())
      return profileError("gate '" + Info.Name + "' has no runtime identifier");
    if (Info.Address.empty())
      return profileError("gate '" + Info.Name +
                          "' has no account, so no "
                          "activation recorded for it can be checked");
  }
  return llvm::Error::success();
}

} // namespace neverd::sbf
