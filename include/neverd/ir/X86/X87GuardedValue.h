//===- X87GuardedValue.h - Guarded native x87 value contracts --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Share raw operand/result shapes and admission for typed native completion.
/// Stack/tag proofs remain with the existing CFG x87 state owner.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X87GUARDEDVALUE_H
#define NEVERD_IR_X86_X87GUARDEDVALUE_H

#include "neverd/ir/X86/X87BCD.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {

constexpr bool isX87PushPairIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X87FptanPair || Id == Intrinsic::X87FsincosPair ||
         Id == Intrinsic::X87FxtractPair;
}

constexpr bool isX87GuardedValueIntrinsic(Intrinsic Id) {
  return isX87BCDIntrinsic(Id) || isX87PushPairIntrinsic(Id);
}

constexpr unsigned x87GuardedValueBytes(Intrinsic Id) {
  return isX87PushPairIntrinsic(Id) ? 20 : isX87BCDIntrinsic(Id) ? 10 : 0;
}

/// FPTAN/FSINCOS only push for |x| < 2^63. The typed projection admits that
/// finite range; the owned context handles conditional TOP changes.
inline constexpr unsigned X87TrigRangeExponent = 0x403e;

constexpr const char *x87PushPairMnemonic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X87FptanPair:
    return "fptan";
  case Intrinsic::X87FsincosPair:
    return "fsincos";
  case Intrinsic::X87FxtractPair:
    return "fxtract";
  default:
    return nullptr;
  }
}

/// Native guarded values carry raw bits and the actual control word.
/// Stack/tag admission belongs to the CFG owner, not this value contract.
struct X87GuardedValueShape {
  Arch TargetArch = Arch::Unknown;
  Intrinsic Kind = Intrinsic::None;
  NdMemoryOrdering Ordering = NdMemoryOrdering::None;
  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default;
  unsigned NumInputs = 0, OutputSize = 0, ValueSize = 0, ControlSize = 0;
  bool IdMatches = false, WritableOutput = false;
  bool ValueIsScalar = false, ControlIsScalar = false;
  bool HasAuxiliaryOutputs = false;
};

constexpr bool x87GuardedValueShapeIsValid(const X87GuardedValueShape &S) {
  return isX87GuardedValueIntrinsic(S.Kind) && S.IdMatches &&
         (S.TargetArch == Arch::Unknown || S.TargetArch == Arch::X86 ||
          S.TargetArch == Arch::X64) &&
         S.Ordering == NdMemoryOrdering::None &&
         S.Space == NdMemoryAddressSpace::Default && !S.HasAuxiliaryOutputs &&
         S.NumInputs == 3 && S.OutputSize == x87GuardedValueBytes(S.Kind) &&
         S.WritableOutput && S.ValueIsScalar && S.ValueSize == 10 &&
         S.ControlIsScalar && S.ControlSize == 2;
}

inline X87GuardedValueShape x87GuardedValueLowShape(const LowOp &Op, Arch A) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != 3 ||
      !Op.Inputs[0].isConst())
    return {};
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const auto IsScalar = [](const NdVar &V) {
    return V.isReg() || V.isTemp() || V.isConst();
  };
  return {.TargetArch = A,
          .Kind = Id,
          .Ordering = Op.MemoryOrdering,
          .Space = Op.MemoryAddressSpace,
          .NumInputs = Op.NumInputs,
          .OutputSize = Op.Output.Size,
          .ValueSize = Op.Inputs[1].Size,
          .ControlSize = Op.Inputs[2].Size,
          .IdMatches = Op.Inputs[0].Size == 2 &&
                       Op.Inputs[0].Offset == static_cast<uint64_t>(Id),
          .WritableOutput = Op.Output.isReg() || Op.Output.isTemp(),
          .ValueIsScalar = IsScalar(Op.Inputs[1]),
          .ControlIsScalar = IsScalar(Op.Inputs[2])};
}

} // namespace neverd

#endif // NEVERD_IR_X86_X87GUARDEDVALUE_H
