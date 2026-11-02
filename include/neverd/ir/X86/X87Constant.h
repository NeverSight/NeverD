//===- X87Constant.h - x87 constant and control contracts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact binary80 ROM values and the explicit control-word value boundary.
/// This numerical contract does not authenticate a source stack's TOP/tags.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86_X87CONSTANT_H
#define NEVERD_IR_X86_X87CONSTANT_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/LowIR.h"

#include <array>
#include <optional>

namespace neverd {

struct X87ConstantBits {
  uint64_t Significand;
  uint16_t Exponent;
};

inline constexpr const char *X87ConstantMnemonics[] = {
    "fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2", "fldz"};

/// Intel SDM Vol. 2A, FLD1/FLDL2T/FLDL2E/FLDPI/FLDLG2/FLDLN2/FLDZ.
/// The ROM's 66-bit significand rounds to extended precision according to RC,
/// independently of PC. No intermediate IEEE double rounding is permitted.
inline std::optional<X87ConstantBits> x87ConstantBits(unsigned Selector,
                                                      uint16_t Control) {
  constexpr X87ConstantBits Down[] = {{UINT64_C(0x8000000000000000), 0x3fff},
                                      {UINT64_C(0xd49a784bcd1b8afe), 0x4000},
                                      {UINT64_C(0xb8aa3b295c17f0bb), 0x3fff},
                                      {UINT64_C(0xc90fdaa22168c234), 0x4000},
                                      {UINT64_C(0x9a209a84fbcff798), 0x3ffd},
                                      {UINT64_C(0xb17217f7d1cf79ab), 0x3ffe},
                                      {0, 0}};
  if (Selector >= std::size(Down))
    return std::nullopt;
  auto Result = Down[Selector];
  const unsigned RC = (Control >> 10) & 3;
  if ((RC == 2 && Selector >= 1 && Selector <= 5) ||
      (RC == 0 && Selector >= 2 && Selector <= 5))
    ++Result.Significand;
  return Result;
}

constexpr bool isX87ConstantControlIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X87ReadControl || Id == Intrinsic::X87WriteControl ||
         Id == Intrinsic::X87LoadConstant;
}

struct X87ConstantControlShape {
  Arch TargetArch = Arch::Unknown;
  Intrinsic Kind = Intrinsic::None;
  NdMemoryOrdering Ordering = NdMemoryOrdering::None;
  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default;
  unsigned NumInputs = 0, OutputSize = 0;
  bool IdMatches = false, WritableOutput = false;
  bool SelectorIsConst = false;
  uint64_t Selector = 0;
  unsigned SelectorSize = 0, ControlSize = 0;
  bool ControlIsValue = false, HasAuxiliaryOutputs = false;
};

constexpr bool
x87ConstantControlShapeIsValid(Intrinsic Id, const X87ConstantControlShape &S) {
  if (!isX87ConstantControlIntrinsic(Id) || S.Kind != Id || !S.IdMatches ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X86 &&
       S.TargetArch != Arch::X64) ||
      S.Ordering != NdMemoryOrdering::None ||
      S.Space != NdMemoryAddressSpace::Default || S.HasAuxiliaryOutputs)
    return false;
  if (Id == Intrinsic::X87ReadControl)
    return S.NumInputs == 1 && S.OutputSize == 2 && S.WritableOutput;
  if (Id == Intrinsic::X87WriteControl)
    return S.NumInputs == 2 && S.OutputSize == 0 && S.ControlIsValue &&
           S.ControlSize == 2;
  return S.NumInputs == 3 && S.OutputSize == 10 && S.WritableOutput &&
         S.SelectorIsConst && S.SelectorSize == 1 && S.Selector < 7 &&
         S.ControlIsValue && S.ControlSize == 2;
}

inline X87ConstantControlShape x87ConstantControlLowShape(const LowOp &Op,
                                                          Arch A) {
  const auto Id = Op.NumInputs && Op.Inputs[0].isConst()
                      ? static_cast<Intrinsic>(Op.Inputs[0].Offset)
                      : Intrinsic::None;
  const unsigned C = Id == Intrinsic::X87LoadConstant ? 2 : 1;
  const auto IsValue = [](const NdVar &V) {
    return V.isReg() || V.isTemp() || V.isConst();
  };
  return {.TargetArch = A,
          .Kind = Id,
          .Ordering = Op.MemoryOrdering,
          .Space = Op.MemoryAddressSpace,
          .NumInputs = Op.NumInputs,
          .OutputSize = Op.Output.Size,
          .IdMatches =
              Op.NumInputs && Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2,
          .WritableOutput = Op.Output.isReg() || Op.Output.isTemp(),
          .SelectorIsConst = Op.NumInputs > 1 && Op.Inputs[1].isConst(),
          .Selector = Op.NumInputs > 1 ? Op.Inputs[1].Offset : 0,
          .SelectorSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : 0U,
          .ControlSize = Op.NumInputs > C ? Op.Inputs[C].Size : 0U,
          .ControlIsValue = Op.NumInputs > C && IsValue(Op.Inputs[C])};
}
} // namespace neverd
#endif
