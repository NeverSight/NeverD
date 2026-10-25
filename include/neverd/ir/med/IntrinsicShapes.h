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

#include "neverd/ir/X86/X86FPState.h"
#include "neverd/ir/X86/X86ShadowStack.h"
#include "neverd/ir/X86/X87Constant.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/X86/X87GuardedValue.h"
#include "neverd/ir/X86/X87PortableABI.h"
#include "neverd/ir/X86/X87Value.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPredicate.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86AmxState.h"
#include "neverd/ir/intrinsics/X86/X86BoundedLeaf.h"
#include "neverd/ir/intrinsics/X86/X86DescriptorState.h"
#include "neverd/ir/intrinsics/X86/X86RepState.h"
#include "neverd/ir/intrinsics/X86/X86Sgx.h"
#include "neverd/ir/intrinsics/X86/X86SystemState.h"
#include "neverd/ir/intrinsics/X86/X86Virtualization.h"
#include "neverd/ir/med/MedIR.h"

namespace neverd {

/// Structural admission before inspecting an intrinsic ID, including during
/// module preflight for bodies which will not be emitted.
inline bool hasValidMedIntrinsicId(const MedOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
         Op.NumInputs <= Op.Inputs.size() && Op.Inputs.front().isConst();
}

inline bool aarch64VectorMedShapeIsValid(const MedOp &Op) {
  using namespace aarch64_sve;
  if (!hasValidMedIntrinsicId(Op) || !Op.IntrinsicOutputs.empty() ||
      Op.NumInputs > kVectorMaxOperands + 1 ||
      Op.Inputs[0].ConstVal >= static_cast<uint64_t>(Intrinsic::_Count) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      (Op.Output.Size &&
       (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp)))
    return false;
  VectorShape Shape;
  Shape.NumOperands = Op.NumInputs - 1;
  Shape.OutputBytes = Op.Output.Size;
  for (unsigned I = 1; I != Op.NumInputs; ++I) {
    const auto &Input = Op.Inputs[I];
    if (Input.Kind != MedVar::Reg && Input.Kind != MedVar::Temp &&
        Input.Kind != MedVar::Param && !Input.isConst())
      return false;
    Shape.Operands[I - 1] = {
        Input.Size, Input.isConst() ? std::optional<uint64_t>(Input.ConstVal)
                                    : std::nullopt};
  }
  return isValidVectorShape(static_cast<Intrinsic>(Op.Inputs[0].ConstVal),
                            Shape);
}

inline bool aarch64SVEPrefixMedShapeIsValid(const MedOp &Op) {
  return aarch64VectorMedShapeIsValid(Op) &&
         aarch64_sve::prefixOperation(
             static_cast<Intrinsic>(Op.Inputs[0].ConstVal))
             .has_value();
}

inline bool aarch64SVECPAMedShapeIsValid(const MedOp &Op) {
  return aarch64VectorMedShapeIsValid(Op) &&
         aarch64_sve::cpaOperation(
             static_cast<Intrinsic>(Op.Inputs[0].ConstVal))
             .has_value();
}

inline bool aarch64AddressMemoryMedShapeIsValid(const MedOp &Op) {
  if (!hasValidMedIntrinsicId(Op) ||
      Op.NumInputs != aarch64AddressMemoryOperandCount(
                          static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) +
                          1 ||
      Op.Inputs[0].Size != kAddressMemoryIdentifierBytes ||
      Op.Inputs[0].ConstVal >= static_cast<uint64_t>(Intrinsic::_Count) ||
      !Op.IntrinsicOutputs.empty() ||
      (Op.Output.Size && Op.Output.Kind != MedVar::Reg &&
       Op.Output.Kind != MedVar::Temp) ||
      (Op.Output.TheArch != Arch::Unknown &&
       Op.Output.TheArch != Arch::AArch64) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I != Op.NumInputs; ++I)
    if ((!Op.Inputs[I].isConst() && Op.Inputs[I].Kind != MedVar::Reg &&
         Op.Inputs[I].Kind != MedVar::Temp &&
         Op.Inputs[I].Kind != MedVar::Param) ||
        (!Op.Inputs[I].isConst() && Op.Inputs[I].TheArch != Arch::Unknown &&
         Op.Inputs[I].TheArch != Arch::AArch64))
      return false;
  return aarch64AddressMemoryShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].ConstVal), Op.NumInputs,
      Op.Output.Size, Op.Inputs[kAddressMemoryBaseOperand + 1].Size,
      Op.Inputs[kAddressMemoryIndexOperand + 1].Size,
      Op.Inputs[kAddressMemoryWordOperand + 1].Size,
      Op.Inputs[kAddressMemoryWordOperand + 1].isConst()
          ? std::optional<uint64_t>(
                Op.Inputs[kAddressMemoryWordOperand + 1].ConstVal)
          : std::nullopt,
      Op.Inputs[kAddressMemoryIndexOperand + 1].isConst()
          ? std::optional<uint64_t>(
                Op.Inputs[kAddressMemoryIndexOperand + 1].ConstVal)
          : std::nullopt,
      Op.NumInputs == kAddressStoreOperandCount + 1
          ? Op.Inputs[kAddressMemoryDataOperand + 1].Size
          : 0,
      Op.NumInputs == kAddressStoreOperandCount + 1 &&
              Op.Inputs[kAddressMemoryDataOperand + 1].isConst()
          ? std::optional<uint64_t>(
                Op.Inputs[kAddressMemoryDataOperand + 1].ConstVal)
          : std::nullopt);
}

inline bool aarch64AddressLoadMedShapeIsValid(const MedOp &Op) {
  return Op.Output.Size && aarch64AddressMemoryMedShapeIsValid(Op);
}

inline bool aarch64PAuthLoadMedShapeIsValid(const MedOp &Op) {
  if (!hasValidMedIntrinsicId(Op) ||
      Op.NumInputs != kPAuthLoadOperandCount + 1 ||
      Op.Inputs[0].ConstVal >= static_cast<uint64_t>(Intrinsic::_Count) ||
      !Op.IntrinsicOutputs.empty() ||
      (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp) ||
      (!Op.Inputs[1].isConst() && Op.Inputs[1].Kind != MedVar::Reg &&
       Op.Inputs[1].Kind != MedVar::Temp &&
       Op.Inputs[1].Kind != MedVar::Param) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return aarch64PAuthLoadShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].ConstVal), Op.NumInputs,
      Op.Output.Size, Op.Inputs[1].Size, Op.Inputs[2].Size,
      Op.Inputs[2].isConst() ? std::optional<uint64_t>(Op.Inputs[2].ConstVal)
                             : std::nullopt);
}

inline bool x86DescriptorStateMedShapeIsValid(const MedOp &Op,
                                              Arch TargetArch = Arch::Unknown) {
  if (!hasValidMedIntrinsicId(Op) || Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].ConstVal >= static_cast<uint64_t>(Intrinsic::_Count) ||
      !Op.IntrinsicOutputs.empty() ||
      !x86DescriptorStateShapeIsValid(
          static_cast<Intrinsic>(Op.Inputs[0].ConstVal), TargetArch,
          Op.NumInputs, Op.Output.Size,
          Op.NumInputs > 1 ? Op.Inputs[1].Size : 0, Op.MemoryAddressSpace,
          Op.MemoryOrdering))
    return false;
  auto OwnedRegister = [&](const MedVar &V) {
    return (V.Kind != MedVar::Reg && V.Kind != MedVar::Flag) ||
           V.TheArch == Arch::Unknown ||
           (V.TheArch == Arch::X86 || V.TheArch == Arch::X64) &&
               (TargetArch == Arch::Unknown || V.TheArch == TargetArch);
  };
  const bool VerifyFlag =
      isX86DescriptorVerify(static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) &&
      Op.Output.Kind == MedVar::Flag;
  if (Op.Output.Size && ((Op.Output.Kind != MedVar::Reg &&
                          Op.Output.Kind != MedVar::Temp && !VerifyFlag) ||
                         !OwnedRegister(Op.Output)))
    return false;
  for (unsigned I = 1; I < Op.NumInputs; ++I) {
    const auto &V = Op.Inputs[I];
    if ((!V.isConst() && V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
         V.Kind != MedVar::Param) ||
        !OwnedRegister(V))
      return false;
  }
  return true;
}

inline bool aarch64PtrueMedShapeIsValid(const MedOp &Op) {
  using namespace aarch64_sve;
  if (!hasValidMedIntrinsicId(Op) || Op.NumInputs != kPtrueOperandCount + 1 ||
      Op.Inputs[0].ConstVal != static_cast<uint64_t>(Intrinsic::A64_SvePtrue) ||
      !Op.IntrinsicOutputs.empty() ||
      (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return isValidPtrueShape(
      {Op.NumInputs - 1U, Op.Output.Size, Op.Inputs[1].Size, Op.Inputs[2].Size,
       Op.Inputs[1].isConst() ? std::optional<uint64_t>(Op.Inputs[1].ConstVal)
                              : std::nullopt,
       Op.Inputs[2].isConst() ? std::optional<uint64_t>(Op.Inputs[2].ConstVal)
                              : std::nullopt});
}

inline bool portableX87MedShapeIsValid(const MedOp &Op) {
  if (!hasValidMedIntrinsicId(Op) || !Op.IntrinsicOutputs.empty() ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Op.Output.Kind != MedVar::Temp || Op.Output.Id < 0 ||
      (Op.Output.TheArch != Arch::Unknown && Op.Output.TheArch != Arch::X64))
    return false;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
  if (!isPortableX87Intrinsic(Id))
    return false;
  const auto &Signature = x87PortableHelperSignatureV1(
      Id == Intrinsic::PortableX87Step ? X87PortableHelperV1::Step
                                       : X87PortableHelperV1::Result);
  const auto Bytes = [](X87PortableABIValueV1 Value) -> unsigned {
    return Value == X87PortableABIValueV1::I32 ? 4 : 8;
  };
  if (Op.NumInputs != Signature.Parameters.size() + 1 ||
      Op.Inputs[0].Size != 4 || Op.Output.Size != Bytes(Signature.Result))
    return false;
  for (unsigned I = 0; I != Signature.Parameters.size(); ++I) {
    const auto &Value = Op.Inputs[I + 1];
    if (Value.Size != Bytes(Signature.Parameters[I]) ||
        (Value.Kind != MedVar::Param && Value.Kind != MedVar::Temp &&
         !Value.isConst()) ||
        (Value.TheArch != Arch::Unknown && Value.TheArch != Arch::X64))
      return false;
  }
  if (Op.Inputs[1].Kind != MedVar::Param)
    return false;
  return Id == Intrinsic::PortableX87Result ||
         (Op.Inputs[2].isConst() && Op.Inputs[2].ConstVal <= UINT32_MAX &&
          Op.Inputs[3].isConst());
}

inline bool aarch64PAuthMedShapeIsValid(const MedOp &Op) {
  if (!hasValidMedIntrinsicId(Op) || Op.NumInputs < 2 ||
      Op.NumInputs > kAArch64PACMOperandCount + 1 ||
      Op.Inputs[0].ConstVal >= static_cast<uint64_t>(Intrinsic::_Count) ||
      !Op.IntrinsicOutputs.empty() ||
      (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I < Op.NumInputs; ++I)
    if (!Op.Inputs[I].isConst() && Op.Inputs[I].Kind != MedVar::Reg &&
        Op.Inputs[I].Kind != MedVar::Temp && Op.Inputs[I].Kind != MedVar::Param)
      return false;
  const bool TwoModifiers = aarch64PAuthExtendedOperation(
                                static_cast<Intrinsic>(Op.Inputs[0].ConstVal))
                                .has_value();
  return aarch64PAuthShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].ConstVal),
      {.NumInputs = Op.NumInputs,
       .OutputBytes = Op.Output.Size,
       .PointerBytes = Op.Inputs[1].Size,
       .ModifierBytes =
           Op.NumInputs > 2
               ? Op.Inputs[TwoModifiers ? 2 : Op.NumInputs - 1].Size
               : 0U,
       .SecondModifierBytes =
           TwoModifiers && Op.NumInputs >= 4 ? Op.Inputs[3].Size : 0U,
       .ModeBytes = Op.NumInputs == kAArch64PACMOperandCount + 1
                        ? Op.Inputs[kAArch64PACMOperandCount].Size
                        : 0U,
       .KeyBytes = Op.NumInputs == 4 ? Op.Inputs[2].Size : 0U,
       .Key = Op.NumInputs == 4 && Op.Inputs[2].isConst()
                  ? std::optional<uint64_t>(Op.Inputs[2].ConstVal)
                  : std::nullopt});
}

inline bool aarch64CPAMedShapeIsValid(const MedOp &Op) {
  if (!hasValidMedIntrinsicId(Op) || Op.NumInputs < 3 || Op.NumInputs > 4 ||
      Op.Inputs[0].ConstVal >= static_cast<uint64_t>(Intrinsic::_Count) ||
      !Op.IntrinsicOutputs.empty() ||
      (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I < Op.NumInputs; ++I)
    if (!Op.Inputs[I].isConst() && Op.Inputs[I].Kind != MedVar::Reg &&
        Op.Inputs[I].Kind != MedVar::Temp && Op.Inputs[I].Kind != MedVar::Param)
      return false;
  return aarch64CPAShapeIsValid(static_cast<Intrinsic>(Op.Inputs[0].ConstVal),
                                Op.NumInputs, Op.Output.Size, Op.Inputs[1].Size,
                                Op.Inputs[2].Size,
                                Op.NumInputs == 4 ? Op.Inputs[3].Size : 0);
}

inline bool x87ContextMedShapeIsValid(const MedOp &Op, Arch A) {
  if (Op.NumInputs > Op.Inputs.size() || Op.NumInputs > 6 ||
      !Op.IntrinsicOutputs.empty())
    return false;
  LowOp Low;
  Low.NumInputs = Op.NumInputs;
  Low.Output = NdVar::tmp(0, Op.Output.Size);
  Low.MemoryOrdering = Op.MemoryOrdering;
  Low.MemoryAddressSpace = Op.MemoryAddressSpace;
  const auto ObserveArch = [&](const MedVar &V) {
    if (V.TheArch == Arch::Unknown)
      return true;
    if (A == Arch::Unknown)
      A = V.TheArch;
    return A == V.TheArch;
  };
  if (!ObserveArch(Op.Output))
    return false;
  if (Op.Output.Size && Op.Output.Kind != MedVar::Reg &&
      Op.Output.Kind != MedVar::Temp)
    return false;
  for (unsigned I = 0; I < Op.NumInputs; ++I) {
    const auto &V = Op.Inputs[I];
    if (!ObserveArch(V))
      return false;
    if (!V.isConst() && V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
        V.Kind != MedVar::Param)
      return false;
    Low.Inputs[I] =
        V.isConst() ? NdVar::cst(V.ConstVal, V.Size) : NdVar::tmp(0, V.Size);
  }
  return x87ContextLowShapeIsValid(Low, A);
}

inline X87ConstantControlShape x87ConstantControlMedShape(const MedOp &Op,
                                                          Arch A) {
  LowOp Low;
  Low.NumInputs = Op.NumInputs;
  Low.MemoryOrdering = Op.MemoryOrdering;
  Low.MemoryAddressSpace = Op.MemoryAddressSpace;
  Low.Output = NdVar::tmp(0, Op.Output.Size);
  if (Op.Output.Size && Op.Output.Kind != MedVar::Reg &&
      Op.Output.Kind != MedVar::Temp)
    Low.Output.Space = VnodeSpace::CONST;
  bool Matches = true;
  const auto Observe = [&](const MedVar &V) {
    if (V.TheArch == Arch::Unknown)
      return;
    if (A == Arch::Unknown)
      A = V.TheArch;
    else if (A != V.TheArch)
      Matches = false;
  };
  Observe(Op.Output);
  if (Op.NumInputs > 6 || Op.NumInputs > Op.Inputs.size())
    return {};
  for (unsigned I = 0; I < Op.NumInputs; ++I) {
    const auto &V = Op.Inputs[I];
    Observe(V);
    Low.Inputs[I] =
        V.isConst() ? NdVar::cst(V.ConstVal, V.Size) : NdVar::tmp(0, V.Size);
    if (!V.isConst() && V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
        V.Kind != MedVar::Param)
      Low.Inputs[I].Space = VnodeSpace::RAM;
  }
  auto S = x87ConstantControlLowShape(Low, A);
  S.HasAuxiliaryOutputs = !Op.IntrinsicOutputs.empty() || !Matches;
  return S;
}

inline X87GuardedValueShape x87GuardedValueMedShape(const MedOp &Op, Arch A) {
  if (Op.NumInputs != 3 || Op.NumInputs > Op.Inputs.size())
    return {};
  LowOp Low;
  Low.Opcode = Op.Opcode;
  Low.MemoryOrdering = Op.MemoryOrdering;
  Low.MemoryAddressSpace = Op.MemoryAddressSpace;
  Low.Output = NdVar::tmp(0, Op.Output.Size);
  if (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp)
    return {};
  bool Matches = true;
  const auto Observe = [&](const MedVar &V) {
    if (V.TheArch == Arch::Unknown)
      return;
    if (A == Arch::Unknown)
      A = V.TheArch;
    else if (A != V.TheArch)
      Matches = false;
  };
  Observe(Op.Output);
  for (unsigned I = 0; I != Op.NumInputs; ++I) {
    const auto &V = Op.Inputs[I];
    Observe(V);
    if (!V.isConst() && V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
        V.Kind != MedVar::Param)
      return {};
    Low.addInput(V.isConst() ? NdVar::cst(V.ConstVal, V.Size)
                             : NdVar::tmp(0, V.Size));
  }
  auto Shape = x87GuardedValueLowShape(Low, A);
  Shape.HasAuxiliaryOutputs = !Op.IntrinsicOutputs.empty() || !Matches;
  return Shape;
}

inline X87ValueShape x87ValueMedShape(const MedOp &Op, Arch A) {
  if (!hasValidMedIntrinsicId(Op) || Op.NumInputs > 3 ||
      (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp))
    return {};
  LowOp Low;
  Low.Opcode = Op.Opcode;
  Low.MemoryOrdering = Op.MemoryOrdering;
  Low.MemoryAddressSpace = Op.MemoryAddressSpace;
  Low.Output = NdVar::tmp(0, Op.Output.Size);
  bool Matches = true;
  const auto Observe = [&](const MedVar &V) {
    if (V.TheArch == Arch::Unknown)
      return;
    if (A == Arch::Unknown)
      A = V.TheArch;
    else if (A != V.TheArch)
      Matches = false;
  };
  Observe(Op.Output);
  for (unsigned I = 0; I != Op.NumInputs; ++I) {
    const auto &V = Op.Inputs[I];
    Observe(V);
    if (!V.isConst() && V.Kind != MedVar::Reg && V.Kind != MedVar::Temp &&
        V.Kind != MedVar::Param)
      return {};
    Low.addInput(V.isConst() ? NdVar::cst(V.ConstVal, V.Size)
                             : NdVar::tmp(0, V.Size));
  }
  auto Shape = x87ValueLowShape(Low, A);
  Shape.HasAuxiliaryOutputs = !Op.IntrinsicOutputs.empty() || !Matches;
  return Shape;
}

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
  const bool ScalarConvert =
      Op.NumInputs && Op.Inputs[0].isConst() &&
      (isX86FPScalarConvertStateIntrinsic(
           static_cast<Intrinsic>(Op.Inputs[0].ConstVal)) ||
       isX86FPPackedConvertStateIntrinsic(
           static_cast<Intrinsic>(Op.Inputs[0].ConstVal)));
  const auto Id = Op.NumInputs && Op.Inputs[0].isConst()
                      ? static_cast<Intrinsic>(Op.Inputs[0].ConstVal)
                      : Intrinsic::X86ReadMXCSR;
  const bool Half = isX86F16CStateIntrinsic(Id),
             Store = Id == Intrinsic::X86F16CStoreState;
  const unsigned ControlIndex =
      Store ? 3 : (Memory || ScalarConvert || Half ? 2 : 1);
  const unsigned ImmediateIndex = isX86FPMaskCompareStateIntrinsic(Id)
                                      ? (Memory ? 3 : 2)
                                  : Store ? 4
                                          : 3;
  const bool Fma =
      Op.NumInputs && Op.Inputs[0].isConst() &&
      isX86FPFmaStateIntrinsic(static_cast<Intrinsic>(Op.Inputs[0].ConstVal));
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
  return {
      .TargetArch = TargetArch,
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
      .FourthSize = Op.NumInputs > 4 ? Op.Inputs[4].Size : 0U,
      .ArithmeticLeftIsZero = Op.NumInputs > (Memory ? 3U : 2U) &&
                              Op.Inputs[Memory ? 3 : 2].isConst() &&
                              Op.Inputs[Memory ? 3 : 2].ConstVal == 0,
      .StateSize =
          isX86FPMaskCompareStateIntrinsic(Id)
              ? (Op.NumInputs > 5 ? Op.Inputs[5].Size : 0U)
          : Half ? (Op.NumInputs > (Store ? 5U : 4U)
                        ? Op.Inputs[Store ? 5 : 4].Size
                        : 0U)
          : Round || Arithmetic || isX86FPComiStateIntrinsic(Id)
              ? (Op.NumInputs > (Fma ? 5U : 4U) ? Op.Inputs[Fma ? 5 : 4].Size
                                                : 0U)
          : Conversion ? (Op.NumInputs > 2 ? Op.Inputs[2].Size : 0U)
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
      .Control =
          Op.NumInputs > ControlIndex ? Op.Inputs[ControlIndex].ConstVal : 0U,
      .ImmediateIsConst =
          Op.NumInputs > ImmediateIndex && Op.Inputs[ImmediateIndex].isConst(),
      .ImmediateSize =
          Op.NumInputs > ImmediateIndex ? Op.Inputs[ImmediateIndex].Size : 0U,
      .Immediate = Op.NumInputs > ImmediateIndex
                       ? Op.Inputs[ImmediateIndex].ConstVal
                       : 0U,
      .AddressIsScalar = Op.NumInputs > 1 && Scalar};
}

/// Whether \p Id only checks a condition, and faults where it fails: the
/// alignment an aligned access requires (`movdqa`), a division's divisor
/// and quotient, BOUND's range and INTO's overflow.  It writes no register
/// and no memory.
constexpr bool onlyChecksForAFault(Intrinsic Id) {
  return Id == Intrinsic::RequireAligned ||
         Id == Intrinsic::X86RequireDivPrecondition || Id == Intrinsic::Bound ||
         Id == Intrinsic::Into;
}

/// Whether \p Op ends the setup of a later call's arguments: what an argument
/// register or outgoing stack slot held before it is no longer set up for
/// that call.  Another call ends it, and so does an intrinsic whose effects
/// are not known.  The x87 wait and context transport (preservesCallSetup),
/// the explicit numerical and MXCSR effects of the x86 floating-state
/// intrinsics and the checks that only fault (onlyChecksForAFault) call no
/// callee and write no argument storage, so a compiler may schedule argument
/// setup across them (`mov edi, 2; cvtsi2sd xmm1, ebp; call f`, or the
/// `movdqa` of a record copied to the stack after `mov %rbx, %rdi`).
inline bool endsCallArgumentSetup(const MedOp &Op) {
  if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
    return true;
  if (Op.Opcode != NdOp::INTRINSIC)
    return false;
  if (!hasValidMedIntrinsicId(Op))
    return true;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
  if (static_cast<uint64_t>(Id) != Op.Inputs[0].ConstVal)
    return true;
  return !preservesCallSetup(Id) && !onlyChecksForAFault(Id) &&
         !aarch64PAuthMedShapeIsValid(Op) &&
         !aarch64PAuthLoadMedShapeIsValid(Op) &&
         !aarch64AddressLoadMedShapeIsValid(Op) &&
         !aarch64CPAMedShapeIsValid(Op) && !aarch64SVECPAMedShapeIsValid(Op) &&
         !aarch64SVEPrefixMedShapeIsValid(Op) &&
         !x86FPStateShapeIsValid(Id, x86FPStateMedShape(Op));
}

inline ApxAtomicIntrinsicShape
apxAtomicMedShape(const MedOp &Op, Arch TargetArch = Arch::Unknown) {
  bool SameArch = true;
  const auto ObserveArch = [&](const MedVar &Value) {
    if (Value.isConst() || Value.Size == 0 || Value.TheArch == Arch::Unknown)
      return;
    if (TargetArch == Arch::Unknown)
      TargetArch = Value.TheArch;
    else if (TargetArch != Value.TheArch)
      SameArch = false;
  };
  ObserveArch(Op.Output);
  for (uint8_t I = 1; I < Op.NumInputs && I < 5; ++I)
    ObserveArch(Op.Inputs[I]);

  return {
      .TargetArch = TargetArch,
      .MemoryOrdering = Op.MemoryOrdering,
      .NumInputs = Op.NumInputs,
      .IntrinsicIdIsConst =
          SameArch && Op.NumInputs > 0 && Op.Inputs[0].isConst(),
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

inline X86VirtualizationShape
x86VirtualizationMedShape(const MedOp &Op, Arch TargetArch = Arch::Unknown) {
  X86VirtualizationShape Shape;
  Shape.TargetArch = TargetArch;
  if (Shape.TargetArch == Arch::Unknown && Op.Output.Size &&
      Op.Output.Kind == MedVar::Reg)
    Shape.TargetArch = Op.Output.TheArch;
  Shape.MemoryOrdering = Op.MemoryOrdering;
  Shape.MemoryAddressSpace = Op.MemoryAddressSpace;
  Shape.ValidId = Op.NumInputs && Op.NumInputs <= Op.Inputs.size() &&
                  Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
                  Op.Inputs[0].ConstVal <= UINT16_MAX &&
                  Op.IntrinsicOutputs.empty();
  if (Op.Output.Size && Op.Output.Kind == MedVar::Reg &&
      Op.Output.TheArch != Shape.TargetArch)
    Shape.ValidId = false;
  Shape.OutputSize = Op.Output.Size;
  Shape.WritableOutput =
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp) &&
      Op.Output.Id >= 0;
  Shape.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (uint8_t I = 0; I < Shape.Arguments && I < Shape.InputSizes.size(); ++I) {
    const auto &Input = Op.Inputs[I + 1];
    Shape.InputSizes[I] = Input.Size;
    Shape.ScalarInputs[I] =
        isMedIntrinsicScalarInput(Input) || Input.Kind == MedVar::Param ||
        Input.Kind == MedVar::Stack || Input.Kind == MedVar::RetVal;
    // TheArch names physical registers. EA/arithmetic temporaries are neutral
    // scalars, including the canonical 64-bit address carrier used on i386.
    if (Input.Kind == MedVar::Reg && Shape.TargetArch == Arch::Unknown)
      Shape.TargetArch = Input.TheArch;
    else if (Input.Kind == MedVar::Reg && Input.TheArch != Shape.TargetArch)
      Shape.ValidId = false;
  }
  return Shape;
}

inline X86AmxShape x86AmxMedShape(const MedOp &Op, Arch A = Arch::Unknown) {
  X86AmxShape S;
  S.TargetArch = A;
  S.MemoryOrdering = Op.MemoryOrdering;
  S.MemoryAddressSpace = Op.MemoryAddressSpace;
  S.ValidId =
      Op.NumInputs && Op.NumInputs <= std::size(Op.Inputs) &&
      Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
      Op.Inputs[0].ConstVal == static_cast<uint16_t>(Intrinsic::X86AMXState);
  S.WritableOutput =
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp) &&
      Op.Output.Id >= 0;
  S.OutputSize = Op.Output.Size;
  S.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &V = Op.Inputs[I + 1];
    S.InputSizes[I] = V.Size;
    S.ScalarInputs[I] = isMedIntrinsicScalarInput(V) ||
                        V.Kind == MedVar::Param || V.Kind == MedVar::Stack ||
                        V.Kind == MedVar::RetVal;
  }
  if (Op.NumInputs > 1)
    S.EmptyBase = Op.Inputs[1].isConst() && Op.Inputs[1].ConstVal == 0;
  if (Op.NumInputs > 2 && Op.Inputs[2].isConst() && Op.Inputs[2].Size == 4 &&
      Op.Inputs[2].ConstVal <= UINT32_MAX)
    S.Descriptor = static_cast<uint32_t>(Op.Inputs[2].ConstVal);
  if (Op.NumInputs > 3 && Op.Inputs[3].isConst())
    S.RowImmediate = Op.Inputs[3].ConstVal;
  if (!Op.IntrinsicOutputs.empty())
    S.ValidId = false;
  auto CheckArch = [&](const MedVar &V) {
    if (V.Kind != MedVar::Reg)
      return;
    if (S.TargetArch == Arch::Unknown)
      S.TargetArch = V.TheArch;
    else if (V.TheArch != S.TargetArch)
      S.ValidId = false;
  };
  CheckArch(Op.Output);
  for (unsigned I = 1; I < Op.NumInputs && I < Op.Inputs.size(); ++I)
    CheckArch(Op.Inputs[I]);
  return S;
}

inline X86RepShape x86RepMedShape(const MedOp &Op,
                                  Arch TargetArch = Arch::Unknown) {
  X86RepShape S;
  S.TargetArch = TargetArch;
  if (S.TargetArch == Arch::Unknown && Op.Output.Kind == MedVar::Reg)
    S.TargetArch = Op.Output.TheArch;
  S.MemoryOrdering = Op.MemoryOrdering;
  S.MemoryAddressSpace = Op.MemoryAddressSpace;
  S.ValidId = Op.NumInputs && Op.NumInputs <= std::size(Op.Inputs) &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
              Op.Inputs[0].ConstVal <= UINT16_MAX;
  if (Op.Output.Kind == MedVar::Reg && Op.Output.TheArch != S.TargetArch)
    S.ValidId = false;
  S.WritableOutput =
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp) &&
      Op.Output.Id >= 0;
  S.OutputSize = Op.Output.Size;
  S.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &Input = Op.Inputs[I + 1];
    S.InputSizes[I] = Input.Size;
    S.ScalarInputs[I] =
        isMedIntrinsicScalarInput(Input) || Input.Kind == MedVar::Param ||
        Input.Kind == MedVar::Stack || Input.Kind == MedVar::RetVal;
    if (Input.Kind == MedVar::Reg) {
      if (S.TargetArch == Arch::Unknown)
        S.TargetArch = Input.TheArch;
      else if (Input.TheArch != S.TargetArch)
        S.ValidId = false;
    }
  }
  if (Op.NumInputs > 5 && Op.Inputs[5].isConst() &&
      Op.Inputs[5].Size == X86RepDescriptorBytes &&
      Op.Inputs[5].ConstVal <= UINT32_MAX)
    S.Descriptor = static_cast<uint32_t>(Op.Inputs[5].ConstVal);
  const auto C =
      S.ValidId ? x86RepContract(static_cast<Intrinsic>(Op.Inputs[0].ConstVal))
                : std::nullopt;
  if (!C || Op.IntrinsicOutputs.size() != C->auxiliaryOutputs())
    S.ValidId = false;
  else
    for (unsigned I = 0; I < C->auxiliaryOutputs(); ++I) {
      const auto &V = Op.IntrinsicOutputs[I];
      if (V.Kind != MedVar::Temp || V.Id < 0 ||
          V.Size != (I + 1 < C->WordResults ? S.OutputSize : X86RepFlagBytes))
        S.ValidId = false;
      for (unsigned J = 0; J < I; ++J)
        if (Op.IntrinsicOutputs[J].Id == V.Id)
          S.ValidId = false;
      if (Op.Output.Kind == MedVar::Temp && V.Id == Op.Output.Id)
        S.ValidId = false;
    }
  return S;
}

inline X86SgxShape x86SgxMedShape(const MedOp &Op,
                                  Arch TargetArch = Arch::Unknown) {
  X86SgxShape S;
  S.TargetArch = TargetArch;
  if (S.TargetArch == Arch::Unknown && Op.Output.Kind == MedVar::Reg)
    S.TargetArch = Op.Output.TheArch;
  S.MemoryOrdering = Op.MemoryOrdering;
  S.MemoryAddressSpace = Op.MemoryAddressSpace;
  S.ValidId = Op.NumInputs && Op.NumInputs <= std::size(Op.Inputs) &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
              Op.Inputs[0].ConstVal <= UINT16_MAX;
  if (Op.Output.Kind == MedVar::Reg && Op.Output.TheArch != S.TargetArch)
    S.ValidId = false;
  S.WritableOutput =
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp) &&
      Op.Output.Id >= 0;
  S.OutputSize = Op.Output.Size;
  S.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &Input = Op.Inputs[I + 1];
    S.InputSizes[I] = Input.Size;
    S.ScalarInputs[I] =
        isMedIntrinsicScalarInput(Input) || Input.Kind == MedVar::Param ||
        Input.Kind == MedVar::Stack || Input.Kind == MedVar::RetVal;
    if (Input.Kind == MedVar::Reg) {
      if (S.TargetArch == Arch::Unknown)
        S.TargetArch = Input.TheArch;
      else if (Input.TheArch != S.TargetArch)
        S.ValidId = false;
    }
  }
  if (Op.IntrinsicOutputs.size() != X86SgxAuxiliaryOutputs)
    S.ValidId = false;
  else
    for (unsigned I = 0; I < X86SgxAuxiliaryOutputs; ++I) {
      const auto &V = Op.IntrinsicOutputs[I];
      if (V.Kind != MedVar::Temp || V.Id < 0 ||
          V.Size != (I == 0 ? S.OutputSize : X86SgxFlagBytes))
        S.ValidId = false;
    }
  if (Op.IntrinsicOutputs.size() == X86SgxAuxiliaryOutputs &&
      Op.IntrinsicOutputs[0].Id == Op.IntrinsicOutputs[1].Id)
    S.ValidId = false;
  return S;
}

inline X86BoundedLeafShape
x86BoundedLeafMedShape(const MedOp &Op, Arch TargetArch = Arch::Unknown) {
  X86BoundedLeafShape S;
  S.TargetArch = TargetArch;
  if (S.TargetArch == Arch::Unknown && Op.Output.Kind == MedVar::Reg)
    S.TargetArch = Op.Output.TheArch;
  S.MemoryOrdering = Op.MemoryOrdering;
  S.MemoryAddressSpace = Op.MemoryAddressSpace;
  S.ValidId = Op.NumInputs && Op.NumInputs <= std::size(Op.Inputs) &&
              Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
              Op.Inputs[0].ConstVal <= UINT16_MAX;
  if (Op.Output.Kind == MedVar::Reg && Op.Output.TheArch != S.TargetArch)
    S.ValidId = false;
  S.WritableOutput =
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp) &&
      Op.Output.Id >= 0;
  S.OutputSize = Op.Output.Size;
  S.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &Input = Op.Inputs[I + 1];
    S.InputSizes[I] = Input.Size;
    S.ScalarInputs[I] =
        isMedIntrinsicScalarInput(Input) || Input.Kind == MedVar::Param ||
        Input.Kind == MedVar::Stack || Input.Kind == MedVar::RetVal;
    if (Input.Kind == MedVar::Reg) {
      if (S.TargetArch == Arch::Unknown)
        S.TargetArch = Input.TheArch;
      else if (Input.TheArch != S.TargetArch)
        S.ValidId = false;
    }
  }
  if (Op.IntrinsicOutputs.size() != X86BoundedLeafAuxiliaryOutputs)
    S.ValidId = false;
  else
    for (unsigned I = 0; I < X86BoundedLeafAuxiliaryOutputs; ++I) {
      const auto &V = Op.IntrinsicOutputs[I];
      if (V.Kind != MedVar::Temp || V.Id < 0 ||
          V.Size != (I < 3 ? S.OutputSize : X86ArithmeticFlagsBytes))
        S.ValidId = false;
    }
  for (unsigned I = 0; I < Op.IntrinsicOutputs.size(); ++I) {
    if (Op.Output.Kind == MedVar::Temp &&
        Op.IntrinsicOutputs[I].Id == Op.Output.Id)
      S.ValidId = false;
    for (unsigned J = 0; J < I; ++J)
      if (Op.IntrinsicOutputs[I].Id == Op.IntrinsicOutputs[J].Id)
        S.ValidId = false;
  }
  if (Op.NumInputs > 1 && Op.Inputs[1].isConst() &&
      !isAddressProvenance(Op.Inputs[1].Provenance))
    S.Ax = Op.Inputs[1].ConstVal;
  return S;
}

inline X86SystemStateShape
x86SystemStateMedShape(const MedOp &Op, Arch TargetArch = Arch::Unknown) {
  X86SystemStateShape Shape;
  Shape.TargetArch = TargetArch;
  if (Shape.TargetArch == Arch::Unknown && Op.Output.Size &&
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Flag))
    Shape.TargetArch = Op.Output.TheArch;
  Shape.MemoryOrdering = Op.MemoryOrdering;
  Shape.MemoryAddressSpace = Op.MemoryAddressSpace;
  Shape.ValidId = Op.NumInputs && Op.NumInputs <= Op.Inputs.size() &&
                  Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
                  Op.Inputs[0].ConstVal <= UINT16_MAX &&
                  Op.IntrinsicOutputs.empty();
  if (Op.Output.Size &&
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Flag) &&
      Op.Output.TheArch != Shape.TargetArch)
    Shape.ValidId = false;
  Shape.OutputSize = Op.Output.Size;
  Shape.WritableOutput =
      (Op.Output.Kind == MedVar::Reg || Op.Output.Kind == MedVar::Temp ||
       (Op.Output.Kind == MedVar::Flag && Op.Output.Size == 1)) &&
      Op.Output.Id >= 0;
  Shape.Arguments = Op.NumInputs ? Op.NumInputs - 1 : 0;
  for (uint8_t I = 0; I < Shape.Arguments && I < Shape.InputSizes.size(); ++I) {
    const auto &Input = Op.Inputs[I + 1];
    Shape.InputSizes[I] = Input.Size;
    Shape.ScalarInputs[I] =
        isMedIntrinsicScalarInput(Input) || Input.Kind == MedVar::Param ||
        Input.Kind == MedVar::Stack || Input.Kind == MedVar::RetVal;
    // TheArch names physical registers. EA/arithmetic temporaries are neutral
    // scalars, including the canonical 64-bit address carrier used on i386.
    if (Input.Kind == MedVar::Reg && Shape.TargetArch == Arch::Unknown)
      Shape.TargetArch = Input.TheArch;
    else if (Input.Kind == MedVar::Reg && Input.TheArch != Shape.TargetArch)
      Shape.ValidId = false;
  }
  if (Shape.ValidId &&
      Op.Inputs[0].ConstVal ==
          static_cast<uint16_t>(Intrinsic::X86SegmentedBlockMemory) &&
      Op.NumInputs > 1 && Op.Inputs[1].isConst() && Op.Inputs[1].Size == 2 &&
      Op.Inputs[1].ConstVal <= UINT16_MAX)
    Shape.SegmentedBlockSelector = static_cast<uint16_t>(Op.Inputs[1].ConstVal);
  if (Shape.ValidId &&
      Op.Inputs[0].ConstVal == static_cast<uint16_t>(Intrinsic::Xabort) &&
      Op.NumInputs > 1 && Op.Inputs[1].isConst() && Op.Inputs[1].Size == 1 &&
      !isAddressProvenance(Op.Inputs[1].Provenance) &&
      Op.Inputs[1].ConstVal <= UINT8_MAX)
    Shape.TransactionAbortCode = static_cast<uint8_t>(Op.Inputs[1].ConstVal);
  return Shape;
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
  const auto IsScalar = [](const MedVar &V) {
    return isMedIntrinsicScalarInput(V) || V.Kind == MedVar::Param;
  };
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
      .IntrinsicIdIsConst = Op.NumInputs > 0 && Op.Inputs[0].isConst() &&
                            Op.IntrinsicOutputs.empty(),
      .IntrinsicIdSize = Op.NumInputs > 0 ? Op.Inputs[0].Size : uint16_t{0},
      .OutputIsWritableScalar = isMedIntrinsicWritableScalar(Op.Output),
      .OutputSize = Op.Output.Size,
      .KindIsConst = Op.NumInputs > 1 && Op.Inputs[1].isConst(),
      .Kind = Op.NumInputs > 1 ? Op.Inputs[1].ConstVal : UINT64_C(0),
      .KindSize = Op.NumInputs > 1 ? Op.Inputs[1].Size : uint16_t{0},
      .SelectorIsScalar = Op.NumInputs > 2 && IsScalar(Op.Inputs[2]),
      .SelectorIsConst = Op.NumInputs > 2 && Op.Inputs[2].isConst(),
      .SelectorSize = Op.NumInputs > 2 ? Op.Inputs[2].Size : uint16_t{0},
      .ValueIsScalar = Op.NumInputs > 3 && IsScalar(Op.Inputs[3]),
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
