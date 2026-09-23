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
  std::string ImportName;
};

inline constexpr llvm::StringLiteral SwiftBooleanComparisonImport =
    "_$ss27_stringCompareWithSmolCheck__9expectingSbs11_StringGutsV_"
    "ADs01_G16ComparisonResultOtF";
inline constexpr llvm::StringLiteral SwiftBooleanComparisonProvider =
    "/usr/lib/swift/libswiftCore.dylib";
inline constexpr llvm::StringLiteral SwiftBooleanPrefixImport =
    "_$sSS9hasPrefixySbSSF";
inline constexpr llvm::StringLiteral SwiftBooleanSuffixImport =
    "_$sSS9hasSuffixySbSSF";
inline constexpr llvm::StringLiteral SwiftBooleanObjectEqualityImport =
    "_$sSo8NSObjectC10ObjectiveCE2eeoiySbAB_ABtFZ";
inline constexpr llvm::StringLiteral SwiftBooleanObjectEqualityProvider =
    "/usr/lib/swift/libswiftObjectiveC.dylib";

inline llvm::StringRef swiftBooleanRuntimeProvider(llvm::StringRef Import) {
  if (Import == SwiftBooleanComparisonImport ||
      Import == SwiftBooleanPrefixImport || Import == SwiftBooleanSuffixImport)
    return SwiftBooleanComparisonProvider;
  if (Import == SwiftBooleanObjectEqualityImport)
    return SwiftBooleanObjectEqualityProvider;
  return {};
}

/// Canonical physical inputs shared by raw runtime and logical source facts.
inline std::optional<SourceFunctionTypeHint> swiftBooleanRuntimeInputs(
    llvm::StringRef Import = SwiftBooleanComparisonImport) {
  if (swiftBooleanRuntimeProvider(Import).empty())
    return std::nullopt;
  SourceFunctionTypeHint Inputs;
  Inputs.ReturnType = NdType::makeVoid();
  const auto Word = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(NdType::makeVoid());
  if (Import == SwiftBooleanObjectEqualityImport) {
    Inputs.Parameters = {
        {"lhs", Pointer}, {"rhs", Pointer}, {"metadata", Pointer}};
    Inputs.Parameters.back().TheRole =
        SourceParameterTypeHint::Role::SwiftContext;
  } else
    Inputs.Parameters = {
        {"lhs0", Word}, {"lhs1", Pointer}, {"rhs0", Word}, {"rhs1", Pointer}};
  if (Import == SwiftBooleanComparisonImport)
    Inputs.Parameters.push_back({"expecting", NdType::makeInt(1, false)});
  std::string Error;
  if (!assignDarwinSwiftSourceABI(Inputs, Arch::AArch64, Error))
    return std::nullopt;
  return Inputs;
}

inline std::optional<SourceFunctionTypeHint> swiftBooleanComparisonInputs() {
  return swiftBooleanRuntimeInputs();
}

/// Compiler evidence: Actions 35653573282, consumer db8d4079f67bd90beb8bbf50ff7
/// 26ac0c488f6d5, Xcode 26.5/17F42. Both ARM64 device and simulator Swift/C
/// probes produce swiftcc i1(i64, ptr, i64, ptr, i8). The C probe has a genuine
/// _Bool declaration, zext i1 to i8, and an AND W0,#1 assembly normalization.
/// Device TBD exports arm64e-ios; simulator exports include
/// arm64-ios-simulator. This candidate is deliberately absent from the ordinary
/// byte-return table.
/// Prefix evidence: Actions 35683213827, consumer bfe0f17b3d7140d468f53a6f7a0
/// 7375d3b7d898a, Xcode 26.5/17F42. Both SDKs independently produce
/// swiftcc i1(i64, ptr, i64, ptr), prefix words before receiver words, with the
/// same genuine _Bool normalization. Their libswiftCore TBDs export the exact
/// hasPrefix symbol for arm64e-ios and arm64-ios-simulator respectively.
/// Suffix evidence: Apple Swift 6.1.2 arm64-macosx15 client IR declares the
/// exact libswiftCore symbol as swiftcc i1(i64, ptr, i64, ptr), passing the
/// suffix words before the receiver words. The call-site LowIR proof below
/// still has to establish the one-bit result normalization before publication.
/// NSObject equality: Actions 35710714248, consumer 4c088964d2aa7a0f1d19ca3
/// f6929551154bc84c4, Xcode 26.5/17F42. Device and simulator Swift/C probes
/// independently produce swiftcc i1(ptr, ptr, ptr swiftself); the metadata is
/// an x20 input, not a third ordinary argument. Both libswiftObjectiveC TBDs
/// export the exact symbol for their ARM64 target. The C result is normalized
/// with zext i1 to i8, retaining the raw one-bit contract here.
inline std::optional<SwiftBooleanRuntimeCandidate>
swiftBooleanRuntimeCandidate(const BinaryImage &Image, va_t ImportSlot) {
  if (Image.Arch != Arch::AArch64 || Image.MachOChainedFixupsAmbiguous)
    return std::nullopt;
  const auto Import = darwinRuntimeImport(Image, ImportSlot);
  const auto Provider =
      Import ? swiftBooleanRuntimeProvider(*Import) : llvm::StringRef{};
  const auto Bind = Image.DyldBindSlots.find(ImportSlot);
  if (!Import || Provider.empty() || Bind == Image.DyldBindSlots.end() ||
      Bind->second.Module != Provider ||
      !Image.isValidImportStorageSlot(ImportSlot, *Import) ||
      std::count(Image.DynInfo.NeededLibs.begin(),
                 Image.DynInfo.NeededLibs.end(), Provider.str()) != 1)
    return std::nullopt;
  const auto Storage = Image.collectImportStorageSlots();
  const auto Slot = Storage.Slots.find(ImportSlot);
  if (Storage.Conflicts.count(ImportSlot) || Slot == Storage.Slots.end() ||
      Slot->second.Name != *Import || Slot->second.Addend)
    return std::nullopt;

  auto Inputs = swiftBooleanRuntimeInputs(*Import);
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
  return SwiftBooleanRuntimeCandidate{&Image, ImportSlot, std::move(Raw),
                                      Import->str()};
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
