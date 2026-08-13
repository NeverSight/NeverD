//===- SBFVersion.h - Solana SBF version feature model ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_IMAGE_SBFVERSION_H
#define NEVERD_SBF_IMAGE_SBFVERSION_H

#include "neverd/sbf/SBFConstants.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace neverd::sbf {

enum class Version : uint8_t {
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME) NAME = 0xff,
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  NAME = ELF_FLAGS,
#include "neverd/sbf/image/SBFVersions.def"
  Reserved = 0xfe,
};

enum class VersionStatus : uint8_t { Legacy, Current, Upstream };

/// The position each feature occupies in a feature set. Record order in
/// SBFVersionFeatures.def assigns these, which is safe because no bit is ever
/// serialized: a feature set only ever exists in memory.
enum class VersionFeatureBit : uint8_t {
#define SBF_VERSION_FEATURE(ID, NAME, SIMD, UPSTREAM, SUMMARY) ID,
#include "neverd/sbf/image/SBFVersionFeatures.def"
  Count,
};

inline constexpr size_t kVersionFeatureCount =
    static_cast<size_t>(VersionFeatureBit::Count);

enum class VersionFeature : uint32_t {
  None = 0,
#define SBF_VERSION_FEATURE(ID, NAME, SIMD, UPSTREAM, SUMMARY)                 \
  ID = uint32_t{1} << static_cast<uint32_t>(VersionFeatureBit::ID),
#include "neverd/sbf/image/SBFVersionFeatures.def"
};

static_assert(
    kVersionFeatureCount <=
        std::numeric_limits<std::underlying_type_t<VersionFeature>>::digits,
    "a version feature set must hold every tabulated feature");

constexpr VersionFeature operator|(VersionFeature L, VersionFeature R) {
  return static_cast<VersionFeature>(static_cast<uint32_t>(L) |
                                     static_cast<uint32_t>(R));
}

constexpr bool hasFeature(VersionFeature Set, VersionFeature Feature) {
  return (static_cast<uint32_t>(Set) & static_cast<uint32_t>(Feature)) != 0;
}

constexpr VersionFeature versionFeatures(Version V) {
  switch (V) {
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  case Version::NAME:                                                          \
    return FEATURES;
#include "neverd/sbf/image/SBFVersions.def"
  default:
    return VersionFeature::None;
  }
}

constexpr bool versionHasFeature(Version V, VersionFeature Feature) {
  return hasFeature(versionFeatures(V), Feature);
}

constexpr bool isConcreteVersion(Version V) {
  switch (V) {
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  case Version::NAME:
#include "neverd/sbf/image/SBFVersions.def"
    return true;
  default:
    return false;
  }
}

constexpr Version versionFromELFFlags(uint32_t Flags) {
  switch (Flags) {
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  case ELF_FLAGS:                                                              \
    return Version::NAME;
#include "neverd/sbf/image/SBFVersions.def"
  default:
    return Version::Reserved;
  }
}

enum class VersionMask : uint8_t {
  None = 0,
  V0 = 1u << 0,
  V1 = 1u << 1,
  V2 = 1u << 2,
  V3 = 1u << 3,
  V4 = 1u << 4,
  All = (1u << 5) - 1,
  NotV2 = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4),
  V2Only = 1u << 2,
  V3Plus = (1u << 3) | (1u << 4),
};

constexpr bool versionInMask(Version V, VersionMask Mask) {
  if (!isConcreteVersion(V))
    return false;
  const auto Bit = uint8_t{1} << static_cast<uint8_t>(V);
  return (static_cast<uint8_t>(Mask) & Bit) != 0;
}

struct VersionInfo {
  Version Value;
  uint32_t ELFFlags;
  llvm::StringLiteral Name;
  llvm::StringLiteral DisplayName;
  VersionFeature Features;
  VersionStatus Status;
};

/// One behavioural change a version makes, and the proposal that decided it.
struct VersionFeatureInfo {
  VersionFeature ID;
  llvm::StringLiteral Name;
  /// The SIMD proposal that accepted this change. Several proposals land in
  /// one version and one proposal changes several things, so this is recorded
  /// per feature rather than per version.
  llvm::StringLiteral SIMD;
  /// The predicate anza-xyz/sbpf exposes for the same question.
  llvm::StringLiteral Upstream;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<VersionFeatureInfo> versionFeatureInfos();
const VersionFeatureInfo &getVersionFeatureInfo(VersionFeatureBit Bit);
llvm::StringRef versionFeatureName(VersionFeature Feature);

llvm::ArrayRef<VersionInfo> versionInfos();
const VersionInfo *getVersionInfo(Version V);
llvm::StringRef versionName(Version V);
llvm::StringRef versionDisplayName(Version V);
std::optional<Version> parseVersion(llvm::StringRef Name);

} // namespace neverd::sbf

#endif // NEVERD_SBF_IMAGE_SBFVERSION_H
