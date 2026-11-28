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
  if (D.op_count < 2 || D.operands[0].type != AARCH64_OP_REG ||
      D.operands[1].type != AARCH64_OP_REG)
    return std::nullopt;
  const auto Wide = [](aarch64_reg R) {
    return (R >= AARCH64_REG_X0 && R <= AARCH64_REG_X28) ||
           R == AARCH64_REG_FP || R == AARCH64_REG_LR || R == AARCH64_REG_SP;
  };
  const auto Plain = [](const cs_aarch64_op &Op) {
    return Op.ext == AARCH64_EXT_INVALID && !Op.shift.value;
  };
  const auto Dest = D.operands[0].reg;
  if (!Wide(Dest))
    return std::nullopt;
  using Copy = std::optional<std::pair<aarch64_reg, aarch64_reg>>;
  const auto From = [&](aarch64_reg Src) -> Copy {
    return Wide(Src) ? Copy(std::pair(Dest, Src)) : std::nullopt;
  };
  const auto &Left = D.operands[1];
  if (D.op_count == 2) {
    // Capstone attaches immediate and register shifts to the source operand.
    switch (I.Raw->alias_id) {
    case AARCH64_INS_ALIAS_ASR:
    case AARCH64_INS_ALIAS_LSL:
    case AARCH64_INS_ALIAS_LSR:
    case AARCH64_INS_ALIAS_ROR:
    case AARCH64_INS_ALIAS_MOV:
      return Plain(Left) ? From(Left.reg) : std::nullopt;
    }
    switch (I.Id) {
    case AARCH64_INS_MOV:
      return Plain(Left) ? From(Left.reg) : std::nullopt;
    case AARCH64_INS_ASR:
    case AARCH64_INS_LSL:
    case AARCH64_INS_LSR:
    case AARCH64_INS_ROR:
      return Left.shift.type >= AARCH64_SFT_LSL_REG &&
                     Left.shift.value == AARCH64_REG_XZR
                 ? From(Left.reg)
                 : std::nullopt;
    default:
      return std::nullopt;
    }
  }
  const auto &Right = D.operands[2];
  if (D.op_count == 4 && I.Id == AARCH64_INS_EXTR &&
      Right.type == AARCH64_OP_REG && D.operands[3].type == AARCH64_OP_IMM &&
      D.operands[3].imm == 0)
    return From(Right.reg);
  if (D.op_count != 3)
    return std::nullopt;
  const bool AddSub = I.Id == AARCH64_INS_ADD || I.Id == AARCH64_INS_SUB;
  const bool OrXor = I.Id == AARCH64_INS_ORR || I.Id == AARCH64_INS_EOR;
  if (Right.type == AARCH64_OP_IMM && AddSub && Right.imm == 0)
    return From(Left.reg);
  if (Right.type != AARCH64_OP_REG)
    return std::nullopt;
  // Shifting or extending the zero operand leaves it zero.
  if ((Right.reg == AARCH64_REG_XZR || Right.reg == AARCH64_REG_WZR) &&
      (AddSub || OrXor))
    return From(Left.reg);
  if (!Plain(Right))
    return std::nullopt;
  if (Left.reg == AARCH64_REG_XZR && (I.Id == AARCH64_INS_ADD || OrXor))
    return From(Right.reg);
  if (Left.reg == Right.reg &&
      (I.Id == AARCH64_INS_AND || I.Id == AARCH64_INS_ORR ||
       I.Id == AARCH64_INS_CSEL))
    return From(Left.reg);
  return std::nullopt;
}

} // namespace neverd::aarch64

#endif // NEVERD_DECODE_AARCH64_AARCH64REGISTERCOPY_H
