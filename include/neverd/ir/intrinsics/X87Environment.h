//===- X87Environment.h - x87 memory image contracts ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the decoded x87 environment extent across IR representations.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X87ENVIRONMENT_H
#define NEVERD_IR_INTRINSICS_X87ENVIRONMENT_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>

namespace neverd {

constexpr bool isX87EnvironmentIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X87Fldenv || Id == Intrinsic::X87Fnstenv ||
         Id == Intrinsic::X87Frstor || Id == Intrinsic::X87Fnsave;
}

/// The image format is already resolved by the decoder. It must not be
/// reconstructed from the target address width or an isolated 66 prefix.
constexpr std::optional<bool> x87EnvironmentUses16BitImage(Intrinsic Id,
                                                           uint64_t Bytes) {
  if (!isX87EnvironmentIntrinsic(Id))
    return std::nullopt;
  const unsigned Registers =
      Id == Intrinsic::X87Frstor || Id == Intrinsic::X87Fnsave ? 80 : 0;
  if (Bytes == 14 + Registers)
    return true;
  if (Bytes == 28 + Registers)
    return false;
  return std::nullopt;
}

/// [id, effective address, constant image bytes] -> void. The extent is an
/// architectural property, so a missing or dynamic value cannot be guessed.
constexpr bool x87EnvironmentShapeIsValid(
    Intrinsic Id, unsigned NumInputs, unsigned OutputSize, unsigned AddressSize,
    unsigned ExtentSize, bool ExtentIsConstant, uint64_t Extent) {
  return NumInputs == 3 && OutputSize == 0 && AddressSize == 8 &&
         ExtentSize == 1 && ExtentIsConstant &&
         x87EnvironmentUses16BitImage(Id, Extent).has_value();
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X87ENVIRONMENT_H
