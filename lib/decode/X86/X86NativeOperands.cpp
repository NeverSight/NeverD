//===- X86NativeOperands.cpp - Typed x86 native operands -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describe x86 and x86-64 immediates using their logical operand widths.
///
//===----------------------------------------------------------------------===//

#include "../NativeOperandsDetail.h"

#include "capstone/capstone.h"

namespace neverd::native_operands_detail {

void addX86Immediates(const DecodedInsn &I, NativeOperands &Result) {
  const auto &Detail = I.Raw->detail->x86;
  for (unsigned N = 0; N < Detail.op_count; ++N)
    if (Detail.operands[N].type == X86_OP_IMM)
      addImmediate(Result, N, Detail.operands[N].imm,
                   Detail.operands[N].size * 8);
}

} // namespace neverd::native_operands_detail
