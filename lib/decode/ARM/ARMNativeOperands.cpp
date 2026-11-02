//===- ARMNativeOperands.cpp - Typed ARM native operands -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Describe ARM and Thumb immediates as 32-bit operands, 64-bit for a 64-bit
/// vector element, and read the `#value` the ARM family's operand text,
/// AArch64's included, spells them as.
///
//===----------------------------------------------------------------------===//

#include "../NativeOperandsDetail.h"

#include "capstone/capstone.h"

#include <charconv>
#include <system_error>

namespace neverd::native_operands_detail {

void addARMImmediates(const DecodedInsn &I, NativeOperands &Result) {
  const auto &Detail = I.Raw->detail->arm;
  // A 64-bit integer element carries its immediate whole
  // (`vmov.i64 d0, #0xff00ff00ff00ff00`).
  const bool Wide = Detail.vector_data == ARM_VECTORDATA_I64 ||
                    Detail.vector_data == ARM_VECTORDATA_S64 ||
                    Detail.vector_data == ARM_VECTORDATA_U64;
  for (unsigned N = 0; N < Detail.op_count; ++N)
    if (Detail.operands[N].type == ARM_OP_IMM)
      addImmediate(Result, N, Detail.operands[N].imm, Wide ? 64 : 32);
}

std::optional<uint64_t> armImmediateText(std::string_view Operand) {
  // A decimal or `0x` hexadecimal number after `#`, negative with a minus.
  if (Operand.empty() || Operand.front() != '#')
    return std::nullopt;
  Operand.remove_prefix(1);
  const bool Negative = !Operand.empty() && Operand.front() == '-';
  if (Negative)
    Operand.remove_prefix(1);
  int Base = 10;
  if (Operand.size() > 2 && Operand[0] == '0' &&
      (Operand[1] == 'x' || Operand[1] == 'X')) {
    Base = 16;
    Operand.remove_prefix(2);
  }
  uint64_t Value = 0;
  const char *End = Operand.data() + Operand.size();
  const auto Parsed = std::from_chars(Operand.data(), End, Value, Base);
  if (Operand.empty() || Parsed.ec != std::errc{} || Parsed.ptr != End)
    return std::nullopt;
  return Negative ? uint64_t(0) - Value : Value;
}

} // namespace neverd::native_operands_detail
