//===- X86RepAddresses.h - Constant REP address facts ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original numeric addresses after normal completion of flat, natural-width
/// MOVS/STOS. These facts neither replace the instruction's actual register
/// results nor establish that its memory accesses can complete.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_X86_X86REPADDRESSES_H
#define NEVERD_IR_MED_X86_X86REPADDRESSES_H

#include "neverd/ir/med/IntrinsicShapes.h"

#include <cstdint>
#include <optional>

namespace neverd {

inline std::optional<uint64_t>
x86RepConstantAddressOutput(const MedOp &Op, const MedVar &Output, Arch A) {
  if ((A != Arch::X86 && A != Arch::X64) || Op.Opcode != NdOp::INTRINSIC ||
      !Op.NumInputs || !Op.Inputs[0].isConst())
    return std::nullopt;
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
  const bool Copy = Id == Intrinsic::X86RepMovsState;
  if (!Copy && Id != Intrinsic::X86RepStosState)
    return std::nullopt;
  const auto Shape = x86RepMedShape(Op, A);
  if (!x86RepShapeIsValid(Id, Shape))
    return std::nullopt;
  const auto Control = *x86RepControl(Id, A, *Shape.Descriptor);
  if (Control.Segment || Control.AddressBytes != Op.Output.Size ||
      !Op.Inputs[4].isConst())
    return std::nullopt;
  const unsigned Input = Output == Op.Output                             ? 1
                         : Copy && Output == Op.IntrinsicOutputs.front() ? 2
                                                                         : 0;
  const auto &Count = Op.Inputs[Copy ? 3 : 2];
  if (!Input || !Op.Inputs[Input].isConst() || !Count.isConst())
    return std::nullopt;
  const uint64_t Bytes = Count.ConstVal * Control.ElementBytes;
  const uint64_t Address = Op.Inputs[Input].ConstVal;
  const uint64_t After = (Op.Inputs[4].ConstVal >> X86RepDirectionShift) & 1
                             ? Address - Bytes
                             : Address + Bytes;
  return Op.Output.Size == 4 ? uint64_t(uint32_t(After)) : After;
}

} // namespace neverd

#endif // NEVERD_IR_MED_X86_X86REPADDRESSES_H
