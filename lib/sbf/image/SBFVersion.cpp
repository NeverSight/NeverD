//===- SBFVersion.cpp - Solana SBF version metadata -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/image/SBFVersion.h"

#include "llvm/ADT/StringSwitch.h"

#include <array>

namespace neverd::sbf {
namespace {

struct AutoVersionInfo {
  llvm::StringLiteral Name;
  llvm::StringLiteral DisplayName;
};

constexpr std::array AutoVersionTable = {
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME)                         \
  AutoVersionInfo{SPELLING, DISPLAY_NAME},
#include "neverd/sbf/image/SBFVersions.def"
};
static_assert(AutoVersionTable.size() == 1,
              "SBF version database must define one auto entry");

constexpr std::array VersionTable = {
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  VersionInfo{Version::NAME, ELF_FLAGS, SPELLING,                              \
              DISPLAY_NAME,  FEATURES,  VersionStatus::STATUS},
#include "neverd/sbf/image/SBFVersions.def"
};

constexpr std::array VersionFeatureTable = {
#define SBF_VERSION_FEATURE(ID, NAME, SIMD, UPSTREAM, SUMMARY)                 \
  VersionFeatureInfo{VersionFeature::ID, NAME, SIMD, UPSTREAM, SUMMARY},
#include "neverd/sbf/image/SBFVersionFeatures.def"
};
static_assert(VersionFeatureTable.size() == kVersionFeatureCount,
              "every tabulated feature must occupy one bit");

} // namespace

llvm::ArrayRef<VersionInfo> versionInfos() { return VersionTable; }

llvm::ArrayRef<VersionFeatureInfo> versionFeatureInfos() {
  return VersionFeatureTable;
}

const VersionFeatureInfo &getVersionFeatureInfo(VersionFeatureBit Bit) {
  return VersionFeatureTable[static_cast<size_t>(Bit)];
}

llvm::StringRef versionFeatureName(VersionFeature Feature) {
  for (const VersionFeatureInfo &Info : VersionFeatureTable)
    if (Info.ID == Feature)
      return Info.Name;
  // A set of several features is not one feature, and neither is the empty
  // set; naming either would invent a capability the table does not define.
  return "none";
}

const VersionInfo *getVersionInfo(Version V) {
  for (const VersionInfo &Info : VersionTable)
    if (Info.Value == V)
      return &Info;
  return nullptr;
}

llvm::StringRef versionName(Version V) {
  if (V == Version::Auto)
    return AutoVersionTable.front().Name;
  if (const VersionInfo *Info = getVersionInfo(V))
    return Info->Name;
  return "reserved";
}

llvm::StringRef versionDisplayName(Version V) {
  if (V == Version::Auto)
    return AutoVersionTable.front().DisplayName;
  if (const VersionInfo *Info = getVersionInfo(V))
    return Info->DisplayName;
  return "reserved SBF version";
}

std::optional<Version> parseVersion(llvm::StringRef Name) {
  return llvm::StringSwitch<std::optional<Version>>(Name.lower())
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME)                         \
  .Case(SPELLING, Version::NAME)
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  .Case(SPELLING, Version::NAME)
#include "neverd/sbf/image/SBFVersions.def"
      .Default(std::nullopt);
}

} // namespace neverd::sbf
