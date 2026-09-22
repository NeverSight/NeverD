#ifndef NEVERD_LOADER_SWIFT_SWIFTBOOLEANRUNTIMECANDIDATE_H
#define NEVERD_LOADER_SWIFT_SWIFTBOOLEANRUNTIMECANDIDATE_H

#include "../../ir/low/SourceBooleanResultContract.h"
#include "../MachO/DarwinRuntimeImport.h"

#include "neverd/loader/MachO/DarwinImportVeneer.h"

#include <algorithm>

namespace neverd {

/// Non-publishing runtime fact. A current direct-call/stub binding and complete
/// Low consumer proof are still required before any source call can use it.
struct SwiftBooleanRuntimeCandidate {
  const BinaryImage *SourceImage = nullptr;
  va_t ImportSlot = 0;
  SourceBooleanResultContract RawContract;
};

inline constexpr llvm::StringLiteral SwiftBooleanComparisonImport =
    "_$ss27_stringCompareWithSmolCheck__9expectingSbs11_StringGutsV_"
    "ADs01_G16ComparisonResultOtF";
inline constexpr llvm::StringLiteral SwiftBooleanComparisonProvider =
    "/usr/lib/swift/libswiftCore.dylib";

/// Canonical physical inputs shared by raw runtime and logical source facts.
inline std::optional<SourceFunctionTypeHint> swiftBooleanComparisonInputs() {
  SourceFunctionTypeHint Inputs;
  Inputs.ReturnType = NdType::makeVoid();
  const auto Word = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  Inputs.Parameters = {{"lhs0", Word},
                       {"lhs1", Pointer},
                       {"rhs0", Word},
                       {"rhs1", Pointer},
                       {"expecting", NdType::makeInt(1, false)}};
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Inputs, Arch::AArch64, Error))
    return std::nullopt;
  return Inputs;
}

/// Compiler evidence: Actions 35653573282, consumer db8d4079f67bd90beb8bbf50ff7
/// 26ac0c488f6d5, Xcode 26.5/17F42. Both ARM64 device and simulator Swift/C
/// probes produce swiftcc i1(i64, ptr, i64, ptr, i8). The C probe has a genuine
/// _Bool declaration, zext i1 to i8, and an AND W0,#1 assembly normalization.
/// Device TBD exports arm64e-ios; simulator exports include
/// arm64-ios-simulator. This candidate is deliberately absent from the ordinary
/// byte-return table.
inline std::optional<SwiftBooleanRuntimeCandidate>
swiftBooleanRuntimeCandidate(const BinaryImage &Image, va_t ImportSlot) {
  if (Image.Arch != Arch::AArch64 || Image.MachOChainedFixupsAmbiguous)
    return std::nullopt;
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Import || *Import != SwiftBooleanComparisonImport ||
      Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != SwiftBooleanComparisonProvider ||
      !Image.isValidImportStorageSlot(ImportSlot, *Import) ||
      std::count(Image.DynInfo.NeededLibs.begin(),
                 Image.DynInfo.NeededLibs.end(),
                 SwiftBooleanComparisonProvider.str()) != 1)
    return std::nullopt;
  const auto Storage = Image.collectImportStorageSlots();
  const auto Slot = Storage.Slots.find(ImportSlot);
  if (Storage.Conflicts.count(ImportSlot) || Slot == Storage.Slots.end() ||
      Slot->second.Name != *Import || Slot->second.Addend)
    return std::nullopt;

  auto Inputs = swiftBooleanComparisonInputs();
  if (!Inputs)
    return std::nullopt;
  SourceBooleanResultContract Raw;
  Raw.Architecture = Arch::AArch64;
  Raw.Parameters = std::move(Inputs->Parameters);
  Raw.ResultRegister = getTargetRegInfo(Arch::AArch64).IntReturnReg;
  Raw.ResultCarrierBytes = 8;
  Raw.DefinedResultBits = 1;
  if (!sourceBooleanInputParameters(Raw))
    return std::nullopt;
  return SwiftBooleanRuntimeCandidate{&Image, ImportSlot, std::move(Raw)};
}

/// The same non-publishing fact reached through one exact ordinary veneer.
/// No symbol at the code address or pre-existing call hint supplies authority.
inline std::optional<SwiftBooleanRuntimeCandidate>
swiftBooleanRuntimeVeneerCandidate(const BinaryImage &Image, va_t Address) {
  const auto Slot = darwinImportVeneerSlot(Image, Address);
  return Slot ? swiftBooleanRuntimeCandidate(Image, *Slot) : std::nullopt;
}
} // namespace neverd
#endif
