//===- LowScalarConstant.cpp - Bounded integer constant evaluation -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shares width-correct scalar constant evaluation across LowIR analyses.

#include "neverd/ir/low/LowScalarConstant.h"

#include "neverd/ir/Limits.h"

#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <utility>

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
       Op.Opcode == NdOp::INT_SEXT || Op.Opcode == NdOp::INT_NOT ||
       Op.Opcode == NdOp::INT_NEGATE || Op.Opcode == NdOp::INT_NEG2 ||
       Op.Opcode == NdOp::BOOL_NOT)) {
    const auto Width = Op.Inputs[0].Size;
    const bool Extension =
        Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT;
    if (!Width || Width > Op.Output.Size ||
        (Op.Opcode == NdOp::BOOL_NOT && (Width != 1 || Op.Output.Size != 1)) ||
        (!Extension && Width != Op.Output.Size))
      return std::nullopt;
    if (auto Value = ValueOf(Op.Inputs[0])) {
      const uint64_t Bits = *Value & Mask(Width);
      if (Op.Opcode == NdOp::INT_SEXT)
        Result = llvm::APInt(Width * 8, Bits)
                     .sextOrTrunc(Op.Output.Size * 8)
                     .getZExtValue();
      else if (Op.Opcode == NdOp::INT_NOT || Op.Opcode == NdOp::INT_NEGATE)
        Result = ~Bits;
      else if (Op.Opcode == NdOp::INT_NEG2)
        Result = uint64_t{0} - Bits;
      else
        Result = Op.Opcode == NdOp::BOOL_NOT ? !Bits : Bits;
    }
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

std::map<SourceCallOccurrenceKey, va_t>
constantRegisterCallTargets(const LowFunc &Function) {
  std::map<SourceCallOccurrenceKey, va_t> Targets;
  size_t Work = 0;
  for (const auto &Block : Function.Blocks) {
    if (Block.Ops.size() > limits::ir::kConstantRegisterCallOperations - Work)
      return {};
    Work += Block.Ops.size();
    if (std::none_of(Block.Ops.begin(), Block.Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::INDIR_CALL && Op.NumInputs == 1 &&
                 !Op.Inputs[0].isConst();
        }))
      continue;
    std::map<uint64_t, uint8_t> Registers;
    std::map<uint64_t, std::pair<uint16_t, uint64_t>> Temps;
    va_t Instruction = InvalidVA;
    auto ValueOf = [&](const NdVar &Value) -> std::optional<uint64_t> {
      if (!Value.Size || Value.Size > sizeof(uint64_t))
        return {};
      if (Value.isConst())
        return Value.Offset & (UINT64_MAX >> ((8 - Value.Size) * 8));
      if (Value.isTemp()) {
        const auto It = Temps.find(Value.Offset);
        if (It != Temps.end() && It->second.first == Value.Size)
          return It->second.second;
        return {};
      }
      if (!Value.isReg() || Value.Offset > UINT64_MAX - Value.Size)
        return {};
      uint64_t Bits = 0;
      for (unsigned I = 0; I < Value.Size; ++I) {
        const auto It = Registers.find(Value.Offset + I);
        if (It == Registers.end())
          return {};
        Bits |= uint64_t{It->second} << (I * 8);
      }
      return Bits;
    };
    for (const auto &Op : Block.Ops) {
      if (Op.Addr != Instruction) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (const auto Site = sourceCallOccurrenceKey(Op);
          Site && Site->Opcode == NdOp::INDIR_CALL && !Site->StaticTarget)
        if (const auto Target = ValueOf(Op.Inputs[0]))
          Targets.emplace(*Site, *Target);
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC) {
        Registers.clear();
        Temps.clear();
        continue;
      }
      const auto &Output = Op.Output;
      if (!Output.Size || (!Output.isReg() && !Output.isTemp()))
        continue;
      const auto Result =
          Op.MemoryOrdering == NdMemoryOrdering::None &&
                  Op.MemoryAddressSpace == NdMemoryAddressSpace::Default
              ? lowScalarConstantResult(Op, ValueOf)
              : std::nullopt;
      if (Output.isTemp()) {
        if (Result)
          Temps[Output.Offset] = {Output.Size, *Result};
        else
          Temps.erase(Output.Offset);
      } else if (Output.Offset <= UINT64_MAX - Output.Size) {
        // Erase overlapping byte facts even for a wide unknown result. Never
        // interpret an architectural subregister write as a whole GPR write.
        Registers.erase(Registers.lower_bound(Output.Offset),
                        Registers.lower_bound(Output.Offset + Output.Size));
        if (Result)
          for (unsigned I = 0; I < Output.Size; ++I)
            Registers.emplace(Output.Offset + I, uint8_t(*Result >> (I * 8)));
      } else
        Registers.clear();
    }
  }
  return Targets;
}

} // namespace neverd
