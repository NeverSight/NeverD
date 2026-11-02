//===- NativeOperands.cpp - Typed decoded native operands ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Typed decoded native operands.
//===----------------------------------------------------------------------===//

#include "neverd/decode/NativeOperands.h"

#include "NativeOperandsDetail.h"

#include "capstone/capstone.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace neverd {

namespace native_operands_detail {

void addImmediate(NativeOperands &Result, unsigned Index, int64_t Value,
                  unsigned Bits) {
  if (!Bits || Bits > 64)
    return;
  const uint64_t Mask = Bits == 64 ? UINT64_MAX : (uint64_t(1) << Bits) - 1;
  Result.Immediates.push_back(
      {Index, static_cast<uint64_t>(Value) & Mask, Bits});
}

} // namespace native_operands_detail

NativeOperands describeNativeOperands(const Decoder &D, const DecodedInsn &I,
                                      Arch A, bool IncludeRegisters) {
  NativeOperands Result;
  if (I.AArch64Occurrence && A != Arch::AArch64)
    return Result;
  if (A == Arch::AArch64 && !D.hasAArch64OperandDetail(I))
    return Result;
  if (!I.Raw || !I.Raw->detail)
    return Result;
  switch (A) {
  case Arch::X86:
  case Arch::X64:
    native_operands_detail::addX86Immediates(I, Result);
    break;
  case Arch::AArch64:
    native_operands_detail::addAArch64Immediates(D, I, Result);
    break;
  case Arch::ARM:
    native_operands_detail::addARMImmediates(I, Result);
    break;
  default:
    break;
  }
  cs_regs Reads{}, Writes{};
  uint8_t ReadCount = 0, WriteCount = 0;
  std::map<std::string, NativeRegister> Registers;
  if (IncludeRegisters &&
      cs_regs_access(D.getHandle(), I.Raw, Reads, &ReadCount, Writes,
                     &WriteCount) == CS_ERR_OK) {
    for (unsigned N = 0; N < ReadCount + WriteCount; ++N) {
      const auto *Name = cs_reg_name(
          D.getHandle(), N < ReadCount ? Reads[N] : Writes[N - ReadCount]);
      if (!Name || !*Name)
        continue;
      auto &R = Registers[Name];
      R.Name = Name;
      if (N < ReadCount)
        R.Read = true;
      else
        R.Write = true;
    }
  }
  for (auto &[Name, R] : Registers)
    Result.Registers.push_back(std::move(R));
  return Result;
}

std::optional<NativeImmediate> nativeImmediateAt(const NativeOperands &Operands,
                                                 const DecodedInsn &I, Arch A,
                                                 unsigned Index) {
  switch (A) {
  case Arch::X86:
  case Arch::X64:
    for (const NativeImmediate &Immediate : Operands.Immediates)
      if (Immediate.Operand == Index)
        return Immediate;
    return std::nullopt;
  case Arch::AArch64:
  case Arch::ARM:
    break;
  default:
    return std::nullopt;
  }
  if (!I.Raw)
    return std::nullopt;
  const std::string_view Text = I.Raw->op_str;
  std::optional<std::string_view> Operand;
  unsigned Depth = 0, Counted = 0;
  size_t Start = 0;
  for (size_t N = 0; N <= Text.size() && !Operand; ++N) {
    const char C = N < Text.size() ? Text[N] : ',';
    if (C == '[' || C == '{' || C == '(')
      ++Depth;
    else if ((C == ']' || C == '}' || C == ')') && Depth)
      --Depth;
    else if (C == ',' && !Depth) {
      if (Counted++ == Index)
        Operand = Text.substr(Start, N - Start);
      Start = N + 1;
    }
  }
  if (!Operand)
    return std::nullopt;
  while (!Operand->empty() && Operand->front() == ' ')
    Operand->remove_prefix(1);
  while (!Operand->empty() && Operand->back() == ' ')
    Operand->remove_suffix(1);
  const auto Value = native_operands_detail::armImmediateText(*Operand);
  if (!Value)
    return std::nullopt;
  // The operand spells the decoder's value itself or its sign extension; a
  // value wider than the immediate is another number.
  for (const NativeImmediate &Immediate : Operands.Immediates) {
    uint64_t Extended = Immediate.Value;
    if (Immediate.Bits < 64 && (Immediate.Value >> (Immediate.Bits - 1)) & 1)
      Extended |= UINT64_MAX << Immediate.Bits;
    if (*Value == Immediate.Value || *Value == Extended)
      return Immediate;
  }
  return std::nullopt;
}
} // namespace neverd
