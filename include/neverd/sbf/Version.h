//===- Version.h - Solana SBF version feature model ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_VERSION_H
#define NEVERD_SBF_VERSION_H

#include "neverd/sbf/SBFConstants.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace neverd::sbf {

enum class Version : uint8_t {
#define SBF_VERSION_AUTO(NAME, SPELLING, DISPLAY_NAME) NAME = 0xff,
#define SBF_VERSION(NAME, ELF_FLAGS, SPELLING, DISPLAY_NAME, FEATURES, STATUS) \
  NAME = ELF_FLAGS,
#include "neverd/sbf/SBFVersions.def"
  Reserved = 0xfe,
};

enum class VersionStatus : uint8_t { Legacy, Current, Upstream };

enum class VersionFeature : uint32_t {
  None = 0,
  StackFrameGaps = 1u << 0,
  ManualStackFrames = 1u << 1,
  PQR = 1u << 2,
  ExplicitSignExtension = 1u << 3,
  SwapSubImmediate = 1u << 4,
  DisableNeg = 1u << 5,
  CallXSource = 1u << 6,
  DisableLDDW = 1u << 7,
  DisableLE = 1u << 8,
  MovedMemory = 1u << 9,
  StaticSyscalls = 1u << 10,
  StrictELF = 1u << 11,
  LowerRodata = 1u << 12,
  JMP32 = 1u << 13,
  CallXDestination = 1u << 14,
  AlignedMemoryMapping = 1u << 15,
};

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
#include "neverd/sbf/SBFVersions.def"
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
#include "neverd/sbf/SBFVersions.def"
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
#include "neverd/sbf/SBFVersions.def"
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

llvm::ArrayRef<VersionInfo> versionInfos();
const VersionInfo *getVersionInfo(Version V);
llvm::StringRef versionName(Version V);
llvm::StringRef versionDisplayName(Version V);
std::optional<Version> parseVersion(llvm::StringRef Name);

} // namespace neverd::sbf

#endif // NEVERD_SBF_VERSION_H
