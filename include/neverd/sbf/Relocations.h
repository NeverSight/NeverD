//===- Relocations.h - Solana SBF relocation metadata --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_RELOCATIONS_H
#define NEVERD_SBF_RELOCATIONS_H

#include "neverd/sbf/Version.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace neverd::sbf {

enum class Relocation : uint32_t {
#define SBF_RELOCATION(ID, VALUE, NAME, WIDTH_BITS, PURPOSE, FIELD_LAYOUT,     \
                       TARGETS, SYMBOL_REQUIREMENT, VERSION_POLICY)            \
  ID = VALUE,
#include "neverd/sbf/SBFRelocations.def"
  Unknown = std::numeric_limits<uint32_t>::max(),
};

enum class RelocationPurpose : uint8_t { Absolute, Relative, Call };
enum class RelocationFieldLayout : uint8_t {
  SplitLDDWImmediate,
  SplitLDDWOrData,
  CallImmediate,
};
enum class RelocationTargetKind : uint8_t {
  None = 0,
  TextLDDW = 1u << 0,
  TextCall = 1u << 1,
  ReadOnlyData = 1u << 2,
};
enum class RelocationSymbolRequirement : uint8_t { None, Required };
enum class RelocationVersionPolicy : uint8_t { LegacyOnly };

constexpr RelocationTargetKind operator|(RelocationTargetKind Left,
                                         RelocationTargetKind Right) {
  return static_cast<RelocationTargetKind>(static_cast<uint8_t>(Left) |
                                           static_cast<uint8_t>(Right));
}

constexpr bool hasTarget(RelocationTargetKind Set,
                         RelocationTargetKind Target) {
  return (static_cast<uint8_t>(Set) & static_cast<uint8_t>(Target)) != 0;
}

struct RelocationInfo {
  Relocation ID;
  uint32_t Value;
  llvm::StringLiteral Name;
  uint8_t Width;
  RelocationPurpose Purpose;
  RelocationFieldLayout FieldLayout;
  RelocationTargetKind Targets;
  RelocationSymbolRequirement SymbolRequirement;
  RelocationVersionPolicy VersionPolicy;
};

llvm::ArrayRef<RelocationInfo> relocationInfos();
const RelocationInfo *getRelocationInfo(uint32_t Value);
bool isRelocationAllowedForVersion(const RelocationInfo &Info, Version V);

/// Reproduce the legacy sbpf function-registry key for an internal target.
uint32_t legacyFunctionKey(size_t TargetSlot, llvm::StringRef Name);

} // namespace neverd::sbf

#endif // NEVERD_SBF_RELOCATIONS_H
