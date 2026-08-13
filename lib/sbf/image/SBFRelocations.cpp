//===- SBFRelocations.cpp - Solana SBF relocation metadata ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/image/SBFRelocations.h"

#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"

#include "llvm/Support/Endian.h"

#include <array>

namespace neverd::sbf {
namespace {

constexpr RelocationTargetKind TextLDDW = RelocationTargetKind::TextLDDW;
constexpr RelocationTargetKind TextCall = RelocationTargetKind::TextCall;
constexpr RelocationTargetKind ReadOnlyData =
    RelocationTargetKind::ReadOnlyData;

constexpr std::array RelocationTable = {
#define SBF_RELOCATION(ID, VALUE, NAME, WIDTH_BITS, PURPOSE, FIELD_LAYOUT,     \
                       TARGETS, SYMBOL_REQUIREMENT, VERSION_POLICY)            \
  RelocationInfo{Relocation::ID,                                               \
                 VALUE,                                                        \
                 NAME,                                                         \
                 WIDTH_BITS,                                                   \
                 RelocationPurpose::PURPOSE,                                   \
                 RelocationFieldLayout::FIELD_LAYOUT,                          \
                 TARGETS,                                                      \
                 RelocationSymbolRequirement::SYMBOL_REQUIREMENT,              \
                 RelocationVersionPolicy::VERSION_POLICY},
#include "neverd/sbf/image/SBFRelocations.def"
};

constexpr bool validateRelocationTable() {
  for (size_t Index = 0; Index < RelocationTable.size(); ++Index) {
    const RelocationInfo &Info = RelocationTable[Index];
    if (Info.Name.empty() || Info.Targets == RelocationTargetKind::None)
      return false;
    for (size_t Other = Index + 1; Other < RelocationTable.size(); ++Other)
      if (Info.ID == RelocationTable[Other].ID ||
          Info.Value == RelocationTable[Other].Value)
        return false;

    switch (Info.FieldLayout) {
    case RelocationFieldLayout::SplitLDDWImmediate:
      if (Info.Width != kDoubleWordBitWidth ||
          Info.Purpose != RelocationPurpose::Absolute ||
          Info.Targets != TextLDDW ||
          Info.SymbolRequirement != RelocationSymbolRequirement::Required)
        return false;
      break;
    case RelocationFieldLayout::SplitLDDWOrData:
      if (Info.Width != kDoubleWordBitWidth ||
          Info.Purpose != RelocationPurpose::Relative ||
          !hasTarget(Info.Targets, TextLDDW) ||
          !hasTarget(Info.Targets, ReadOnlyData) ||
          Info.SymbolRequirement != RelocationSymbolRequirement::None)
        return false;
      break;
    case RelocationFieldLayout::CallImmediate:
      if (Info.Width != kWordBitWidth ||
          Info.Purpose != RelocationPurpose::Call || Info.Targets != TextCall ||
          Info.SymbolRequirement != RelocationSymbolRequirement::Required)
        return false;
      break;
    }
  }
  return true;
}

static_assert(validateRelocationTable(),
              "SBFRelocations.def contains inconsistent metadata");

} // namespace

llvm::ArrayRef<RelocationInfo> relocationInfos() { return RelocationTable; }

const RelocationInfo *getRelocationInfo(uint32_t Value) {
  for (const RelocationInfo &Info : RelocationTable)
    if (Info.Value == Value)
      return &Info;
  return nullptr;
}

bool isRelocationAllowedForVersion(const RelocationInfo &Info, Version V) {
  switch (Info.VersionPolicy) {
  case RelocationVersionPolicy::LegacyOnly:
    return !versionHasFeature(V, VersionFeature::StrictELF);
  }
  return false;
}

uint32_t legacyFunctionKey(size_t TargetSlot, llvm::StringRef Name) {
  if (Name == kEntrySymbolName)
    return hashSymbolName(Name);
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), TargetSlot);
  return hashSymbolName(llvm::StringRef(
      reinterpret_cast<const char *>(Bytes.data()), Bytes.size()));
}

} // namespace neverd::sbf
