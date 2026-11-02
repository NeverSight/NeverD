//===- AArch64NativeOperands.cpp - AArch64 native operands ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describe AArch64 immediates using the first register operand's width.
///
//===----------------------------------------------------------------------===//

#include "../NativeOperandsDetail.h"

#include "capstone/capstone.h"

namespace neverd::native_operands_detail {

void addAArch64Immediates(const Decoder &D, const DecodedInsn &I,
                          NativeOperands &Result) {
  const auto &Detail = I.Raw->detail->aarch64;
  unsigned Bits = 64;
  for (unsigned N = 0; N < Detail.op_count; ++N)
    if (Detail.operands[N].type == AARCH64_OP_REG) {
      const auto *Name = cs_reg_name(D.getHandle(), Detail.operands[N].reg);
      if (Name && Name[0] == 'w')
        Bits = 32;
      break;
    }
  for (unsigned N = 0; N < Detail.op_count; ++N)
    if (Detail.operands[N].type == AARCH64_OP_IMM)
      addImmediate(Result, N, Detail.operands[N].imm, Bits);
}

} // namespace neverd::native_operands_detail
