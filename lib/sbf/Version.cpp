//===- Version.cpp - Solana SBF version metadata ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Version.h"

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
#include "neverd/sbf/SBFVersions.def"
};
static_assert(AutoVersionTable.size() == 1,
              "SBF version database must define one auto entry");

constexpr std::array VersionTable = {
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  VersionInfo{Version::NAME, ELF_FLAGS, SPELLING,                              \
              DISPLAY_NAME,  FEATURES,  VersionStatus::STATUS},
#include "neverd/sbf/SBFVersions.def"
};

} // namespace

llvm::ArrayRef<VersionInfo> versionInfos() { return VersionTable; }

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
#include "neverd/sbf/SBFVersions.def"
      .Default(std::nullopt);
}

} // namespace neverd::sbf
