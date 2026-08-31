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
    return false;
  }
  return false;
}

inline bool isMedIntrinsicWritableScalar(const MedVar &Value) {
  return Value.Kind == MedVar::Reg || Value.Kind == MedVar::Temp;
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
      .DividendIsScalar =
          Op.NumInputs > 1 && isMedIntrinsicScalarInput(Op.Inputs[1]),
      .DividendSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
      .DivisorIsScalar =
          Op.NumInputs > 2 && isMedIntrinsicScalarInput(Op.Inputs[2]),
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
