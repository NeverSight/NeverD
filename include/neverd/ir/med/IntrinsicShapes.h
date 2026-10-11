//===- IntrinsicShapes.h - MedIR intrinsic contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Adapts MedIR value kinds to representation-neutral intrinsic contracts.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_INTRINSICSHAPES_H
#define NEVERD_IR_MED_INTRINSICSHAPES_H

#include "neverd/ir/X86FPState.h"
#include "neverd/ir/X86ShadowStack.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

inline bool isMedIntrinsicScalarInput(const MedVar &Value) {
  switch (Value.Kind) {
  case MedVar::Reg:
  case MedVar::Temp:
  case MedVar::Const:
    return true;
  case MedVar::Stack:
  case MedVar::Param:
  case MedVar::RetVal:
  case MedVar::Flag:
  case MedVar::EHException:
  case MedVar::EHSelector:
  case MedVar::SEHExceptionCode:
  case MedVar::Unspecified:
    return false;
  }
  return false;
}

inline bool isMedIntrinsicWritableScalar(const MedVar &Value) {
  return Value.Kind == MedVar::Reg || Value.Kind == MedVar::Temp;
}

inline X86ShadowStackReadShape
x86ShadowStackReadMedShape(const MedOp &Op, Arch TargetArch = Arch::Unknown) {
  bool Matches = true;
  const auto Observe = [&](const MedVar &V) {
    if (V.TheArch == Arch::Unknown)
      return;
    if (TargetArch == Arch::Unknown)
      TargetArch = V.TheArch;
    else if (V.TheArch != TargetArch)
      Matches = false;
  };
  Observe(Op.Output);
  for (unsigned I = 1; I < Op.NumInputs && I < Op.Inputs.size(); ++I)
    Observe(Op.Inputs[I]);
  return {.TargetArch = TargetArch,
          .MemoryOrdering = Op.MemoryOrdering,
          .MemoryAddressSpace = Op.MemoryAddressSpace,
          .NumInputs = Op.NumInputs,
          .IdIsConst = Op.NumInputs && Op.Inputs[0].isConst() &&
                       Op.Inputs[0].ConstVal == unsigned(Intrinsic::CetRdSsp),
          .IdSize = Op.NumInputs ? Op.Inputs[0].Size : 0U,
          .OutputIsWritable = isMedIntrinsicWritableScalar(Op.Output),
          .OutputSize = Op.Output.Size,
          .OldIsScalar =
              Op.NumInputs > 1 && (isMedIntrinsicScalarInput(Op.Inputs[1]) ||
                                   Op.Inputs[1].Kind == MedVar::Param),
          .OldSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : 0U,
          .WidthIsConst = Op.NumInputs > 2 && Op.Inputs[2].isConst(),
          .WidthSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : 0U,
          .ReadWidth = Op.NumInputs > 2 ? Op.Inputs[2].ConstVal : 0U,
          .HasAuxiliaryOutputs = !Op.IntrinsicOutputs.empty(),
          .ArchitectureMatchesOperands = Matches};
}

inline X86FPStateShape x86FPStateMedShape(const MedOp &Op,
                                          Arch TargetArch = Arch::Unknown) {
  if (Op.NumInputs > Op.Inputs.size())
    return {.TargetArch = TargetArch};
  const bool Memory = Op.NumInputs && Op.Inputs[0].isConst() &&
                      isX86FPStateMemoryIntrinsic(
                          static_cast<Intrinsic>(Op.Inputs[0].ConstVal));
  const bool Round =
      Op.NumInputs && Op.Inputs[0].isConst() &&
      isX86FPRoundStateIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].ConstVal));
  const bool Arithmetic =
      Op.NumInputs && Op.Inputs[0].isConst() &&
      isX86FPArithStateIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].ConstVal));
  const unsigned ControlIndex = Memory ? 2 : 1;
  const bool Conversion = Op.NumInputs && Op.Inputs[0].isConst() &&
                          isX86FPConversionStateIntrinsic(
                              static_cast<Intrinsic>(Op.Inputs[0].ConstVal));
  bool Scalar = true;
  bool ArchitectureMatchesOperands = true;
  const auto ObserveArch = [&](const MedVar &Value) {
    if (Value.TheArch == Arch::Unknown)
      return;
    if (TargetArch == Arch::Unknown)
      TargetArch = Value.TheArch;
    else if (TargetArch != Value.TheArch)
      ArchitectureMatchesOperands = false;
  };
  ObserveArch(Op.Output);
  for (unsigned Index = 1; Index < Op.NumInputs; ++Index) {
    Scalar &= isMedIntrinsicScalarInput(Op.Inputs[Index]) ||
              Op.Inputs[Index].Kind == MedVar::Param;
    ObserveArch(Op.Inputs[Index]);
  }
  return {.TargetArch = TargetArch,
          .MemoryOrdering = Op.MemoryOrdering,
          .MemoryAddressSpace = Op.MemoryAddressSpace,
          .NumInputs = Op.NumInputs,
          .IdIsConst = Op.NumInputs && Op.Inputs[0].isConst(),
          .IdSize = Op.NumInputs ? Op.Inputs[0].Size : 0U,
          .IdValue = Op.NumInputs ? Op.Inputs[0].ConstVal : 0U,
          .OutputIsWritable = isMedIntrinsicWritableScalar(Op.Output),
          .OutputSize = Op.Output.Size,
          .OperandsAreScalar = Scalar,
          .LeftSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : 0U,
          .RightSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : 0U,
          .ThirdSize = Op.NumInputs > 3 ? Op.Inputs[3].Size : 0U,
          .ArithmeticLeftIsZero = Op.NumInputs > (Memory ? 3U : 2U) &&
                                  Op.Inputs[Memory ? 3 : 2].isConst() &&
                                  Op.Inputs[Memory ? 3 : 2].ConstVal == 0,
          .StateSize =
              Round || Arithmetic ? (Op.NumInputs > 4 ? Op.Inputs[4].Size : 0U)
              : Conversion        ? (Op.NumInputs > 2 ? Op.Inputs[2].Size : 0U)
                                  : (Op.NumInputs > 3 ? Op.Inputs[3].Size : 0U),
          .HasAuxiliaryOutputs = !Op.IntrinsicOutputs.empty(),
          .DestinationIsConst = Op.NumInputs > 3 && Op.Inputs[3].isConst(),
          .DestinationSelectorSize = Op.NumInputs > 3 ? Op.Inputs[3].Size : 0U,
          .DestinationBytes = Op.NumInputs > 3 ? Op.Inputs[3].ConstVal : 0U,
          .ArchitectureMatchesOperands = ArchitectureMatchesOperands,
          .ControlIsConst =
              Op.NumInputs > ControlIndex && Op.Inputs[ControlIndex].isConst(),
          .ControlSize =
              Op.NumInputs > ControlIndex ? Op.Inputs[ControlIndex].Size : 0U,
          .Control = Op.NumInputs > ControlIndex
                         ? Op.Inputs[ControlIndex].ConstVal
                         : 0U,
          .ImmediateIsConst = Op.NumInputs > 3 && Op.Inputs[3].isConst(),
          .ImmediateSize = Op.NumInputs > 3 ? Op.Inputs[3].Size : 0U,
          .Immediate = Op.NumInputs > 3 ? Op.Inputs[3].ConstVal : 0U,
          .AddressIsScalar = Op.NumInputs > 1 && Scalar};
}

inline ApxAtomicIntrinsicShape apxAtomicMedShape(const MedOp &Op) {
  Arch TargetArch = Arch::Unknown;
  const auto ObserveArch = [&](const MedVar &Value) {
    if (Value.isConst() || Value.Size == 0 || Value.TheArch == Arch::Unknown)
      return;
    if (Value.TheArch != Arch::X64)
      TargetArch = Value.TheArch;
    else if (TargetArch == Arch::Unknown)
      TargetArch = Arch::X64;
  };
  ObserveArch(Op.Output);
  for (uint8_t I = 1; I < Op.NumInputs && I < 5; ++I)
    ObserveArch(Op.Inputs[I]);

  return {
      .TargetArch = TargetArch,
      .MemoryOrdering = Op.MemoryOrdering,
      .NumInputs = Op.NumInputs,
      .IntrinsicIdIsConst = Op.NumInputs > 0 && Op.Inputs[0].isConst(),
      .IntrinsicIdSize = Op.NumInputs > 0 ? Op.Inputs[0].Size : uint16_t{0},
      .OutputIsWritableScalar = isMedIntrinsicWritableScalar(Op.Output),
      .OutputSize = Op.Output.Size,
      .AddressIsScalar =
          Op.NumInputs > 1 && isMedIntrinsicScalarInput(Op.Inputs[1]),
      .AddressSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
      .SourceIsScalar =
          Op.NumInputs > 2 && isMedIntrinsicScalarInput(Op.Inputs[2]),
      .SourceSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
      .CompareIsScalar =
          Op.NumInputs > 3 && isMedIntrinsicScalarInput(Op.Inputs[3]),
      .CompareSize = Op.NumInputs > 3 ? Op.Inputs[3].Size : uint16_t{0},
      .ConditionIsConst = Op.NumInputs > 4 && Op.Inputs[4].isConst(),
      .Condition = Op.NumInputs > 4 ? Op.Inputs[4].ConstVal : UINT64_C(0),
      .ConditionSize = Op.NumInputs > 4 ? Op.Inputs[4].Size : uint16_t{0},
  };
}

inline X86InvalidateIntrinsicShape x86InvalidateMedShape(const MedOp &Op) {
  Arch TargetArch = Arch::Unknown;
  const auto ObserveArch = [&](const MedVar &Value) {
    if (Value.isConst() || Value.Size == 0 || Value.TheArch == Arch::Unknown)
      return;
    if (Value.TheArch != Arch::X64)
      TargetArch = Value.TheArch;
    else if (TargetArch == Arch::Unknown)
      TargetArch = Arch::X64;
  };
  for (uint8_t I = 1; I < Op.NumInputs && I < 4; ++I)
    ObserveArch(Op.Inputs[I]);

  return {
      .TargetArch = TargetArch,
      .MemoryOrdering = Op.MemoryOrdering,
      .MemoryAddressSpace = Op.MemoryAddressSpace,
      .NumInputs = Op.NumInputs,
      .IntrinsicIdIsConst = Op.NumInputs > 0 && Op.Inputs[0].isConst(),
      .IntrinsicIdSize = Op.NumInputs > 0 ? Op.Inputs[0].Size : uint16_t{0},
      .OutputSize = Op.Output.Size,
      .DescriptorAddressIsScalar =
          Op.NumInputs > 1 && isMedIntrinsicScalarInput(Op.Inputs[1]),
      .DescriptorAddressSize =
          Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
      .KindIsConst = Op.NumInputs > 2 && Op.Inputs[2].isConst(),
      .Kind = Op.NumInputs > 2 ? Op.Inputs[2].ConstVal : UINT64_C(0),
      .KindSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
      .TypeIsScalar =
          Op.NumInputs > 3 && isMedIntrinsicScalarInput(Op.Inputs[3]),
      .TypeSize = Op.NumInputs > 3 ? Op.Inputs[3].Size : uint16_t{0},
  };
}

inline X86MsrAccessIntrinsicShape x86MsrAccessMedShape(const MedOp &Op) {
  Arch TargetArch = Arch::Unknown;
  const auto ObserveArch = [&](const MedVar &Value) {
    if (Value.isConst() || Value.Size == 0 || Value.TheArch == Arch::Unknown)
      return;
    if (Value.TheArch != Arch::X64)
      TargetArch = Value.TheArch;
    else if (TargetArch == Arch::Unknown)
      TargetArch = Arch::X64;
  };
  ObserveArch(Op.Output);
  for (uint8_t I = 1; I < Op.NumInputs && I < 4; ++I)
    ObserveArch(Op.Inputs[I]);

  return {
      .TargetArch = TargetArch,
      .MemoryOrdering = Op.MemoryOrdering,
      .MemoryAddressSpace = Op.MemoryAddressSpace,
      .NumInputs = Op.NumInputs,
      .IntrinsicIdIsConst = Op.NumInputs > 0 && Op.Inputs[0].isConst(),
      .IntrinsicIdSize = Op.NumInputs > 0 ? Op.Inputs[0].Size : uint16_t{0},
      .OutputIsWritableScalar = isMedIntrinsicWritableScalar(Op.Output),
      .OutputSize = Op.Output.Size,
      .KindIsConst = Op.NumInputs > 1 && Op.Inputs[1].isConst(),
      .Kind = Op.NumInputs > 1 ? Op.Inputs[1].ConstVal : UINT64_C(0),
      .KindSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
      .SelectorIsScalar =
          Op.NumInputs > 2 && isMedIntrinsicScalarInput(Op.Inputs[2]),
      .SelectorSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
      .ValueIsScalar =
          Op.NumInputs > 3 && isMedIntrinsicScalarInput(Op.Inputs[3]),
      .ValueSize = Op.NumInputs > 3 ? Op.Inputs[3].Size : uint16_t{0},
  };
}

inline X86DivPreconditionIntrinsicShape
x86DivPreconditionMedShape(const MedOp &Op) {
  // i386 calling-convention recovery rewrites immutable stack-argument loads
  // to Param values before the final pipeline verification.  Parameters are
  // readable scalar operands for this side-effect-only guard even though the
  // stricter generic intrinsic classifier excludes them from raw instruction
  // contracts.
  const auto IsReadableScalar = [](const MedVar &Value) {
    return isMedIntrinsicScalarInput(Value) || Value.Kind == MedVar::Param;
  };
  Arch TargetArch = Arch::Unknown;
  if (Op.NumInputs > 1 && !Op.Inputs[1].isConst())
    TargetArch = Op.Inputs[1].TheArch;
  return {
      .TargetArch = TargetArch,
      .MemoryOrdering = Op.MemoryOrdering,
      .MemoryAddressSpace = Op.MemoryAddressSpace,
      .NumInputs = Op.NumInputs,
      .IntrinsicIdIsConst = Op.NumInputs > 0 && Op.Inputs[0].isConst(),
      .IntrinsicIdSize = Op.NumInputs > 0 ? Op.Inputs[0].Size : uint16_t{0},
      .OutputSize = Op.Output.Size,
      .DividendIsScalar = Op.NumInputs > 1 && IsReadableScalar(Op.Inputs[1]),
      .DividendSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
      .DivisorIsScalar = Op.NumInputs > 2 && IsReadableScalar(Op.Inputs[2]),
      .DivisorSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
      .KindIsConst = Op.NumInputs > 3 && Op.Inputs[3].isConst(),
      .Kind = Op.NumInputs > 3 ? Op.Inputs[3].ConstVal : UINT64_C(0),
      .KindSize = Op.NumInputs > 3 ? Op.Inputs[3].Size : uint16_t{0},
  };
}

inline PdepPextIntrinsicShape pdepPextMedShape(const MedOp &Op) {
  return {
      .MemoryOrdering = Op.MemoryOrdering,
      .MemoryAddressSpace = Op.MemoryAddressSpace,
      .NumInputs = Op.NumInputs,
      .IntrinsicIdIsConst = Op.NumInputs > 0 && Op.Inputs[0].isConst(),
      .IntrinsicIdSize = Op.NumInputs > 0 ? Op.Inputs[0].Size : uint16_t{0},
      .OutputIsWritableScalar = isMedIntrinsicWritableScalar(Op.Output),
      .OutputSize = Op.Output.Size,
      .SourceIsScalar =
          Op.NumInputs > 1 && isMedIntrinsicScalarInput(Op.Inputs[1]),
      .SourceSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
      .MaskIsScalar =
          Op.NumInputs > 2 && isMedIntrinsicScalarInput(Op.Inputs[2]),
      .MaskSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
  };
}

} // namespace neverd

#endif // NEVERD_IR_MED_INTRINSICSHAPES_H
