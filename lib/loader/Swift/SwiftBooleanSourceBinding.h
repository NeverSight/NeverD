#ifndef NEVERD_LOADER_SWIFT_SWIFTBOOLEANSOURCEBINDING_H
#define NEVERD_LOADER_SWIFT_SWIFTBOOLEANSOURCEBINDING_H

#include "SwiftBooleanRuntimeCandidate.h"

namespace neverd {
inline constexpr llvm::StringLiteral SwiftBooleanSourceName =
    "neverd_swift_string_compare_bool";

inline std::string swiftBooleanSourceName(llvm::StringRef TargetName) {
  if (TargetName == SwiftBooleanComparisonImport.drop_front())
    return SwiftBooleanSourceName.str();
  if (TargetName == SwiftBooleanPrefixImport.drop_front())
    return "neverd_swift_string_has_prefix_bool";
  if (TargetName == SwiftBooleanSuffixImport.drop_front())
    return "neverd_swift_string_has_suffix_bool";
  if (TargetName == SwiftBooleanObjectEqualityImport.drop_front())
    return "neverd_swift_nsobject_equal_bool";
  return {};
}

/// This is the normalized expression's ABI-shaped carrier description. The
/// emitter must declare the linked routine as true swiftcc _Bool, then convert
/// its result to uint8_t. It must never use this signature as that prototype.
inline std::optional<SourceFunctionTypeHint> swiftBooleanNormalizedSignature(
    llvm::StringRef Import = SwiftBooleanComparisonImport) {
  auto Signature = swiftBooleanRuntimeInputs(Import);
  if (!Signature)
    return std::nullopt;
  Signature->Origin = SourceFunctionTypeHint::OriginKind::SwiftRuntime;
  Signature->ReturnType = NdType::makeInt(1, false);
  std::string Error;
  if (!assignDarwinSwiftSourceABI(*Signature, Arch::AArch64, Error))
    return std::nullopt;
  return Signature;
}

inline bool isSwiftBooleanSourceBinding(const SourceCallTypeHint &Binding) {
  const auto Expected =
      swiftBooleanNormalizedSignature("_" + Binding.TargetName);
  return Expected &&
         Binding.CallKind == SourceCallTypeHint::Kind::SwiftBooleanProjection &&
         Binding.BooleanResult && Binding.BooleanResult->FunctionEntry &&
         Binding.BooleanResult->Site.Instruction &&
         Binding.BooleanResult->FunctionEntry % 4 == 0 &&
         Binding.BooleanResult->Site.Instruction % 4 == 0 &&
         Binding.BooleanResult->Site.Sequence >= 0 &&
         Binding.BooleanResult->Site.Opcode == NdOp::CALL &&
         Binding.BooleanResult->Site.StaticTarget &&
         *Binding.BooleanResult->Site.StaticTarget % 4 == 0 &&
         Binding.TargetAddress && Binding.TargetAddress % 8 == 0 &&
         !swiftBooleanSourceName(Binding.TargetName).empty() &&
         equalSourceABIs(Binding.Signature, *Expected) &&
         !Binding.ValueWitness && !Binding.DoesNotReturn &&
         !Binding.WeakImport && !Binding.ReturnedArgument &&
         !Binding.RuntimeObjCResultType && Binding.Selector.empty() &&
         Binding.OwnerClass.empty() && !Binding.SelectorReferenceAddress &&
         Binding.BorrowedByteInputs.empty() &&
         Binding.SwiftStringInputs.empty() && !Binding.Format &&
         !Binding.NilTerminated && !Binding.SwiftTypeMetadata &&
         !Binding.Receiver && !Binding.SelectorResultUse &&
         !Binding.SelectorResultTypeUse && !Binding.SelectorArgumentTypeUse &&
         !Binding.SelectorForwardingUse &&
         !Binding.SelectorArgumentStorageUse &&
         !Binding.ObjCIndirectResultStorage && !Binding.ByteCount &&
         !Binding.ImmutablePointerSlot;
}
} // namespace neverd
#endif
