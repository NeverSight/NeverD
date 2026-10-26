//===- LowScalarConstant.cpp - Bounded integer constant evaluation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shares width-correct scalar constant evaluation across LowIR analyses.

#include "neverd/ir/low/LowScalarConstant.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>

namespace neverd {

std::optional<uint64_t> lowScalarConstantResult(
    const LowOp &Op,
    llvm::function_ref<std::optional<uint64_t>(const NdVar &)> ValueOf) {
  if (!Op.Output.Size || Op.Output.Size > 8 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.NumInputs > std::size(Op.Inputs))
    return std::nullopt;
  const auto Mask = [](uint16_t Width) {
    return UINT64_MAX >> ((8 - Width) * 8);
  };
  std::optional<uint64_t> Result;
  if (Op.NumInputs == 1 &&
      (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
       Op.Opcode == NdOp::INT_NOT || Op.Opcode == NdOp::BOOL_NOT)) {
    const auto Width = Op.Inputs[0].Size;
    if (!Width || Width > Op.Output.Size ||
        (Op.Opcode == NdOp::BOOL_NOT && (Width != 1 || Op.Output.Size != 1)) ||
        ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_NOT) &&
         Width != Op.Output.Size))
      return std::nullopt;
    if (auto Value = ValueOf(Op.Inputs[0]))
      Result = Op.Opcode == NdOp::BOOL_NOT  ? !(*Value & Mask(Width))
               : Op.Opcode == NdOp::INT_NOT ? ~*Value
                                            : *Value & Mask(Width);
  } else if (Op.NumInputs == 2 && Op.Output.Size == 1 && Op.Inputs[0].Size &&
             Op.Inputs[0].Size <= 8 && Op.Inputs[0].Size == Op.Inputs[1].Size &&
             (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL ||
              Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_LESSEQUAL ||
              Op.Opcode == NdOp::INT_SLESS ||
              Op.Opcode == NdOp::INT_SLESSEQUAL)) {
    const auto Left = ValueOf(Op.Inputs[0]);
    const auto Right = ValueOf(Op.Inputs[1]);
    if (Left && Right) {
      const auto Width = Op.Inputs[0].Size;
      uint64_t L = *Left & Mask(Width), R = *Right & Mask(Width);
      if (Op.Opcode == NdOp::INT_SLESS || Op.Opcode == NdOp::INT_SLESSEQUAL) {
        // Bias the sign bit instead of converting an out-of-range unsigned
        // value to a host signed integer.
        const uint64_t Sign = uint64_t{1} << (Width * 8 - 1);
        L ^= Sign;
        R ^= Sign;
      }
      Result = Op.Opcode == NdOp::INT_EQUAL      ? L == R
               : Op.Opcode == NdOp::INT_NOTEQUAL ? L != R
               : Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_SLESS
                   ? L < R
                   : L <= R;
    }
  } else if (Op.NumInputs == 2 &&
             (Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
              Op.Opcode == NdOp::INT_ASHR) &&
             Op.Inputs[0].Size == Op.Output.Size &&
             Op.Inputs[1].Size == Op.Output.Size) {
    const auto Left = ValueOf(Op.Inputs[0]);
    const auto Right = ValueOf(Op.Inputs[1]);
    if (Left && Right) {
      const unsigned Bits = Op.Output.Size * 8;
      const llvm::APInt Value(Bits, *Left);
      const unsigned Count =
          unsigned(std::min<uint64_t>(*Right & Mask(Op.Output.Size), Bits));
      // ISA count masking belongs to the lifter. LowIR shifts saturate at
      // their declared width; never perform an out-of-range host shift.
      Result = (Op.Opcode == NdOp::INT_LEFT ? Value.shl(Count)
                : Op.Opcode == NdOp::INT_RIGHT
                    ? Value.lshr(Count)
                    : Value.ashr(std::min(Count, Bits - 1)))
                   .getZExtValue();
    }
  } else if (Op.NumInputs == 2 &&
             (Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB ||
              Op.Opcode == NdOp::INT_MULT || Op.Opcode == NdOp::INT_XOR ||
              Op.Opcode == NdOp::INT_AND || Op.Opcode == NdOp::INT_OR ||
              Op.Opcode == NdOp::BOOL_AND || Op.Opcode == NdOp::BOOL_OR ||
              Op.Opcode == NdOp::BOOL_XOR) &&
             Op.Inputs[0].Size == Op.Output.Size &&
             Op.Inputs[1].Size == Op.Output.Size) {
    if (Op.Opcode == NdOp::INT_XOR && Op.Inputs[0] == Op.Inputs[1] &&
        (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp()))
      Result = 0;
    else {
      const auto Left = ValueOf(Op.Inputs[0]);
      const auto Right = ValueOf(Op.Inputs[1]);
      const uint64_t WidthMask = Mask(Op.Output.Size);
      if (Op.Opcode == NdOp::INT_AND && ((Left && (*Left & WidthMask) == 0) ||
                                         (Right && (*Right & WidthMask) == 0)))
        Result = 0;
      else if (Op.Opcode == NdOp::INT_OR &&
               ((Left && (*Left & WidthMask) == WidthMask) ||
                (Right && (*Right & WidthMask) == WidthMask)))
        Result = WidthMask;
      else if (Left && Right) {
        if (Op.Opcode == NdOp::BOOL_AND || Op.Opcode == NdOp::BOOL_OR ||
            Op.Opcode == NdOp::BOOL_XOR) {
          if (Op.Output.Size != 1)
            return std::nullopt;
          const bool L = (*Left & UINT8_MAX) != 0;
          const bool R = (*Right & UINT8_MAX) != 0;
          Result = Op.Opcode == NdOp::BOOL_AND  ? L && R
                   : Op.Opcode == NdOp::BOOL_OR ? L || R
                                                : L != R;
        } else
          Result = Op.Opcode == NdOp::INT_ADD    ? *Left + *Right
                   : Op.Opcode == NdOp::INT_SUB  ? *Left - *Right
                   : Op.Opcode == NdOp::INT_MULT ? *Left * *Right
                   : Op.Opcode == NdOp::INT_AND  ? *Left & *Right
                   : Op.Opcode == NdOp::INT_OR   ? *Left | *Right
                                                 : *Left ^ *Right;
      }
    }
  }
  return Result ? std::optional<uint64_t>{*Result & Mask(Op.Output.Size)}
                : std::nullopt;
}

} // namespace neverd
