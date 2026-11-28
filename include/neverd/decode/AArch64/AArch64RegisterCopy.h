//===- AArch64RegisterCopy.h - Full-width register copies ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognize decoded AArch64 instructions that preserve a full GPR value.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DECODE_AARCH64_AARCH64REGISTERCOPY_H
#define NEVERD_DECODE_AARCH64_AARCH64REGISTERCOPY_H

#include "neverd/decode/Decoder.h"

#include <optional>
#include <utility>

namespace neverd::aarch64 {

/// The decoded destination and source preserve every bit, including LR.
inline std::optional<std::pair<aarch64_reg, aarch64_reg>>
fullWidthRegisterCopy(const DecodedInsn &I) {
  const auto &D = I.Raw->detail->aarch64;
  const bool Move =
      (I.Id == AARCH64_INS_MOV || I.Raw->alias_id == AARCH64_INS_ALIAS_MOV) &&
      D.op_count == 2;
  const bool AddZero =
      (I.Id == AARCH64_INS_ADD || I.Id == AARCH64_INS_SUB) && D.op_count == 3 &&
      D.operands[2].type == AARCH64_OP_IMM && D.operands[2].imm == 0;
  if ((!Move && !AddZero) || D.operands[0].type != AARCH64_OP_REG ||
      D.operands[1].type != AARCH64_OP_REG)
    return std::nullopt;
  const auto Wide = [](aarch64_reg R) {
    return (R >= AARCH64_REG_X0 && R <= AARCH64_REG_X28) ||
           R == AARCH64_REG_FP || R == AARCH64_REG_LR || R == AARCH64_REG_SP;
  };
  const auto Dest = D.operands[0].reg, Src = D.operands[1].reg;
  if (!Wide(Dest) || !Wide(Src))
    return std::nullopt;
  return std::pair(Dest, Src);
}

} // namespace neverd::aarch64

#endif // NEVERD_DECODE_AARCH64_AARCH64REGISTERCOPY_H
