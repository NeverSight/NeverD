//===- X87Value.h - Scalar x87 value and status contracts -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share exact operand counts and raw binary80 shapes across IR consumers.
/// Missing operands cannot be replaced with another operand or narrowed.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X87VALUE_H
#define NEVERD_IR_X86_X87VALUE_H

#include "neverd/ir/low/LowIR.h"

#include <optional>

namespace neverd {

struct X87ValueInfo {
  unsigned Operands = 0;
  bool PopsSecond = false;
  bool SamplesStatus = false;
};

constexpr std::optional<X87ValueInfo> x87ValueInfo(Intrinsic Id) {
  switch (Id) {
#define NEVERD_X87_VALUE_HELPER(ID, Name, Asm, Operands, PopsST1, Status)      \
  case Intrinsic::ID:                                                          \
    return X87ValueInfo{Operands, PopsST1, Status};
#include "neverd/ir/intrinsics/X86/X87ValueInstructions.def"
  case Intrinsic::X87Fprem:
  case Intrinsic::X87Fprem1:
    return X87ValueInfo{2, false, true};
  case Intrinsic::X87ReadStatus:
    return X87ValueInfo{};
  default:
    return std::nullopt;
  }
}

constexpr bool isLegacyX87PushValue(Intrinsic Id) {
  return Id == Intrinsic::X87Fptan || Id == Intrinsic::X87Fxtractsig ||
         Id == Intrinsic::X87Fxtractexp;
}

struct X87ValueShape {
  Arch TargetArch = Arch::Unknown;
  Intrinsic Kind = Intrinsic::None;
  NdMemoryOrdering Ordering = NdMemoryOrdering::None;
  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default;
  unsigned NumInputs = 0, OutputSize = 0;
  unsigned OperandSize[2] = {};
  bool IdMatches = false, WritableOutput = false;
  bool ScalarOperands = false, HasAuxiliaryOutputs = false;
};

constexpr bool x87ValueShapeIsValid(const X87ValueShape &S) {
  const auto Info = x87ValueInfo(S.Kind);
  // Legacy single-result push helpers cannot express the conditional push
  // or authenticated CW. Keep their IDs reserved and require the pair form.
  if (!Info || isLegacyX87PushValue(S.Kind) || !S.IdMatches ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X86 &&
       S.TargetArch != Arch::X64) ||
      S.Ordering != NdMemoryOrdering::None ||
      S.Space != NdMemoryAddressSpace::Default || S.HasAuxiliaryOutputs ||
      S.NumInputs != Info->Operands + 1 || !S.WritableOutput ||
      !S.ScalarOperands ||
      S.OutputSize != (S.Kind == Intrinsic::X87ReadStatus ? 2u : 10u))
    return false;
  for (unsigned I = 0; I != Info->Operands; ++I)
    if (S.OperandSize[I] != 10)
      return false;
  return true;
}

inline X87ValueShape x87ValueLowShape(const LowOp &Op, Arch A) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs < 1 || Op.NumInputs > 3 ||
      !Op.Inputs[0].isConst())
    return {};
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  X87ValueShape S{.TargetArch = A,
                  .Kind = Id,
                  .Ordering = Op.MemoryOrdering,
                  .Space = Op.MemoryAddressSpace,
                  .NumInputs = Op.NumInputs,
                  .OutputSize = Op.Output.Size,
                  .IdMatches = Op.Inputs[0].Size == 2 &&
                               Op.Inputs[0].Offset == unsigned(Id),
                  .WritableOutput = Op.Output.isReg() || Op.Output.isTemp(),
                  .ScalarOperands = true};
  for (unsigned I = 1; I != Op.NumInputs; ++I) {
    S.OperandSize[I - 1] = Op.Inputs[I].Size;
    S.ScalarOperands &=
        Op.Inputs[I].isConst() || Op.Inputs[I].isReg() || Op.Inputs[I].isTemp();
  }
  return S;
}

} // namespace neverd

#endif // NEVERD_IR_X86_X87VALUE_H
