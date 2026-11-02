//===- LLVMAArch64ReturnProtection.h - Native signed frames ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Carry a proved entry-SP signing policy through LLVM and source projection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_AARCH64_LLVMAARCH64RETURNPROTECTION_H
#define NEVERD_BACKEND_LLVM_AARCH64_LLVMAARCH64RETURNPROTECTION_H

#include "neverd/ir/med/AArch64/AArch64ReturnProtection.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace neverd {

#define NEVERD_A64_RETURN_LLVM_TEXT(Name, Text)                                \
  inline constexpr char Name[] = Text;
#include "neverd/backend/llvm/AArch64/LLVMAArch64ReturnProtection.def"

inline void
setLLVMAArch64ReturnProtection(llvm::Function &Function,
                               const AArch64ReturnProtection &Protection) {
  Function.addFnAttr(kReturnSigningScopeAttribute, Protection.Leaf
                                                       ? kReturnSigningAll
                                                       : kReturnSigningNonLeaf);
  Function.addFnAttr(kReturnSigningKeyAttribute,
                     Protection.BKey ? kReturnSigningBKey : kReturnSigningAKey);
  std::string Features =
      Function.getFnAttribute(kReturnSigningFeaturesAttribute)
          .getValueAsString()
          .str();
  if (!Features.empty())
    Features += ',';
  Features += kReturnSigningFeature;
  Function.addFnAttr(kReturnSigningFeaturesAttribute, Features);
  Function.setUWTableKind(llvm::UWTableKind::Async);
}

inline std::optional<AArch64ReturnProtection>
getLLVMAArch64ReturnProtection(const llvm::Function &Function) {
  const bool Arm64e = Function.hasFnAttribute(kReturnSigningArm64eAttribute);
  const auto Scope = Function.getFnAttribute(kReturnSigningScopeAttribute);
  if (!Arm64e &&
      (!Scope.isValid() || Scope.getValueAsString() == kReturnSigningNone))
    return std::nullopt;
  AArch64ReturnProtection Protection;
  if (Arm64e) {
    Protection.BKey = true;
  } else {
    if (Scope.getValueAsString() != kReturnSigningAll &&
        Scope.getValueAsString() != kReturnSigningNonLeaf)
      throw std::invalid_argument(kReturnSigningMalformed);
    Protection.Leaf = Scope.getValueAsString() == kReturnSigningAll;
    const auto Key = Function.getFnAttribute(kReturnSigningKeyAttribute);
    if (Key.isValid()) {
      if (Key.getValueAsString() != kReturnSigningAKey &&
          Key.getValueAsString() != kReturnSigningBKey)
        throw std::invalid_argument(kReturnSigningMalformed);
      Protection.BKey = Key.getValueAsString() == kReturnSigningBKey;
    } else {
      Protection.BKey = Function.getParent() &&
                        Function.getParent()->getTargetTriple().isOSWindows();
    }
  }
  if (Function.hasFnAttribute(kReturnSigningPCAttribute))
    throw std::invalid_argument(kReturnSigningPCSourceUnsupported);
  return Protection;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_AARCH64_LLVMAARCH64RETURNPROTECTION_H
