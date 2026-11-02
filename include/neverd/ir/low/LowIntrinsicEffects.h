//===- LowIntrinsicEffects.h - Validate LowIR intrinsic effects -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate LowIR intrinsic effects.

#ifndef NEVERD_IR_LOW_LOWINTRINSICEFFECTS_H
#define NEVERD_IR_LOW_LOWINTRINSICEFFECTS_H

#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPredicate.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/intrinsics/IntrinsicValueEffects.h"
#include "neverd/ir/low/LowIR.h"

#include <iterator>

namespace neverd {

inline bool lowAArch64VectorShapeIsValid(const LowOp &Op) {
  using namespace aarch64_sve;
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != kLowIntrinsicIdentifierBytes ||
      Op.NumInputs > kVectorMaxOperands + 1 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      (Op.Output.Size && (!Op.Output.isReg() && !Op.Output.isTemp())))
    return false;
  VectorShape Shape;
  Shape.NumOperands = Op.NumInputs - 1;
  Shape.OutputBytes = Op.Output.Size;
  for (unsigned I = 1; I != Op.NumInputs; ++I) {
    const auto &Input = Op.Inputs[I];
    if (!Input.isReg() && !Input.isTemp() && !Input.isConst())
      return false;
    Shape.Operands[I - 1] = {
        Input.Size,
        Input.isConst() ? std::optional<uint64_t>(Input.Offset) : std::nullopt};
  }
  return isValidVectorShape(static_cast<Intrinsic>(Op.Inputs[0].Offset), Shape);
}

inline bool lowAArch64SVEPrefixShapeIsValid(const LowOp &Op) {
  return lowAArch64VectorShapeIsValid(Op) &&
         aarch64_sve::prefixOperation(
             static_cast<Intrinsic>(Op.Inputs[0].Offset))
             .has_value();
}

inline bool lowAArch64SVECPAShapeIsValid(const LowOp &Op) {
  return lowAArch64VectorShapeIsValid(Op) &&
         aarch64_sve::cpaOperation(static_cast<Intrinsic>(Op.Inputs[0].Offset))
             .has_value();
}

inline bool lowAArch64AddressMemoryShapeIsValid(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() ||
      Op.NumInputs != aarch64AddressMemoryOperandCount(
                          static_cast<Intrinsic>(Op.Inputs[0].Offset)) +
                          1 ||
      Op.Inputs[0].Size != kAddressMemoryIdentifierBytes ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      (Op.Output.Size && !Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I != Op.NumInputs; ++I)
    if (!Op.Inputs[I].isConst() && !Op.Inputs[I].isReg() &&
        !Op.Inputs[I].isTemp())
      return false;
  return aarch64AddressMemoryShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].Offset), Op.NumInputs, Op.Output.Size,
      Op.Inputs[kAddressMemoryBaseOperand + 1].Size,
      Op.Inputs[kAddressMemoryIndexOperand + 1].Size,
      Op.Inputs[kAddressMemoryWordOperand + 1].Size,
      Op.Inputs[kAddressMemoryWordOperand + 1].isConst()
          ? std::optional<uint64_t>(
                Op.Inputs[kAddressMemoryWordOperand + 1].Offset)
          : std::nullopt,
      Op.Inputs[kAddressMemoryIndexOperand + 1].isConst()
          ? std::optional<uint64_t>(
                Op.Inputs[kAddressMemoryIndexOperand + 1].Offset)
          : std::nullopt,
      Op.NumInputs == kAddressStoreOperandCount + 1
          ? Op.Inputs[kAddressMemoryDataOperand + 1].Size
          : 0,
      Op.NumInputs == kAddressStoreOperandCount + 1 &&
              Op.Inputs[kAddressMemoryDataOperand + 1].isConst()
          ? std::optional<uint64_t>(
                Op.Inputs[kAddressMemoryDataOperand + 1].Offset)
          : std::nullopt);
}

inline bool lowAArch64AddressLoadShapeIsValid(const LowOp &Op) {
  return Op.Output.Size && lowAArch64AddressMemoryShapeIsValid(Op);
}

inline bool lowAArch64WritebackMemoryShapeIsValid(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || !Op.NumInputs ||
      !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != kAddressMemoryIdentifierBytes ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      Op.NumInputs != aarch64WritebackMemoryOperandCount(
                          static_cast<Intrinsic>(Op.Inputs[0].Offset)) +
                          1 ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I != Op.NumInputs; ++I) {
    const auto &Input = Op.Inputs[I];
    if (!Input.isConst() && !Input.isReg() && !Input.isTemp())
      return false;
  }
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  if (aarch64StructureOperation(Id))
    return aarch64StructureShapeIsValid(
        Id, Op.NumInputs - 1, Op.Output.Size,
        [&](unsigned I) { return Op.Inputs[I + 1].Size; },
        [&](unsigned I) -> std::optional<uint64_t> {
          return Op.Inputs[I + 1].isConst()
                     ? std::optional<uint64_t>(Op.Inputs[I + 1].Offset)
                     : std::nullopt;
        });
  const bool Store = isAArch64WritebackStoreIntrinsic(
      static_cast<Intrinsic>(Op.Inputs[0].Offset));
  return aarch64WritebackMemoryShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].Offset), Op.NumInputs, Op.Output.Size,
      Op.Inputs[kWritebackBaseOperand + 1].Size,
      Op.Inputs[kWritebackWordOperand + 1].Size,
      Op.Inputs[kWritebackWordOperand + 1].isConst()
          ? std::optional<uint64_t>(Op.Inputs[kWritebackWordOperand + 1].Offset)
          : std::nullopt,
      Store ? Op.Inputs[kWritebackDataOperand + 1].Size : 0,
      Store && Op.Inputs[kWritebackDataOperand + 1].isConst()
          ? std::optional<uint64_t>(Op.Inputs[kWritebackDataOperand + 1].Offset)
          : std::nullopt,
      Store && isAArch64PairMemoryIntrinsic(
                   static_cast<Intrinsic>(Op.Inputs[0].Offset))
          ? Op.Inputs[kPairSecondDataOperand + 1].Size
          : 0,
      Store &&
              isAArch64PairMemoryIntrinsic(
                  static_cast<Intrinsic>(Op.Inputs[0].Offset)) &&
              Op.Inputs[kPairSecondDataOperand + 1].isConst()
          ? std::optional<uint64_t>(
                Op.Inputs[kPairSecondDataOperand + 1].Offset)
          : std::nullopt);
}

inline bool lowAArch64PAuthLoadShapeIsValid(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC ||
      Op.NumInputs != kPAuthLoadOperandCount + 1 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != sizeof(Intrinsic) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      (!Op.Inputs[1].isConst() && !Op.Inputs[1].isReg() &&
       !Op.Inputs[1].isTemp()) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return aarch64PAuthLoadShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].Offset), Op.NumInputs, Op.Output.Size,
      Op.Inputs[1].Size, Op.Inputs[2].Size,
      Op.Inputs[2].isConst() ? std::optional<uint64_t>(Op.Inputs[2].Offset)
                             : std::nullopt);
}

inline bool lowAArch64PtrueShapeIsValid(const LowOp &Op) {
  using namespace aarch64_sve;
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs != kPtrueOperandCount + 1 ||
      !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != kLowIntrinsicIdentifierBytes ||
      Op.Inputs[0].Offset != static_cast<uint64_t>(Intrinsic::A64_SvePtrue) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  return isValidPtrueShape(
      {Op.NumInputs - 1U, Op.Output.Size, Op.Inputs[1].Size, Op.Inputs[2].Size,
       Op.Inputs[1].isConst() ? std::optional<uint64_t>(Op.Inputs[1].Offset)
                              : std::nullopt,
       Op.Inputs[2].isConst() ? std::optional<uint64_t>(Op.Inputs[2].Offset)
                              : std::nullopt});
}

inline bool lowAArch64CPAShapeIsValid(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs < 3 || Op.NumInputs > 4 ||
      !Op.Inputs[0].isConst() || Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I < Op.NumInputs; ++I)
    if (!Op.Inputs[I].isReg() && !Op.Inputs[I].isTemp() &&
        !Op.Inputs[I].isConst())
      return false;
  return aarch64CPAShapeIsValid(static_cast<Intrinsic>(Op.Inputs[0].Offset),
                                Op.NumInputs, Op.Output.Size, Op.Inputs[1].Size,
                                Op.Inputs[2].Size,
                                Op.NumInputs == 4 ? Op.Inputs[3].Size : 0);
}

inline bool lowAArch64PAuthShapeIsValid(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs < 2 ||
      Op.NumInputs > kAArch64PACMOperandCount + 1 || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (unsigned I = 1; I < Op.NumInputs; ++I)
    if (!Op.Inputs[I].isReg() && !Op.Inputs[I].isTemp() &&
        !Op.Inputs[I].isConst())
      return false;
  const bool TwoModifiers =
      aarch64PAuthExtendedOperation(static_cast<Intrinsic>(Op.Inputs[0].Offset))
          .has_value();
  return aarch64PAuthShapeIsValid(
      static_cast<Intrinsic>(Op.Inputs[0].Offset),
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
                  ? std::optional<uint64_t>(Op.Inputs[2].Offset)
                  : std::nullopt});
}

/// One shared effect classification for LowIR consumers. A malformed or
/// unknown intrinsic cannot grant address-suppression or call-barrier proofs.
inline bool lowIntrinsicHasOnlyValueEffects(const LowOp &Op) {
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 ||
      Op.NumInputs > std::size(Op.Inputs) || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) || Op.Output.Size == 0 ||
      Op.MemoryOrdering != NdMemoryOrdering::None ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return false;
  for (uint8_t I = 1; I < Op.NumInputs; ++I)
    if ((!Op.Inputs[I].isReg() && !Op.Inputs[I].isTemp() &&
         !Op.Inputs[I].isConst()) ||
        Op.Inputs[I].Size == 0)
      return false;
  return intrinsicHasOnlyValueEffects(
      static_cast<Intrinsic>(Op.Inputs[0].Offset), Op.NumInputs, Op.Output.Size,
      Op.NumInputs > 1 ? Op.Inputs[1].Size : 0,
      Op.NumInputs > 2 ? Op.Inputs[2].Size : 0);
}

/// Whether \p Op is an intrinsic, in its lifted shape, that writes no memory
/// and no register but its explicit output (intrinsicWritesOnlyItsOutput).
/// It reads no value but constants, such as the register an MRS names.
inline bool lowIntrinsicWritesOnlyItsOutput(const LowOp &Op) {
  for (unsigned I = 1; I < Op.NumInputs; ++I)
    if (!Op.Inputs[I].isConst())
      return false;
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 1 &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset < static_cast<uint64_t>(Intrinsic::_Count) &&
         (Op.Output.isReg() || Op.Output.isTemp()) && Op.Output.Size != 0 &&
         Op.MemoryOrdering == NdMemoryOrdering::None &&
         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         intrinsicWritesOnlyItsOutput(
             static_cast<Intrinsic>(Op.Inputs[0].Offset));
}

/// Whether \p Op is an intrinsic that unconditionally traps: no state after
/// it, including the output an opaque lift gives it, is reached on its path.
inline bool lowIntrinsicIsUnconditionalTrap(const LowOp &Op) {
  return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs >= 1 &&
         Op.Inputs[0].isConst() && Op.Inputs[0].Size == 2 &&
         Op.Inputs[0].Offset < static_cast<uint64_t>(Intrinsic::_Count) &&
         isUnconditionalTrapIntrinsic(
             static_cast<Intrinsic>(Op.Inputs[0].Offset),
             Op.NumInputs >= 2 && Op.Inputs[1].isConst()
                 ? std::optional<uint64_t>(Op.Inputs[1].Offset)
                 : std::nullopt);
}

/// The memory operands of an intrinsic whose complete effects
/// intrinsicMemoryAccessRoles describes, checked against its LowIR shape.
/// Complete stays false for every other operation, so an unknown or malformed
/// intrinsic keeps conservative effect handling.
inline LowMemoryOperandView lowIntrinsicMemoryOperands(const LowOp &Op) {
  LowMemoryOperandView View;
  if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 ||
      Op.NumInputs > std::size(Op.Inputs) || !Op.Inputs[0].isConst() ||
      Op.Inputs[0].Size != 2 ||
      Op.Inputs[0].Offset >= static_cast<uint64_t>(Intrinsic::_Count) ||
      (!Op.Output.isReg() && !Op.Output.isTemp()) || Op.Output.Size == 0 ||
      Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    return View;
  const auto Roles =
      intrinsicMemoryAccessRoles(static_cast<Intrinsic>(Op.Inputs[0].Offset));
  if (!Roles || Op.NumInputs != Roles->NumInputs)
    return View;
  for (uint8_t I = 1; I < Op.NumInputs; ++I) {
    const NdVar &Input = Op.Inputs[I];
    const bool Operand = I == Roles->Address || I == Roles->StoredValue;
    if (Input.Size == 0 ||
        (Operand ? !Input.isReg() && !Input.isTemp() && !Input.isConst()
                 : !Input.isConst()))
      return View;
  }
  const uint64_t Width = Op.Inputs[Roles->AccessSize].Offset;
  if (Width == 0 || Width > 16 ||
      (Roles->ReturnsMemoryValue && Op.Output.Size != Width) ||
      (Roles->StoredValue && Op.Inputs[Roles->StoredValue].Size != Width))
    return View;
  View.Address = &Op.Inputs[Roles->Address];
  if (Roles->StoredValue)
    View.StoredValue = &Op.Inputs[Roles->StoredValue];
  View.AccessSize = static_cast<uint16_t>(Width);
  View.Complete = true;
  return View;
}

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWINTRINSICEFFECTS_H
